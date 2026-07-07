# MusiConnect WebRTC P2P Audio — Summary & Changelog

## Project Structure

```
Webapp_p2p_test/
├── server.js            — Signaling server (WebSocket relay)
├── client.html          — Simple P2P audio client (default, works everywhere)
├── lowlatency.html      — Low-latency client with configurable buffer control
├── playout-worklet.js   — AudioWorklet processor (custom ring buffer for speakers)
└── ARCHITECTURE.md      — Original architecture notes
```

## How to Run

```bash
cd Webapp_p2p_test
bun server.js
```

- `http://localhost:3000` → simple client
- `http://localhost:3000/low` → low-latency client with buffer controls

Open in two tabs or two devices. Both need mic access (HTTPS or localhost required).

---

## Part 1: Signaling Server (`server.js`)

### What it does
Relays WebRTC handshake messages between two browsers so they can establish a direct P2P connection.

### How it works
1. Runs an HTTP server on port 3000
2. Serves `client.html`, `lowlatency.html`, or `playout-worklet.js` based on the URL path
3. Accepts WebSocket connections at `/ws`
4. When two peers connect, tells the first peer to initiate a call
5. Forwards all WebSocket messages from one peer to the other (offer, answer, ICE candidates)

### Key concept
The server is **only** needed during connection setup. Once WebRTC establishes the P2P link, audio flows directly between browsers and the server could be shut down.

### Changes made
- Added route `/low` to serve `lowlatency.html`
- Added route `/playout-worklet.js` to serve the AudioWorklet file (browsers require worklets to be loaded from a URL)

---

## Part 2: Simple Client (`client.html`)

### What it does
Establishes a two-way P2P audio connection between two browsers using WebRTC.

### How it works

1. **Connect to signaling server** — opens a WebSocket to exchange setup messages
2. **Capture microphone** — `getUserMedia()` with music-optimized settings (no echo cancellation, no noise suppression, no auto gain)
3. **Create RTCPeerConnection** — the browser's WebRTC engine
4. **Exchange offer/answer** — one peer creates an "offer" (SDP), the other responds with an "answer"
5. **Exchange ICE candidates** — both peers share possible network paths to reach each other
6. **Audio flows** — once connected, Opus-encoded audio streams directly between browsers

### Key concepts
- **SDP (Session Description Protocol)** — describes what codecs and media each peer supports
- **ICE candidates** — possible IP:port combinations to try for the direct connection
- **STUN server** — helps discover your public IP (Google's `stun.l.google.com:19302`)

### Changes made
- Fixed race condition: ICE candidates now queue until remote description is set (was causing `addIceCandidate` errors)
- Added latency measurement via WebRTC data channel (ping/pong every 2 seconds)
- Added autoplay workaround (`.play()` call on user gesture)

---

## Part 3: Low-Latency Client (`lowlatency.html`)

### What it does
Same P2P connection as the simple client, but routes received audio through a custom AudioWorklet with a configurable buffer — giving you direct control over playout latency.

### Architecture

```
Remote peer's mic
    ↓
WebRTC (receives + decodes Opus automatically)
    ↓
MediaStream (decoded PCM audio)
    ↓
AudioContext.createMediaStreamSource()
    ↓
AudioWorkletNode (our custom ring buffer)
    ↓
audioCtx.destination (speakers)
```

### How it works

1. **WebRTC connection** — same as simple client (offer/answer/ICE)
2. **AudioContext** — created with `latencyHint: 'interactive'` for lowest system latency
3. **MediaStreamSource** — takes the decoded WebRTC audio stream and feeds it into Web Audio
4. **AudioWorkletNode** — routes audio through `playout-worklet.js` (our ring buffer)
5. **Output** — worklet sends buffered audio to speakers every 2.7ms (128 samples)

### Controls

| Control | What it does |
|---------|-------------|
| **Buffer Size slider** | Sets the ring buffer size (128–9600 samples = 2.7ms–200ms). Smaller = lower latency but more glitches. |
| **Bypass checkbox** | Toggles between custom worklet path and browser default playout. Use this to A/B compare latency. |

### Latency measurement
Uses a WebRTC data channel to measure network round-trip time:
- Sends a timestamped "ping" message every 2 seconds
- Other peer echoes it back as "pong"
- Calculates: `estimated audio latency = RTT/2 + AudioContext base latency + buffer size`

### Visualization
- **Pipeline stages** — light up as data flows through each part
- **Buffer bar** — shows real-time fill level (red = starving/glitching, green = healthy)
- **Stats line** — buffered ms, underrun count, total samples played

### Why NOT Insertable Streams
The initial approach used Insertable Streams (`createEncodedStreams`) to intercept raw Opus frames before decode. This would give frame-level control (drop/play decisions per packet). However, browser support is inconsistent:
- The API exists but the transform wasn't being invoked in Edge
- The newer `RTCRtpScriptTransform` standard isn't widely supported yet

The current approach (standard decode → AudioWorklet) is simpler, works in all browsers, and still gives us control over the biggest latency source: the playout buffer.

---

## Part 4: Playout Worklet (`playout-worklet.js`)

### What it does
A ring buffer that sits between decoded audio and the speakers. Its size determines the playout latency.

### How it works

The browser calls `process()` every ~2.7ms, requesting 128 samples to play.

**Writing (input arrives):**
```
If buffer is full → drop the OLDEST sample (keeps latency constant)
Write new sample into ring buffer
```

**Reading (speakers need audio):**
```
If buffer has data → output from ring buffer
If buffer is empty → output silence (underrun/glitch)
```

### Overflow strategy (buffer full)
When more audio arrives than fits, we **drop the oldest** samples. This means:
- Latency stays constant (always ≤ buffer size)
- You might miss a tiny bit of audio during a network burst
- Alternative would be to drop the NEW samples, but that could grow latency

### Underrun strategy (buffer empty)
When the speakers need audio but the buffer is empty, we **output silence**. This means:
- You hear a brief click or gap
- Latency doesn't grow (we don't "wait" for data)
- Alternative would be to stretch/repeat the last sample, which sounds smoother but adds complexity

### Runtime resizing
The buffer can be resized at any time via the slider. The worklet receives a `resize` message, reallocates the buffer, and resets the read/write positions.

---

## Latency Breakdown

| Component | Latency | Controllable? |
|-----------|---------|---------------|
| Mic capture (OS + browser) | ~3-10ms | No (hardware/OS) |
| Opus encode (sender browser) | ~20ms | Partially (frame size in SDP) |
| Network (one-way) | ~5-50ms | No (physics) |
| WebRTC jitter buffer (receiver) | ~20-80ms | No (browser internal) |
| Opus decode (receiver browser) | <1ms | No |
| **Our playout buffer** | **2.7-200ms** | **YES (the slider)** |
| OS audio output | ~3-10ms | No (hardware/OS) |

The **playout buffer** is what we control. The WebRTC jitter buffer is internal to the browser and not directly accessible — it's the next target for optimization (would require Insertable Streams or native code).

---

## Key Decisions & Tradeoffs

1. **P2P vs Server**: P2P eliminates one network hop, saving ~10-40ms. Trade: scales poorly beyond 3-4 peers.

2. **Music-optimized audio settings**: We disable echo cancellation, noise suppression, and auto gain. These are designed for voice calls and would destroy instrument audio.

3. **Drop-old-data overflow**: When buffer is full, we discard the oldest queued audio. This prioritizes real-time over completeness — musicians want to hear "now" not "100ms ago".

4. **Simple approach over complex**: Used standard WebRTC decode + AudioWorklet instead of Insertable Streams. Less control per-frame, but works reliably across browsers.

---

## Next Steps (future work)

- **Insertable Streams** — revisit when browser support stabilizes, for per-frame drop/play decisions
- **Smaller Opus frames** — manipulate SDP to request 10ms frames instead of 20ms
- **Multiple peers** — mesh topology (each peer connects to all others) for 3-4 musicians
- **TURN server** — relay fallback for restrictive corporate firewalls
- **HTTPS** — required for production (mic access on non-localhost origins)
- **Server mixing (MCU)** — for 5+ participants, use the C++ Opus mixer on a server
