# Herman_testar_göra_server — MusiConnect P2P audio + signaling

A UDP **signaling / rendezvous server** and a **peer-to-peer audio transceiver**
that let two musicians behind home NATs find each other and stream low-latency
audio directly to one another.

The server only introduces the two peers (a "rendezvous"); it **never carries
audio**, so it adds **zero latency** to the jam. Once the peers have hole-punched
a direct UDP path, `audio_transceiver_p2p` reuses that NAT mapping and streams
PCM audio straight between the two clients.

The signaling server ships in **two languages**: the original C++
(`signaling_server.cpp`) and a Python port (`signaling_server_python.py`) that
speaks the exact same wire protocol — pick whichever you prefer to run. There is
also a **multi-peer** Python server (`signaling_server_multipeer.py`) for
full-mesh rooms of more than two peers.

See `PROTOCOL.md` for the full wire protocol and the reasoning behind it, and
`SIGNALING_PYTHON.md` for details on the Python servers.

## How it works

1. Both peers start `audio_transceiver_p2p` with the **same room code** and point
   it at the signaling server.
2. Each client registers in the room, and the server tells each peer the other's
   public `ip:port`.
3. The clients hole-punch a direct UDP path (works on most home routers).
4. The audio socket re-binds the **same local port** (`SO_REUSEADDR`) so the
   hole-punched NAT mapping is kept alive, then PortAudio streams audio directly
   peer-to-peer. The server is no longer involved.

## Files

| File | Purpose |
|------|---------|
| `PROTOCOL.md` | Wire protocol + design rationale (read this first) |
| `signaling_protocol.h` | Shared constants, cross-platform socket glue, (de)serialization |
| `signaling_server.cpp` | The rendezvous server (host this on a public IP) |
| `signaling_server_python.py` | Python port of the server (1:1, protocol v1); drop-in replacement |
| `signaling_server_multipeer.py` | Python multi-peer server (protocol v2, sender-ID aware) |
| `multipeer_test_client.py` | Signaling-only test client for the multi-peer server |
| `SIGNALING_PYTHON.md` | Documentation for the Python servers + test client |
| `signaling_client.h/.cpp` | Client: register → wait for peer → hole punch → resolved endpoint |
| `audio_transceiver_p2p.cpp` | **Full P2P audio transceiver**: signaling + hole punch + live audio (PortAudio) |
| `demo_client.cpp` | Minimal demo: resolve a peer and print the handoff (no audio) |
| `CMakeLists.txt` | Builds `signaling_server` and `demo_client` |

## Prerequisites

- A C++17 compiler (MSVC, MinGW-w64, g++, or clang++) — for the C++ server and
  the audio transceiver.
- **Python 3.6+** — only if you want to run the Python signaling servers
  (`signaling_server_python.py` / `signaling_server_multipeer.py`). They use the
  standard library only, so there is nothing to install and nothing to build.
- **PortAudio** — required only for `audio_transceiver_p2p` (the server and
  `demo_client` have no audio dependency).
  - Windows (MSYS2/MinGW): `pacman -S mingw-w64-x86_64-portaudio`
  - Linux (Debian/Ubuntu): `sudo apt install portaudio19-dev`
  - macOS (Homebrew): `brew install portaudio`

## Build

### The signaling server

The server has no audio dependencies and builds with CMake:

```
cd Herman_testar_göra_server
cmake -B build
cmake --build build
```

This produces `signaling_server` (and `demo_client`) in `build/` or
`build/Release/`.

You can also build the server directly:

```
# POSIX (Linux/macOS)
g++ -std=c++17 -O2 signaling_server.cpp -o signaling_server

# Windows (MSVC Developer Prompt)
cl /std:c++17 /EHsc signaling_server.cpp ws2_32.lib

# Windows (MinGW)
g++ -std=c++17 -O2 signaling_server.cpp -o signaling_server.exe -lws2_32
```

### The Python signaling server (no build step)

The Python servers need no compilation — they run directly on Python 3.6+ with
only the standard library:

```
python signaling_server_python.py            # 1:1 server (protocol v1)
python signaling_server_multipeer.py          # multi-peer server (protocol v2)
```

`signaling_server_python.py` is a byte-for-byte port of `signaling_server.cpp`,
so the existing C++ clients connect to it unchanged. `signaling_server_multipeer.py`
supports full-mesh rooms and a sender-ID protocol extension (v2) — it needs v2
clients. See `SIGNALING_PYTHON.md` for the wire-format details and the test
client.

### The P2P audio transceiver (`audio_transceiver_p2p`)

`audio_transceiver_p2p` is not part of the CMake targets — compile it directly
and link PortAudio (`-lportaudio`) plus the sockets library on Windows
(`-lws2_32`). It needs `signaling_client.cpp` as well.

```
# Windows (MinGW / MSYS2), one line
g++ -std=c++17 -O2 audio_transceiver_p2p.cpp signaling_client.cpp -o audio_transceiver_p2p.exe -lws2_32 -lportaudio

# Linux/macOS, one line
g++ -std=c++17 -O2 audio_transceiver_p2p.cpp signaling_client.cpp -o audio_transceiver_p2p -lportaudio
```

On Windows with MSVC, use the Developer Prompt and point at your PortAudio
headers/libs:

```
cl /std:c++17 /EHsc audio_transceiver_p2p.cpp signaling_client.cpp /I <portaudio_include> <portaudio_lib>\portaudio.lib ws2_32.lib
```

If the compiler cannot find `portaudio.h` or the PortAudio library, add the
include path (`-I/path/to/include`) and library path (`-L/path/to/lib`) for your
PortAudio installation.

## Run

### 1. Start the server (on a machine reachable by both peers)

```
./signaling_server            # listens on UDP 5000 by default
./signaling_server 5000       # explicit port
```

For a real internet test this must be a host with a public IP (e.g. a VPS) with
UDP 5000 open in the firewall. For LAN testing, any machine on the network works.

You can run the Python server instead of the C++ one — it listens the same way
and takes the same optional port argument:

```
python signaling_server_python.py 5000        # 1:1 (protocol v1)
python signaling_server_multipeer.py 5000      # multi-peer (protocol v2)
```

### 2. Run the transceiver on both peers with the SAME room code

On two different machines (or two ports on one machine for a local loopback
test):

```
./audio_transceiver_p2p <server_ip> jam-4f9a2c 12345 5000
./audio_transceiver_p2p <server_ip> jam-4f9a2c 12346 5000
```

Arguments: `<server_ip> <room_code> [local_port=12345] [server_port=5000]`.

- `<server_ip>` — address of the machine running `signaling_server`.
- `<room_code>` — any shared string; both peers must use the exact same code.
- `[local_port]` — the local UDP port used for **both** signaling and audio.
- `[server_port]` — the server's UDP port (must match how you started the server).

Each client:

1. Registers in the room and waits for the peer (Ctrl+C to abort).
2. Prints the resolved peer endpoint once hole punching succeeds
   (`CONFIRMED bidirectional path` or a `best-effort` fallback).
3. Re-binds the same local port and starts streaming audio directly to the peer.

Press Ctrl+C to shut down cleanly.

### Local loopback test (one machine)

Start the server, then run two transceivers with different local ports but the
same room code, both pointing at `127.0.0.1`:

```
./signaling_server 5000
./audio_transceiver_p2p 127.0.0.1 test-room 12345 5000
./audio_transceiver_p2p 127.0.0.1 test-room 12346 5000
```

## Cross-network (over the internet)

The LAN/loopback tests above only work when both peers can already reach the
server directly. To let peers on **different home networks** jam together, host
the signaling server on a machine with a **public IP that anyone can reach**,
then point both transceivers at that IP:

```
# On the public host (VPS, cloud VM, etc.)
./signaling_server 5000

# On each peer's machine (anywhere on the internet)
./audio_transceiver_p2p <public_server_ip> jam-4f9a2c 12345 5000
```

Because the server only introduces the two peers and never relays audio, a
small/cheap instance is enough — CPU and bandwidth needs are negligible, and it
adds no latency to the jam.

Checklist for a public deployment:

- **Public IP / DNS** — a VPS or cloud VM with a static public IP (or a domain
  name pointing at it). Peers connect to this address.
- **Open UDP port** — allow inbound **UDP 5000** (or whatever port you pass to
  `signaling_server`) in both the OS firewall and any cloud security group. This
  is UDP, not TCP.
- **Keep it running** — run the server under a supervisor (systemd, tmux, a
  container, etc.) so it survives disconnects and reboots.
- **NAT reality** — hole punching handles most home routers, but **symmetric
  NATs still won't connect** without a relay (see Status / limitations and
  PROTOCOL.md §7). Cellular/carrier-grade NAT is the common failure case.
- **Security** — the server is currently unauthenticated (only basic rate/room
  limits). Anyone who knows the IP and a room code can register. Before exposing
  it widely, add an auth token or DTLS, and consider restricting who can create
  rooms (see Status / limitations).

## Audio packet format

`audio_transceiver_p2p` uses the same wire format as `audio_transceiver.cpp`
(and the Python version):

```
[seq: uint32_t (4B)] [timestamp: double (8B)] [PCM int16 interleaved ...]
```

A bare 12-byte header with no PCM payload is an RTT (round-trip time) echo.
Default audio settings: 48 kHz, 2 channels, 96-frame (~2 ms) blocks, with a
jitter buffer (25 ms target, 120 ms max). These must match on both peers.

## Status / limitations

- Works for full-cone / restricted / port-restricted NATs (most home routers).
- **Symmetric NATs are not handled** — they need a TURN-style relay (future
  `RELAY` message; see PROTOCOL.md §7). Treat unconfirmed (`best-effort`)
  results as "may not be tight enough to jam".
- Assumes client and server share endianness (little-endian). Fine for typical
  x86/ARM64 targets.
- Server abuse limits are basic (max rooms + per-IP REGISTER rate limit). Add
  TLS/DTLS or an auth token before exposing widely if abuse is a concern.
- IPv4 only for now.
