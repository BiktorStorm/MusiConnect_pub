# MusiConnect — Code Documentation

Low-latency peer-to-peer audio over a network, for musicians playing together
remotely. The goal is **minimum latency for musicians with proper audio gear**
(audio interfaces with ASIO drivers), with the network path kept as thin as
possible (raw UDP, no retransmission).

The repository holds several parallel experiments toward that goal:

| Folder | Language | Role | Latency target |
|--------|----------|------|----------------|
| `Audio_handling_test_python/` | Python | **Proof of concept** — prove the capture→network→playback loop works | ~15–60ms (WASAPI) |
| `asio_app_test/` | C++ | **The endgame** — native ASIO + CELT codec for true low latency | ~3–7ms |
| `Webapp_p2p_test/` | JS / WebRTC | Browser version — "anyone with a laptop" | ~30–60ms |
| `Server_client_system_old/` | C++ | Earlier client/server experiment (superseded) | — |

The two actively developed tracks are the **Python POC** and the **C++ ASIO
app**. This document covers those in detail and gives an overview of the rest.

> **Why ASIO?** ASIO is a Windows audio driver standard that talks almost
> directly to the hardware, bypassing the Windows audio mixer. That mixer is
> the single largest source of latency (10–50ms) on Windows. ASIO gets buffer
> sizes down to 32–96 samples (under ~2ms). It requires a dedicated audio
> interface (e.g. a Focusrite Scarlett) — laptops' built-in audio has no ASIO
> driver.

---

## Current state of the code (July 2026)

The Python implementation is mid-refactor. Be aware of this when reading:

- **`audio_sender_asio.py`** — already rewritten to the **callback-driven**
  architecture (audio thread pushes UDP packets). This is the good design.
- **`audio_receiver_asio.py`** — still the **older blocking-write** design with
  **no jitter buffer**. This is the component responsible for the distortion
  and runaway latency observed in testing.
- **`low_latency_backend.py`** — contains a `JitterBuffer` class and a
  callback-capable `open_stream()` helper, but the **receiver does not use them
  yet**. Wiring the receiver to a callback stream + `JitterBuffer` is the next
  planned step.

So the sender and receiver are currently **architecturally mismatched**. See
[Known issues & next steps](#known-issues--next-steps).

---

## Part 1 — Python proof of concept (`Audio_handling_test_python/`)

### Files

| File | Purpose |
|------|---------|
| `low_latency_backend.py` | Backend selection (ASIO/WASAPI), stream opening with fallback, `JitterBuffer` |
| `audio_sender_asio.py` | Captures audio from the interface, sends it over UDP (callback-driven) |
| `audio_receiver_asio.py` | Receives UDP audio, plays it out (blocking; older design) |
| `list_devices.py` | Diagnostic — lists audio devices and detects ASIO |
| `scarlett_test_code.py` | Standalone record-and-playback sanity check |
| `audio_sender.py` / `audio_receiver.py` | Early basic versions (superseded) |
| `audio_sender_lowlat.py` / `audio_receiver_lowlat.py` | PyAudio variants (superseded — same blocking design) |
| `sender.py` / `receiver.py` / `socket_parameters.py` | Earliest UDP-only experiments |
| `test_packet_control.py` | Test file |

### Dependencies

```
pip install sounddevice numpy
```

`sounddevice` is a thin Python wrapper over **PortAudio** (a C audio library).
PortAudio is what actually talks to ASIO / WASAPI / MME. The Python layer adds
negligible latency (microseconds) — the latency comes from buffer sizes and the
OS audio path, not from Python itself.

> **Important limitation discovered during testing:** the pip `sounddevice`
> wheel bundles a PortAudio built **without ASIO** (Steinberg's licensing keeps
> ASIO out of public redistributable builds). So on a stock install,
> `list_devices.py` reports *"NO ASIO HOST API FOUND"* even with the Focusrite
> driver correctly installed. The code therefore falls back to **WASAPI**. True
> ASIO in Python would require a custom PortAudio build — which is why the C++
> app exists.

---

### The network packet format

Both sender and receiver agree on this wire format for every UDP datagram:

```
┌────────────┬──────────────────────┬─────────────────────────────┐
│ seq (4B)   │ capture_timestamp(8B)│ PCM audio (16-bit samples)  │
│ uint32 LE  │ float64 LE           │ FRAME_SIZE × CHANNELS × 2B  │
└────────────┴──────────────────────┴─────────────────────────────┘
    HEADER_SIZE = 12 bytes                AUDIO_BYTES
```

Packed/unpacked with Python's `struct` using the format string `'<Id'`:
- `<` little-endian
- `I` unsigned 32-bit int → the **sequence number** (increments per packet)
- `d` 64-bit double → the **capture timestamp** (`time.perf_counter()` seconds)

The timestamp lets the receiver compute one-way latency (only meaningful when
sender and receiver run on the **same machine**, sharing a clock). Across two
machines, use round-trip time (RTT) / 2 instead — see the RTT echo below.

At the defaults (`FRAME_SIZE = 96`, `CHANNELS = 1`, 16-bit), each packet carries
`96 × 1 × 2 = 192` bytes of audio + 12-byte header = 204 bytes, sent 500×/second
(one every 2ms). That is ~816 kbit/s of raw PCM — trivial on a LAN.

---

### `low_latency_backend.py` — the shared backend helper

This module isolates all the messy device/backend logic so both scripts stay
clean. Four public pieces:

#### `select_backend(direction)` → dict

`direction` is `'input'` or `'output'`. It:

1. Prints the full device table (`sd.query_devices()`).
2. Detects the best available host API in priority order:
   - **ASIO** if present (lowest latency).
   - **WASAPI** otherwise → configured for **exclusive mode**
     (`sd.WasapiSettings(exclusive=True)`), which bypasses the Windows mixer for
     near-ASIO latency.
   - **default** as a last resort.
3. Lists only the devices on the chosen host API that support the requested
   direction (via `_devices_for_hostapi`, which filters on
   `max_input_channels` / `max_output_channels`).
4. Prompts the user to pick a device index (Enter = first candidate).

Returns `{'backend', 'device', 'extra_settings'}`.

> **Why a device can appear many times:** Windows exposes each physical device
> once per host API (MME, DirectSound, WASAPI, WDM-KS). So the Focusrite shows
> up ~4 times with different indices. Only the **WASAPI** copy is useful here,
> and only the one with input channels for capture / output channels for
> playback. Picking the wrong copy caused a `PaErrorCode -9996 (invalid device)`
> during testing.

#### `open_stream(direction, backend_info, **stream_kwargs)` → (stream, label)

The robust stream opener. It passes `stream_kwargs` (samplerate, channels,
dtype, blocksize, latency, callback, …) straight to `sd.InputStream` /
`sd.OutputStream`, and tries backends in order with **automatic fallback**:

- WASAPI: try **exclusive** first → if the device rejects the format, fall back
  to **shared**.
- ASIO / default: single attempt.

The fallback exists because WASAPI **exclusive mode does no format conversion**.
Requesting **mono (1ch)** on the Focusrite — whose endpoint is natively
**stereo** — is rejected in exclusive mode (this is the `-9996` we hit). The
helper then silently drops to shared mode so the POC still runs (at higher
latency). Getting exclusive mode to succeed requires matching the native format
(stereo) — see next steps.

Returns the (unstarted) stream plus a human-readable label of which backend
actually opened (e.g. `"WASAPI shared"`).

#### `make_stream(...)` → stream

A thin backward-compatible wrapper around `open_stream` for the **blocking**
(no-callback) case. The current receiver uses this.

#### `class JitterBuffer`

A thread-safe buffer that decouples the network from the audio clock. **Present
but not yet wired into the receiver.**

- `push(frame)` — the network thread appends received samples. If the buffer
  exceeds `max_ms` (default 120ms) it drops the oldest samples, bounding latency
  and counting an `overflow`.
- `pull(n)` — the audio callback requests exactly `n` samples. It **prebuffers**
  up to `target_ms` (default 25ms) before it starts returning real audio
  (`filling` flag); on underrun it returns what it has, pads with silence, counts
  an `underrun`, and re-arms prebuffering.

The prebuffer (target fill) is the core trick: it absorbs network/timing jitter
so the audio callback rarely starves. A starved callback is exactly what
produces the crackle/distortion.

---

### `audio_sender_asio.py` — capture side (callback-driven ✅)

Configuration constants at the top:
- `RECEIVER_IP` / `RECEIVER_PORT` — where to send audio (default `127.0.0.1:12345` for localhost).
- `LISTEN_PORT` — local port (12346) used to receive RTT echo packets back.
- `RATE = 48000`, `CHANNELS = 1`, `FRAME_SIZE = 96`.

Flow:

1. Create a **non-blocking** UDP socket, bind `LISTEN_PORT`, enlarge the send
   buffer (`SO_SNDBUF`).
2. `select_backend('input')` to choose the capture device.
3. Define `capture_callback(indata, frames, time_info, status)` — **the audio
   hardware calls this on its own realtime thread** each time `FRAME_SIZE`
   samples are ready. Inside it:
   - Timestamp with `time.perf_counter()`.
   - Build the 12-byte header (`seq` + timestamp) and `sock.sendto(header + indata.tobytes())`.
   - Increment `seq`, update `last_peak` for the level meter.
4. `open_stream('input', …, callback=capture_callback)` opens the stream with
   backend fallback, then `stream.start()`.
5. The **main thread** just loops printing the `[IN ]` level meter and draining
   RTT "pong" packets echoed back by the receiver, sleeping 0.2s between prints.
   All the real-time work happens in the callback.

Why callback and not a `while` loop calling `stream.read()`? A polling loop is
at the mercy of Python's scheduling — any hiccup (GC, print, GIL) makes it miss
the hardware's timing window, causing overflows. The callback runs on
PortAudio's realtime thread, tied to the hardware clock, so timing is stable.

**Level meter output** (~5×/second):
```
  [IN ] ##########                    0.312  sent=1200  RTT ~4.1ms
```
`0.000` = silence, rising number = signal present. This is the definitive proof
that real audio (not silence) is being captured and sent.

---

### `audio_receiver_asio.py` — playback side (blocking; older design ⚠️)

Configuration mirrors the sender (`RATE`, `CHANNELS`, `FRAME_SIZE` must match).
It binds `PORT = 12345` on `0.0.0.0` (accepts from any sender) with a 1-second
socket timeout.

Current flow (the problematic one):

1. `select_backend('output')`, then `make_stream(...)` — a **blocking** output
   stream (no callback).
2. Main loop:
   - `sock.recvfrom(...)` a packet.
   - Unpack header, `np.frombuffer(...).reshape(-1, CHANNELS)`.
   - Compute one-way latency from the timestamp.
   - **`stream.write(audio_array)`** — blocks until the output buffer accepts.
   - Echo the header back to the sender's `LISTEN_PORT` for RTT.
   - Print the `[OUT]` level meter (every 100 packets) and latency stats
     (every 500).

**Why this design causes distortion + runaway latency:** there is **no jitter
buffer** ("immediate playback"). The receiver plays each packet the instant it
arrives via a blocking write. Any mismatch between network arrival timing and
the audio hardware's consumption rate causes:
- **Buffer underruns** → the output device plays stale/empty buffers → audible
  crackle/distortion.
- **Queue buildup** → packets pile up in the socket + PortAudio buffers → latency
  balloons (observed ~200ms in testing).

**Level meter output:**
```
  [OUT] ##########                    0.298  (recv 1180 pkts)
```

---

### `list_devices.py` — diagnostic

Prints:
1. `sd.query_devices()` — the full device table.
2. All host APIs and their default devices.
3. Specifically whether an **ASIO host API** exists, and which devices belong to
   it. If none, prints `"NO ASIO HOST API FOUND!"` — the expected result with
   the stock pip wheel.

Run this first on any new machine to see what backends are available.

### `scarlett_test_code.py` — hardware sanity check

Records 3 seconds from a chosen device and plays it back. Useful to confirm the
interface itself works before involving the network code. (Note: its hardcoded
`device_index = 8` should be changed to match `list_devices.py` output.)

---

### Running the POC

```powershell
# 1. install deps
pip install sounddevice numpy

# 2. check what backends exist
python list_devices.py

# 3. start receiver first (so it's listening), then sender — two terminals
python audio_receiver_asio.py    # pick the Focusrite WASAPI OUTPUT device
python audio_sender_asio.py      # pick the Focusrite WASAPI INPUT device
```

For two machines (LAN): set `RECEIVER_IP` in the sender to the receiver's LAN
IP, and open UDP ports 12345/12346 in Windows Firewall:
```
netsh advfirewall firewall add rule name="MusiConnect UDP" dir=in action=allow protocol=UDP localport=12345,12346
```

**Signal chain gotcha:** the sender captures a **physical hardware input**
(e.g. Analogue 1), not "whatever plays on the computer." A sound source must be
physically connected to that input, with the interface's gain up. Verify the
interface's own input meters move before expecting the `[IN ]` meter to respond.

**Monitoring gotcha:** hearing sound from studio speakers does **not** prove the
audio went through the program — the interface may be doing hardware direct
monitoring. To test cleanly, route the interface output from **DAW/Playback
1-2** only (in Scarlett MixControl), so the only path to the speakers is
Windows → the Python receiver. Then "receiver off = silence" confirms the loop.

---

## Part 2 — C++ ASIO application (`asio_app_test/`)

The production-grade track. Achieves ~3–7ms by using ASIO directly + a
low-delay codec + raw UDP, all on realtime-priority threads.

```
asio_app_test/
├── CMakeLists.txt        # Build (fetches libopus automatically)
├── README.md             # Build & usage
├── external/asio/        # ← you place the Steinberg ASIO SDK here (not included)
└── src/
    ├── main.cpp          # Entry point — wires everything together
    ├── audio_asio.h/.cpp # ASIO driver management (capture + playout)
    ├── celt_codec.h/.cpp # CELT (Opus Custom Mode) encode/decode
    ├── network.h/.cpp    # UDP transport
    └── ring_buffer.h     # Lock-free SPSC ring buffer
```

### Data flow

```
ASIO capture callback → CELT encode → UDP send
   → UDP receive → CELT decode → playout ring buffer → ASIO playout callback
```

### `main.cpp`

Parses CLI options, initializes the four subsystems, wires the pipeline via
callbacks, then prints stats every 2 seconds until Ctrl+C.

CLI options:
```
--local-port / --remote-host / --remote-port   UDP config
--buffer-size N          ASIO buffer in samples (default 64 = 1.33ms)
--bitrate N              CELT bitrate (default 64000)
--driver NAME            ASIO driver (default: first available)
--input-channel N        Input channel to capture   (added for device selection)
--output-channel N       Output channel to play      (added for device selection)
--interactive, -i        Interactively pick driver + input/output channels (added)
--list-drivers           List ASIO drivers and exit
```

The pipeline wiring:
- **Capture callback** (runs on the ASIO realtime thread): `CELT.encode()` the
  samples, then `network.send()`.
- **Network receive callback**: detects packet loss via sequence gaps and runs
  **PLC** (packet loss concealment, up to 3 frames) via `decoder.decodePLC()`,
  then `CELT.decode()` the frame and `audio.writePlayoutSamples()` into the ring
  buffer.

It also reconciles the CELT frame size to the actual ASIO buffer size the driver
grants (they must match).

### `audio_asio.h/.cpp` — ASIO driver management

Wraps the Steinberg ASIO SDK. Key points:

- `listDrivers()` — enumerates installed ASIO drivers from the Windows registry.
- `queryChannels(driverName)` — **(added for device selection)** loads +
  initializes a driver, calls `ASIOGetChannels` / `ASIOGetChannelInfo` to read
  the input/output channel **names**, then releases the driver. Lets the app
  present a friendly channel menu (e.g. "Analogue 1", "Monitor 1") like the
  Python version's device list.
- `init(config)` — loads the driver, sets sample rate, negotiates buffer size
  (clamped to the driver's min/max), queries channel formats, creates buffers,
  reports hardware latency.
- The global `bufferSwitch()` callback is the realtime heart: it converts input
  samples to float (`asioSampleToFloat`, handling 16/24/32-bit int and 32/64-bit
  float formats), hands them to the capture callback, then pulls from the
  playout ring buffer and converts back to the hardware format
  (`floatToAsioSample`).

> The ASIO callback runs at realtime priority and **must not** block, allocate,
> lock, or do I/O. That constraint is why the ring buffer is lock-free.

**Interactive selection** (`--interactive`, added to `main.cpp`): lists drivers →
`queryChannels()` → lists input channels → lists output channels, letting the
user pick each by number. This mirrors the ease of the Python `sounddevice`
device selection but is two-tiered (driver → channel) because one ASIO driver
exposes many channels.

### `celt_codec.h/.cpp` — the low-delay codec

Wraps **libopus Custom Mode (CELT)** — the same configuration Jamulus uses for
networked music:
- Custom frame sizes (64/128/256 samples) matching the ASIO buffer.
- `OPUS_APPLICATION_RESTRICTED_LOWDELAY` — CELT only, no SILK layer, minimal
  algorithmic delay.
- Constant bitrate (CBR), low complexity (fast encode).
- `decodePLC()` — passes NULL to the decoder to synthesize a "best guess" frame
  when a packet is lost, avoiding a hard gap.

CELT is chosen over raw PCM because it drops the network payload dramatically
(64kbps vs 768kbps) while adding almost no delay — critical for internet play.

### `network.h/.cpp` — UDP transport

- Cross-platform (Winsock / POSIX) raw UDP.
- 4-byte sequence-number header (loss detection only — no reordering, no
  retransmission).
- Non-blocking socket with a dedicated receive thread that polls on a tight
  0.5–1ms timeout for low latency.
- Small socket buffers (4KB) to avoid queuing.
- Designed for **UDP hole punching** (both peers send to each other, opening the
  NAT mapping) for peer-to-peer play across the internet.
- Atomic counters for sent/received/lost stats.

### `ring_buffer.h` — lock-free SPSC ring buffer

Single-producer/single-consumer, lock-free (atomic read/write positions). Passes
audio between the network/codec thread (producer) and the ASIO callback thread
(consumer):
- **Overflow** → drops oldest samples (bounds latency).
- **Underrun** → returns zeros (silence).

This is the C++ equivalent of the Python `JitterBuffer`, but lock-free because it
runs under the realtime ASIO constraint.

### Building the C++ app

Requires a **native Windows compiler** — MSVC or **MinGW-w64**. It **cannot** be
built with WSL `g++`: WSL is Linux, produces Linux binaries, and has no access to
Windows COM or the ASIO drivers (the SDK links `ole32`/`uuid` and reads the
Windows registry). The CMakeLists currently only links `ole32`/`uuid` under
MSVC, so a MinGW build needs those link libraries added.

```powershell
# 1. Download the ASIO SDK from https://www.steinberg.net/asiosdk
#    Extract so external/asio/common/asio.h exists.
# 2. Build (native Windows shell, not WSL)
mkdir build && cd build
cmake .. -DUSE_ASIO=ON            # add -G "MinGW Makefiles" for MinGW
cmake --build . --config Release
# 3. Two instances on localhost
.\Release\musiconnect.exe --interactive --local-port 4464 --remote-port 4465
.\Release\musiconnect.exe --interactive --local-port 4465 --remote-port 4464
```

---

## Part 3 — Other subprojects (overview)

### `Webapp_p2p_test/` — browser / WebRTC

A browser-based peer-to-peer approach for the "anyone with a laptop" use case
(no special hardware). Contents: `server.js` (signaling), `client.html` /
`lowlatency.html` (clients), `playout-worklet.js` (an AudioWorklet for playout),
plus `ARCHITECTURE.md` / `SUMMARY.md`. Latency is inherently higher (~30–60ms)
because the browser routes through the OS mixer and WebRTC adds overhead — see
the ASIO-vs-browser comparison table in `asio_app_test/README.md`.

### `Server_client_system_old/` — earlier C++ experiment

A superseded client/server design in C++ (`server.cpp`, `client.cpp`,
`udpsocket`, `messagequeue`, `receiverthread`). Kept for reference; the
peer-to-peer `asio_app_test` is the current direction.

> These two are documented at overview level only. Ask if you want either one
> documented in full detail.

---

## Known issues & next steps

1. **Receiver architecture mismatch (highest priority).** The sender is
   callback-driven but the receiver still uses a blocking write with no jitter
   buffer, which causes the distortion and ~200ms latency seen in testing.
   **Next step:** rewrite the receiver to a **callback-driven output stream** fed
   by the existing `JitterBuffer` (network thread `push()`es, audio callback
   `pull()`s). This is expected to remove the distortion and bound the latency.

2. **Mono vs. WASAPI exclusive.** Exclusive mode (low latency) is rejected for a
   mono request on the stereo-native Focusrite, so the code falls back to shared
   mode (higher latency, mixer resampling). **Next step:** run the pipeline in
   **stereo (`CHANNELS = 2`)** and fix packet sizing so exclusive mode opens.

3. **No true ASIO in Python.** The pip `sounddevice`/PortAudio build lacks ASIO.
   Options: build PortAudio with the ASIO SDK, or (preferred) use the C++ app for
   the real low-latency target.

4. **Same-machine latency numbers only.** The timestamp-based one-way latency is
   only valid on one machine (shared clock). Use RTT/2 across machines.

5. **No security/auth on the UDP transport.** The receiver accepts audio from any
   sender (`0.0.0.0`). Fine for a LAN POC; a real deployment would need peer
   authentication and possibly encryption.
```
