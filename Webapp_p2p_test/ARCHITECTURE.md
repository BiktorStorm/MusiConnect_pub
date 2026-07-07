# MusiConnect WebRTC P2P Audio — Architecture

## Overview

Two browsers connect directly to each other and stream audio in real-time.
No audio passes through a server — only the initial handshake ("signaling") uses one.

```
┌──────────┐         Signaling (WebSocket)         ┌──────────┐
│ Browser A│ ←──────── offer/answer/ICE ──────────→ │ Browser B│
│          │                                        │          │
│  Mic → Opus encode → ─── UDP (direct P2P) ───→ → Opus decode → Speaker
│  Speaker ← Opus decode ← ── UDP (direct P2P) ← ← Opus encode ← Mic
└──────────┘                                        └──────────┘
                    ↑                      ↑
            Signaling server          Audio flows directly
            (only during setup)       (not through server)
```

## The Connection Flow (step by step)

### Phase 1: Signaling (via WebSocket server)

```
1. Peer A opens the page → connects to signaling server via WebSocket
2. Peer B opens the page → connects to signaling server
3. Server tells Peer A: "a second peer arrived, start the call"
4. Peer A creates an OFFER (SDP) and sends it via the server to Peer B
5. Peer B receives the offer, creates an ANSWER, sends it back via server
6. Both peers exchange ICE CANDIDATES via the server
```

### Phase 2: ICE Negotiation (finding a path)

ICE (Interactive Connectivity Establishment) tries multiple network paths:

```
Priority 1: Direct local network (both on same WiFi) — ~1ms
Priority 2: Direct via public IP (STUN discovers your public IP) — ~5-20ms  
Priority 3: Relayed through TURN server (firewall blocks direct) — ~50-100ms
```

The browser tries all candidates simultaneously and picks the fastest that works.

### Phase 3: P2P Audio (direct, no server)

Once ICE succeeds:
- Audio is captured from the microphone
- The browser's built-in Opus encoder compresses it (~20ms frames → ~150 bytes)
- Encrypted with DTLS-SRTP (mandatory in WebRTC — always encrypted)
- Sent via UDP directly to the other browser
- Decoded and played on the other end

The signaling server is NO LONGER NEEDED and can be shut down.

## Key Concepts

### SDP (Session Description Protocol)
A text blob describing what a peer can do: supported codecs (Opus, etc.),
media types (audio/video), network info. The "offer" and "answer" are both SDP.

### ICE Candidate
One possible network path to reach a peer. Examples:
- `192.168.1.5:54321` (local IP — works if both on same network)
- `84.22.103.7:12345` (public IP discovered via STUN)
- `turn-server.example.com:443` (relay — last resort)

### STUN Server
A public server that tells you "your public IP is X.X.X.X".
It doesn't relay any data — just helps with IP discovery.
Google provides free ones (`stun:stun.l.google.com:19302`).

### Why Opus is automatic
WebRTC mandates Opus support in all browsers. When you add an audio track
to a PeerConnection, the browser handles:
- Capture → encode (Opus) → packetize → encrypt → send
- Receive → decrypt → depacketize → decode (Opus) → play

You never touch raw audio or codec APIs. It's all built in.

## File Structure

```
musiconnect-webrtc/
├── server.js      — Signaling server (relays handshake messages)
├── client.html    — Browser UI + WebRTC logic (all in one file)
└── ARCHITECTURE.md
```

## Running

```bash
# Start the signaling server (serves both WebSocket and the HTML page)
cd musiconnect-webrtc
bun server.js

# Open http://localhost:3000 in two browser tabs
# Allow microphone access in both
# Audio streams directly between them
```

## Latency Breakdown

| Step | Time |
|------|------|
| Mic capture | ~3ms (browser audio pipeline) |
| Opus encode (20ms frame) | ~20ms (frame buffering, not CPU time) |
| Network (same city) | ~5-20ms |
| Jitter buffer (WebRTC internal) | ~20-40ms |
| Opus decode + playback | ~3ms |
| **Total one-way** | **~50-85ms** |

### Reducing latency further (future)
- Smaller Opus frame size (10ms instead of 20ms) — less buffering
- Disable WebRTC's jitter buffer (requires custom configuration)
- Use `AudioWorklet` for lower-level control over the audio pipeline

## Scaling Beyond 2 Peers (future)

For 3+ musicians, options:
1. **Mesh P2P**: every peer connects to every other peer (works up to ~4)
2. **SFU (Selective Forwarding Unit)**: server receives all streams, forwards 
   them without mixing (clients mix locally) — lower server CPU
3. **MCU (your C++ mixer)**: server decodes, mixes, re-encodes — one stream 
   per client regardless of participant count
