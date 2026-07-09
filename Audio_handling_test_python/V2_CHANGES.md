# Audio v2 — callback + jitter buffer + stereo/WASAPI-exclusive

New files added to `Audio_handling_test_python/`:

- **`audio_sender_v2.py`**
- **`audio_receiver_v2.py`**

They reuse the existing `low_latency_backend.py` (the `select_backend`,
`open_stream`, and `JitterBuffer` pieces). The original `audio_sender_asio.py`
and `audio_receiver_asio.py` are left untouched so you can compare / fall back.

These v2 files target the two dominant causes of the ">100ms latency + awful
audio quality" you observed:

1. The old receiver played every packet immediately with a **blocking write and
   no jitter buffer** → buffer underruns (distortion) and queue buildup (latency).
2. The pipeline ran in **mono**, which forced **WASAPI shared mode** (the Windows
   mixer adds latency and resamples) because exclusive mode rejects a mono
   request on the stereo-native Focusrite.

---

## What changed, and why

### 1. Callback-driven playout (the big quality fix)

**Old receiver:** a Python `while` loop did `recvfrom()` then a blocking
`stream.write()`. Python's scheduling (GIL, prints, GC) can't hit the hardware's
2ms timing window reliably, so the output device starves → **underruns** =
crackle/distortion. Packets also queue up → latency grows to ~200ms.

**v2 receiver:** the audio hardware drives playback via a **callback** running on
PortAudio's realtime thread. It asks for a block of samples exactly when the
hardware needs it, and we hand it audio from a buffer. Python timing no longer
gates the audio clock.

### 2. Jitter buffer (decouples network from audio)

A `JitterBuffer` (already in `low_latency_backend.py`) sits between the two
threads:

```
 network thread                 jitter buffer                 audio callback
 recvfrom() ── push(frame) ──▶  [ ~25ms of audio ]  ── pull(n) ──▶ outdata
```

- The network thread **pushes** received frames in (irregular timing).
- The audio callback **pulls** fixed blocks out (perfectly regular timing).
- It **prebuffers** `JITTER_MS` of audio before starting playback, so brief
  network/timing jitter doesn't starve the output. This is what removes the
  distortion.
- On **underrun** it outputs silence and re-buffers (counted as `under=`).
- It caps at `MAX_JITTER_MS` by dropping oldest samples (counted as `over=`), so
  latency can never balloon the way it did before.

Verified behavior (unit-tested): returns silence until the prebuffer target is
reached, then real audio; fill is hard-capped at the max and overflows are
counted.

### 3. Stereo → unlocks WASAPI exclusive (the latency fix)

Both v2 files use `CHANNELS = 2`. WASAPI **exclusive mode does no format
conversion**, so a mono request on the stereo-native Focusrite was rejected and
the code fell back to **shared mode** (Windows mixer: extra latency + resampling
artifacts). Matching the native **stereo** format lets `open_stream()` succeed in
**exclusive mode**, which bypasses the mixer entirely → lower latency and no
resampling. If exclusive still can't open on your machine, `open_stream()`
automatically falls back to shared so it always runs — check the printed
`Backend:` line to see which you got.

---

## Architecture (v2)

```
                          SENDER v2
   Scarlett input ──▶ audio callback (realtime thread)
                          │  struct.pack header + PCM
                          ▼
                       UDP sendto ───────────────┐
                                                  │  network
                          RECEIVER v2             ▼
                    ┌── net_loop thread: recvfrom ─── parse ─── jitter.push()
                    │                                              │
   Scarlett output ◀── audio callback: jitter.pull() ◀────────────┘
                       (realtime thread)
```

- **Sender:** one audio callback (sends), main thread prints the `[IN ]` meter
  and drains RTT pongs.
- **Receiver:** one network thread (`push`), one audio callback (`pull`), main
  thread prints the `[OUT]` meter + buffer/latency/under/over stats.

### Packet format (unchanged)

```
[ seq: uint32 LE (4B) ][ capture_timestamp: float64 LE (8B) ][ PCM int16 interleaved ]
```

Stereo means the PCM payload is now `FRAME_SIZE × 2 channels × 2 bytes`
(interleaved L,R,L,R…). The jitter buffer reshapes it to `(n, 2)`.

> Note: because the jitter buffer stores a continuous sample stream, the
> sender's and receiver's block sizes don't strictly have to match — the buffer
> hands the output callback whatever block size it asks for. They're both set to
> `FRAME_SIZE = 96` for simplicity.

---

## Tuning — the one dial that matters

In `audio_receiver_v2.py`:

```python
JITTER_MS = 25        # target prebuffer. THE latency <-> robustness dial.
MAX_JITTER_MS = 120   # hard cap so latency can't run away.
```

- **Lower `JITTER_MS`** → less latency, but more `under=` (dropouts) if the
  network/timing is jittery.
- **Raise `JITTER_MS`** → rock-solid audio, but more latency.

Method: start at 25ms. Watch the receiver's `under=` counter while playing.
- If `under=` keeps climbing → raise `JITTER_MS` until it stops.
- If `under=` stays at 0 → lower `JITTER_MS` (e.g. 15, 10) until dropouts appear,
  then back off a touch.

Other knobs: `FRAME_SIZE` (smaller = lower latency, more CPU/underrun risk) and
`RATE`. Keep sender and receiver identical on `RATE`, `CHANNELS`, `FRAME_SIZE`.

---

## How to run

```powershell
cd C:\Users\ehjseto\MusiConnect_pub\Audio_handling_test_python

# start the receiver first, then the sender (two terminals)
python audio_receiver_v2.py     # pick the Focusrite WASAPI OUTPUT device
python audio_sender_v2.py       # pick the Focusrite WASAPI INPUT device
```

Reading the receiver meter line:
```
  [OUT] ####  0.298  recv=1180  buf= 24ms  1way~ 1ms  outHW= 10ms  e2e~ 35ms(+inHW)  under=0 over=0
         │    │      │          │           │          │           │                  │       └ latency capped (old audio dropped)
         │    │      │          │           │          │           │                  └ buffer starved (raise JITTER_MS)
         │    │      │          │           │          │           └ estimated end-to-end (see below)
         │    │      │          │           │          └ output hardware buffer (PortAudio negotiated)
         │    │      │          │           └ network one-way (capture-callback -> arrival)
         │    │      │          └ current buffer fill in ms (should hover near JITTER_MS)
         │    │      └ packets received
         │    └ peak level (0.000 = silence)
         └ level bar
```

### Latency budget instrumentation (added)

Because `1way~` only measures capture-callback → network-arrival, it hides the
two biggest terms. v2 now also reports the **actual hardware buffers** that
PortAudio negotiated, via `stream.latency`:

- **Sender startup** prints `Input HW buffer: X ms` — the capture-side buffer.
  This is large in WASAPI **shared** mode (Windows mixer) and small in
  **exclusive**/ASIO.
- **Receiver startup + meter** prints `outHW = Y ms` — the playout-side buffer.

The receiver also shows an **estimated end-to-end**:
```
e2e ~= 1way + jitter fill + output HW buffer
```
It is labelled `(+inHW)` because this process cannot see the *sender's* input
hardware buffer. For the full capture→ear latency, **add the sender's printed
`Input HW buffer` value**:

```
full latency  ≈  sender Input HW buffer          (capture side)
              +  ~1ms network
              +  jitter fill (~JITTER_MS)
              +  receiver Output HW buffer        (playout side)
```

This makes it obvious where the latency lives. Example finding during testing:
receiver opened **exclusive** (small `outHW`) but the laptop mic array sender
fell back to **shared** (large input HW buffer) — so the *input* path dominated.
On a stereo-native interface (Focusrite) both sides open exclusive and both HW
buffers shrink.

Reading the sender meter line:
```
  [IN ] ####  0.312  sent=1200  RTT ~2.0ms
         │    │      │           └ round-trip time (echoed headers), i.e. ~2 x network one-way
         │    │      └ packets sent
         │    └ peak level of captured audio (0.000 = silence)
         └ level bar
```
(The sender's `Input HW buffer` is printed once at startup.)

Goal state: `buf` hovers near `JITTER_MS`, `under`/`over` stay low, and the audio
is clean. Then lower `JITTER_MS` toward your dropout threshold.

---

## Expected result vs. the old scripts

| | old `_asio.py` | new `_v2.py` |
|---|---|---|
| Playout | blocking write, no buffer | callback + jitter buffer |
| Distortion (underruns) | frequent | rare / tunable |
| Latency | ballooned to ~200ms | bounded, ~`JITTER_MS` + HW buffers |
| Channels | mono | stereo |
| WASAPI mode | shared (mixer) | exclusive when available |

This is still the **Python POC**. The absolute latency floor (~3–7ms) needs true
ASIO, which in practice means the C++ app (`asio_app_test/`) — the pip
`sounddevice`/PortAudio build can't see ASIO. v2 is the best achievable in pure
Python and should give clean audio at a controllable, much lower latency.

---

## Verification done

- `python -m py_compile audio_sender_v2.py audio_receiver_v2.py low_latency_backend.py` → OK.
- `JitterBuffer` unit-checked: silence during prebuffer, real audio after the
  25ms target, fill hard-capped at 120ms with overflow counting.
- Not yet run against live hardware — the meters (`[IN ]`, `[OUT]`, `buf`,
  `under`, `over`) are there so you can confirm behavior on the Scarlett.
