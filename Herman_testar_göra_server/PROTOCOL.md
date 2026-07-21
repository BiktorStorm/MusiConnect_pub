# MusiConnect Signaling / Rendezvous Protocol

Version: 1 (draft)

This document defines the wire protocol for the MusiConnect signaling server
and its clients. The signaling server exists **only to introduce two peers to
each other**. It never carries audio. Once both peers know each other's
reachable UDP endpoint, they hole-punch and stream audio directly, peer-to-peer,
using the existing `audio_transceiver` / `UdpTransport` path. The server adds
**zero latency to the audio path**.

---

## 1. Why the server is needed (short version)

Two musicians on ordinary home internet cannot simply exchange IP addresses:

- A LAN address (`192.168.x.x`) is meaningless outside the LAN.
- The public IP is shared behind NAT; the router won't forward an arbitrary
  inbound UDP port to a specific machine.
- Even knowing the public IP, no NAT "pinhole" exists yet for that port.

The server solves this by:

1. Observing each peer's **public IP:port as seen from the internet** (this is
   the peer's NAT mapping — something the peer cannot learn on its own). This is
   the same idea as the STUN protocol.
2. Introducing the two peers in a room and telling each the other's observed
   public endpoint (and LAN endpoint, for same-network jams).
3. Letting the peers **UDP hole-punch** directly. After that the server is done.

---

## 2. Transport & key design decision

- Signaling runs over **UDP**.
- **The client uses the SAME UDP socket / same local port for signaling AND for
  audio.** This is critical: the NAT mapping the server observes must be the
  exact mapping the audio stream will use, or hole punching targets the wrong
  port. In practice the client binds its audio local port (e.g. 12345 / 4464),
  talks to the signaling server from it, hole-punches from it, then hands that
  same socket (or the same rebound port with `SO_REUSEADDR`) to the audio
  transport.

All multi-byte scalar fields that are **endpoints** (IP, port) are transmitted
in **network byte order** (as they come out of `sockaddr_in`). All other
scalar fields (magic, lengths) are transmitted in host order via raw `memcpy`,
matching the convention already used in `audio_transceiver.cpp`. Client and
server are assumed to share endianness (little-endian x86/ARM64); this is
documented as a known limitation, not a guarantee.

---

## 3. Common header (all messages)

Every datagram begins with an 8-byte header:

```
offset size field
0      4    magic     uint32   = 0x4D4A414D  ('M','J','A','M')
4      1    version   uint8    = 1
5      1    type      uint8    (see message types)
6      2    reserved  uint16   = 0
```

Datagrams that don't start with the correct magic + version are dropped.

---

## 4. Message types

```
1   REGISTER     client -> server
2   REGISTERED   server -> client
3   PEER         server -> client
4   PING         client -> server   (keepalive)
5   BYE          client -> server
6   ERROR        server -> client
10  PUNCH        client -> client   (peer-to-peer, not sent to server)
11  PUNCH_ACK    client -> client   (peer-to-peer, not sent to server)
```

### 4.1 REGISTER (1) — client → server
Client asks to join a room identified by a room code ("call sign").

```
header
1    room_len   uint8            (1..32)
N    room       char[room_len]   ASCII, not NUL-terminated
4    local_ip   uint32 (net ord) client's best-guess LAN IP, or 0 if unknown
2    local_port uint16 (net ord) client's local UDP port (== audio port)
```

### 4.2 REGISTERED (2) — server → client
Acknowledges registration and reflects the client's public endpoint (STUN-style).

```
header
4    public_ip   uint32 (net ord)  as the server observed the sender
2    public_port uint16 (net ord)  as the server observed the sender
1    status      uint8             0 = OK, waiting for peer
                                    1 = room full (already 2 peers)
```

### 4.3 PEER (3) — server → client
Sent to both peers once a room has two registered members. Delivers the *other*
peer's endpoints so hole punching can begin.

```
header
4    peer_public_ip    uint32 (net ord)
2    peer_public_port  uint16 (net ord)
4    peer_local_ip     uint32 (net ord)   0 if unknown
2    peer_local_port   uint16 (net ord)
```

### 4.4 PING (4) — client → server
Keepalive. Sent every few seconds while waiting for a peer, so the NAT mapping
stays open and the server can refresh last-seen. Header only.

### 4.5 BYE (5) — client → server
Client leaves the room. Header only. Server removes the registration.

### 4.6 ERROR (6) — server → client
```
header
1    code   uint8   (1 = bad request, 2 = room full, 3 = rate limited, 4 = version mismatch)
```

### 4.7 PUNCH (10) / PUNCH_ACK (11) — client → client
Peer-to-peer hole-punch probes. Header only. Sent directly to the candidate
endpoints received in PEER. Never sent to the server.

---

## 5. Flow

```
Peer A                         Server                         Peer B
  |                              |                              |
  |----- REGISTER(room=X) ------>|                              |
  |<---- REGISTERED(pubA) -------|                              |
  |----- PING (repeat) --------->|                              |
  |                              |<----- REGISTER(room=X) ------|
  |                              |------ REGISTERED(pubB) ----->|
  |                              |                              |
  |          (room X now has two members -> introduce)         |
  |<---- PEER(pubB, lanB) -------|------ PEER(pubA, lanA) ----->|
  |                              |                              |
  |============ direct UDP hole punch on audio port ===========|
  |----- PUNCH ------------------------------------------------>|
  |<---------------------------------------------- PUNCH -------|
  |----- PUNCH_ACK -------------------------------------------->|
  |<------------------------------------------ PUNCH_ACK -------|
  |                                                             |
  |========== HANDOFF: start audio_transceiver on ============ |
  |==========   the confirmed remote endpoint      ========== |
```

### Hole-punch details
On receiving PEER, each client builds a candidate list, in priority order:
1. `peer_local` (only tried if it looks like it might be same-LAN)
2. `peer_public`

Then for up to ~3 seconds it repeatedly (every ~50 ms):
- sends `PUNCH` to every candidate,
- reads inbound datagrams:
  - a `PUNCH` from a candidate → reply `PUNCH_ACK` to that source,
  - a `PUNCH_ACK` from a candidate → that source address is **confirmed** as the
    working remote endpoint; hole punching succeeds.

The first confirmed endpoint wins. That `ip:port` is what gets handed to the
audio transport. If nothing is confirmed within the timeout, the client falls
back to `peer_public` on a best-effort basis (works for many cone NATs even
without an ACK) and surfaces a "connection may be unreliable" state.

---

## 6. Server responsibilities & limits

- Maintain `room_code -> [member endpoints]`, max **2** members per room.
- Reflect each sender's observed public endpoint in REGISTERED.
- When a room reaches 2 members, send PEER to both.
- Drop members not seen (no PING/REGISTER) for `MEMBER_TIMEOUT` (30 s).
- Reject a 3rd registrant to a full room with ERROR(code=2).
- **Basic abuse limits** (must-have before public hosting):
  - Cap total simultaneous rooms.
  - Rate-limit REGISTER per source IP.
  - Room codes should be non-trivial to enumerate (recommend >= 6 random
    chars generated client-side; the server treats the code as opaque).
- The server is stateless about audio and never sees media.

---

## 7. NAT reality & TURN fallback (future)

- **Full-cone / restricted-cone / port-restricted NATs**: hole punching from the
  same source port generally works. This covers most home routers.
- **Symmetric NATs** (some corporate / carrier-grade NAT): the public port
  differs per destination, so the port the server observed won't match the port
  used toward the peer. Hole punching fails for these (~10-20% of cases).
- For those, the only fix is a **TURN-style relay**: a public host that both
  peers can reach, which forwards packets between them. This **adds latency**
  (an extra hop) and may break the tight-jam budget, so it should be treated as
  "connected, but possibly not tight enough to jam" and clearly labeled in the
  UI. TURN is out of scope for this draft; the protocol leaves room to add a
  `RELAY` message type later.

---

## 8. Direct / LAN mode (kept)

The existing manual-IP entry in `audio_transceiver.cpp` stays as an "advanced /
LAN" mode: same-network jams, port-forwarded hosts, and VPNs don't need the
server at all. The signaling server is the default path for the common
NAT-behind-home-router case.
