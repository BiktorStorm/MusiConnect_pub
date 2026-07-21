# Herman_testar_göra_server — MusiConnect signaling / rendezvous

A small UDP **signaling server** and a **client-side hole-punch module** that let
two musicians behind home NATs find each other and open a direct peer-to-peer
UDP path. The server only introduces peers; it **never carries audio**, so it
adds **zero latency** to the jam. Once a peer is resolved, its `ip:port` is
handed off to the existing `audio_transceiver` / `UdpTransport`.

See `PROTOCOL.md` for the full wire protocol and the reasoning behind it.

## Files

| File | Purpose |
|------|---------|
| `PROTOCOL.md` | Wire protocol + design rationale (read this first) |
| `signaling_protocol.h` | Shared constants, cross-platform socket glue, (de)serialization |
| `signaling_server.cpp` | The rendezvous server (host this on a public IP) |
| `signaling_client.h/.cpp` | Client: register → wait for peer → hole punch → resolved endpoint |
| `demo_client.cpp` | Minimal demo: resolve a peer and print the handoff (no audio) |
| `CMakeLists.txt` | Builds `signaling_server` and `demo_client` |

## Build

### With CMake (all platforms)
```
cd Herman_testar_göra_server
cmake -B build
cmake --build build --config Release
```
Produces `signaling_server` and `demo_client` (in `build/` or `build/Release/`).

### Directly

POSIX (Linux/macOS):
```
g++ -std=c++17 -O2 signaling_server.cpp -o signaling_server
g++ -std=c++17 -O2 demo_client.cpp signaling_client.cpp -o demo_client
```

Windows (MSVC Developer Prompt):
```
cl /std:c++17 /EHsc signaling_server.cpp ws2_32.lib
cl /std:c++17 /EHsc demo_client.cpp signaling_client.cpp ws2_32.lib
```

Windows (MinGW):
```
g++ -std=c++17 -O2 signaling_server.cpp -o signaling_server.exe -lws2_32
g++ -std=c++17 -O2 demo_client.cpp signaling_client.cpp -o demo_client.exe -lws2_32
```

## Run

### 1. Start the server (on a machine reachable by both peers)
```
./signaling_server            # listens on UDP 5000 by default
./signaling_server 5000       # explicit port
```
For a real internet test this must be a host with a public IP (VPS) and UDP
5000 open in the firewall. For LAN testing, any machine on the network works.

### 2. Run two clients with the SAME room code
On two different machines (or two ports on one machine for a local loopback test):
```
./demo_client <server_ip> jam-4f9a2c 12345 5000
./demo_client <server_ip> jam-4f9a2c 12346 5000
```
Arguments: `<server_ip> <room_code> [local_port=12345] [server_port=5000]`.

Each client prints the public endpoint the server observed, then the resolved
peer endpoint after hole punching, then the exact handoff snippet.

### 3. Hand off to audio
The resolved `ip:port` goes straight into your audio path — bind the audio
socket to the **same `local_port`** you gave the client:
```cpp
NetworkConfig cfg;
cfg.localPort  = 12345;          // same port used for signaling
cfg.remoteHost = peer.ip;        // from ResolvedPeer
cfg.remotePort = peer.port;
UdpTransport t; t.init(cfg); t.start();
```

## Integrating into the app

Add `signaling_client.cpp` to your audio target and:
```cpp
#include "signaling_client.h"

SignalingClient sig(localPort);
ResolvedPeer peer;
if (sig.resolve(serverIp, serverPort, roomCode, peer) == ResolveResult::Success) {
    // start UdpTransport / audio_transceiver on localPort toward peer.ip:peer.port
}
```
`resolve()` closes its socket before returning so the audio transport can bind
the same port (both use `SO_REUSEADDR`). For zero-gap handoff you can adapt it
to keep the socket and call `take_socket()`.

## Status / limitations (stub)

- Works for full-cone / restricted / port-restricted NATs (most home routers).
- **Symmetric NATs are not handled** — they need a TURN-style relay (future
  `RELAY` message; see PROTOCOL.md §7). Treat unconfirmed (`best-effort`)
  results as "may not be tight enough to jam".
- Assumes client and server share endianness (little-endian). Fine for typical
  x86/ARM64 targets.
- Server abuse limits are basic (max rooms + per-IP REGISTER rate limit). Add
  TLS/DTLS or an auth token before exposing widely if abuse is a concern.
- IPv4 only for now.
