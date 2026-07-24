# Python Signaling Servers

Python ports of the MusiConnect rendezvous server, plus a test client. These
sit alongside the C++ `signaling_server.cpp` and speak the same UDP wire
protocol (see `PROTOCOL.md`). Like the C++ server, they **only introduce peers
so they can hole-punch — they never carry audio.**

There are two servers:

| File | Protocol | Peers per room | Use when |
|------|----------|----------------|----------|
| `signaling_server_python.py` | v1 | 2 (fixed) | Drop-in replacement for the C++ 1:1 server; talks to existing v1 clients. |
| `signaling_server_multipeer.py` | v2 | up to 8 | Multi-peer mesh jams; requires multipeer supported clients (sender-ID aware). |
| `multipeer_test_client.py` | v2 | — | Verifies the multi-peer server without any audio. |

> The two protocol versions are **not** interoperable: a v1 client cannot talk
> to the v2 server (the header version check rejects the mismatch), and vice
> versa. Run the server that matches your client build.

---

## 1. `signaling_server_python.py` (1:1, protocol v1)

A faithful, byte-for-byte port of `signaling_server.cpp`. It is a drop-in
replacement — existing C++ clients (`audio_transceiver_p2p`, `demo_client`)
connect to it unchanged.

### Run

```
python signaling_server_python.py            # listens on UDP 5000
python signaling_server_python.py 5000        # explicit port
```

### Behaviour

- **REGISTER** — adds the sender to a room (max 2 members) and reflects its
  public `ip:port` back in **REGISTERED** (STUN-style).
- When a room reaches 2 members, each is sent the other's endpoints in **PEER**
  (one-shot).
- **PING** — keepalive; refreshes last-seen.
- **BYE** — removes the sender from its room.
- Stale members (not seen for 30 s) are reaped; empty rooms are deleted.
- Abuse limits: `MAX_ROOMS = 1000`, and a per-source-IP REGISTER rate limit of
  20 per second.

---

## 2. `signaling_server_multipeer.py` (multi-peer, protocol v2)

Extends the server for full-mesh rooms of up to `MAX_MEMBERS` peers, and adds a
**sender ID** to the protocol.

### Run

```
python signaling_server_multipeer.py          # listens on UDP 5000
python signaling_server_multipeer.py 5000      # explicit port
```

### What changed vs. v1

- **Rooms hold up to `MAX_MEMBERS` (default 8)** members, stored in a dict keyed
  by sender ID (not by endpoint).
- **Members are keyed by sender ID (a `uint8`, 0–255)** that the user supplies
  when starting the client. This gives each peer a *stable identity*: if a
  peer's NAT remaps its port mid-session, a re-REGISTER with the same ID simply
  updates its stored endpoint instead of creating a duplicate "ghost" member.
- **Incremental mesh introduction.** When a peer joins (or its endpoint
  changes), the server sends it a **PEER** for every existing member, and sends
  each existing member a **PEER** for the newcomer. A full mesh forms as peers
  arrive. Periodic keepalive re-registers do *not* re-fire PEER — only a new or
  moved member triggers introductions — so there is no PEER spam.
- **PEER now carries the peer's sender ID**, so a client can map an endpoint to
  an ID before any audio arrives.
- **`REGISTER` carries a trailing sender-ID byte.**
- **`PROTO_VERSION` is 2**, so v1 and v2 endpoints won't silently misparse.

### Sender-ID trust model

The server treats "same ID = same peer" and lets a later REGISTER move that ID's
endpoint. This is what makes rejoin / NAT-rebind robust. The trade-off: if two
users accidentally pick the same ID, the second silently takes over the first's
slot. If you prefer to reject a same-ID-different-endpoint collision instead,
that is a small change in `handle_register` — at the cost of the rebind
robustness.

### Departure notifications

When a peer sends BYE or times out, the server simply drops it; it does **not**
notify the other members (there is no `PEER_GONE` message in the protocol).
Mesh clients must detect a dead peer via their own audio-side timeout.

---

## 3. Wire protocol

Both servers share an 8-byte header. It is a **raw little-endian** layout
(matching the C++ `Writer`/`Reader` `memcpy` convention). Endpoint fields
(`ip`/`port`) travel in **network byte order**; the client-reported LAN
`ip`/`port` are stored and re-emitted as raw bytes.

### Header (8 bytes)

| Field | Type | Notes |
|-------|------|-------|
| magic | `uint32` LE | `0x4D4A414D` (`'M','J','A','M'`) |
| version | `uint8` | `1` (1:1 server) or `2` (multi-peer server) |
| type | `uint8` | message type (see below) |
| reserved | `uint16` | `0` |

### Message types

| Value | Name | Direction |
|-------|------|-----------|
| 1 | `REGISTER` | client → server |
| 2 | `REGISTERED` | server → client |
| 3 | `PEER` | server → client |
| 4 | `PING` | client → server |
| 5 | `BYE` | client → server |
| 6 | `ERROR` | server → client |
| 10 | `PUNCH` | client → client (ignored by server) |
| 11 | `PUNCH_ACK` | client → client (ignored by server) |

### Payloads

`REGISTER` (after header):

| Field | Type | v1 | v2 |
|-------|------|----|----|
| room_len | `uint8` (1–32) | ✓ | ✓ |
| room | `room_len` bytes | ✓ | ✓ |
| local_ip | 4 bytes (net order) | ✓ | ✓ |
| local_port | 2 bytes (net order) | ✓ | ✓ |
| sender_id | `uint8` | — | ✓ |

`REGISTERED` (after header) — identical in v1 and v2:

| Field | Type |
|-------|------|
| pub_ip | 4 bytes (net order) |
| pub_port | 2 bytes (net order) |
| status | `uint8` — `0` = `REG_OK_WAITING`, `1` = `REG_ROOM_FULL` |

`PEER` (after header):

| Field | Type | v1 | v2 |
|-------|------|----|----|
| pub_ip | 4 bytes (net order) | ✓ | ✓ |
| pub_port | 2 bytes (net order) | ✓ | ✓ |
| local_ip | 4 bytes (net order) | ✓ | ✓ |
| local_port | 2 bytes (net order) | ✓ | ✓ |
| sender_id | `uint8` | — | ✓ |

`ERROR` (after header): `code : uint8` — `1` = `BAD_REQUEST`, `2` = `ROOM_FULL`,
`3` = `RATE_LIMITED`, `4` = `VERSION`.

`PING` / `BYE`: header only. The server identifies the sender by its source
endpoint.

---

## 4. Message flow (multi-peer)

```
peer A (id=0)                server                 peer B (id=1)
     |  REGISTER(room,id=0)     |                          |
     |------------------------->|                          |
     |  REGISTERED(OK)          |                          |
     |<-------------------------|                          |
     |                          |   REGISTER(room,id=1)    |
     |                          |<-------------------------|
     |                          |   REGISTERED(OK)         |
     |                          |------------------------->|
     |  PEER(id=1)              |   PEER(id=0)             |
     |<-------------------------|------------------------->|
     |                          |                          |
     |  ...both peers now hole-punch and stream directly...|
     |  PING (keepalive) ------>|<------ PING (keepalive)  |
```

A third peer joining sends its REGISTER, receives a PEER for id=0 and id=1, and
the server sends id=0 and id=1 a PEER for the newcomer — extending the mesh.

---

## 5. Configuration (tunables)

Edit the constants near the top of each server file:

| Constant | Default | Meaning |
|----------|---------|---------|
| `DEFAULT_PORT` | 5000 | UDP port if none is given on the command line |
| `MAX_ROOMS` | 1000 | Total simultaneous rooms |
| `MAX_MEMBERS` | 8 | Peers per room (multi-peer server only) |
| `MEMBER_TIMEOUT_S` | 30 | Drop members not seen for this long |
| `REG_RATE_WINDOW_S` | 1 | Rate-limit window |
| `REG_RATE_MAX` | 20 | Max REGISTERs per window per source IP |

---

## 6. Test client — `multipeer_test_client.py`

Registers several fake peers in one room against a running multi-peer server and
prints the mesh (peer endpoints + sender IDs) each one is told about. It sends
**no audio** — it only exercises signaling.

```
python multipeer_test_client.py [server_ip=127.0.0.1] [port=5000] \
       [room=jam-test] [num_peers=3]
```

Example (local server on port 5098, 3 peers):

```
python signaling_server_multipeer.py 5098
python multipeer_test_client.py 127.0.0.1 5098 jam-test 3
```

Expected output — each peer learns exactly the other peers, and it prints
`RESULT: full mesh formed for all peers`. Running with more peers than
`MAX_MEMBERS` shows the overflow peer receiving `status=ROOM_FULL` and
`ERROR code=2`.

The client exits `0` when the full mesh formed, non-zero otherwise, so it can be
used in CI.

---

## 7. Troubleshooting

**`Failed to bind on UDP port 5000`** — the server prints the underlying OS
error and a hint. The two common Windows causes:

- *Address already in use* (WinError 10048): another instance or app owns the
  port. Pick another: `python signaling_server_multipeer.py 5001`.
- *Access denied* (WinError 10013): the port falls in a reserved range
  (Hyper-V / WSL / Docker reserve dynamic ranges that can include 5000). List
  them with `netsh int ipv4 show excludedportrange protocol=udp` and choose a
  port outside those ranges.

Whatever port you pick must match on the clients (the transceiver's final
argument is the server port).

---

## 8. Notes & limitations

- The servers are single-threaded and OS-agnostic (plain UDP); nothing depends
  on the client audio backend (CoreAudio, WASAPI, etc.).
- Timeouts use a monotonic clock (`time.monotonic()`), matching the C++
  `steady_clock`.
- Endianness: assumes little-endian hosts (x86 / ARM64), same as the C++ code.
- IPv4 only.
- The server is unauthenticated beyond the room-count and per-IP REGISTER rate
  limits. Add an auth token or DTLS before exposing it widely.
