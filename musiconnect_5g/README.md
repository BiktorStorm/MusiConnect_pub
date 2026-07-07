# MusiConnect 5G

Ultra low latency P2P audio streaming for live instruments over 5G.

## Latency Budget (one-way, same city)

| Stage | Time |
|-------|------|
| ASIO capture (64 samples) | 1.33ms |
| CELT encode | ~0.05ms |
| 5G network | ~5-10ms |
| CELT decode | ~0.05ms |
| ASIO playout (64 samples) | 1.33ms |
| **Total** | **~8-13ms** |

## Design Decisions

- **No jitter buffer** — packets are decoded and played immediately on arrival
- **No PLC** — lost packets are simply skipped (5G loss rate is negligible)
- **DSCP EF marking** — packets tagged for real-time QoS on 5G networks
- **CPU core pinning** — receive thread locked to core 1 with realtime priority
- **Lock-free SPSC ring buffer** — bulk memcpy, no per-sample atomics
- **Raw function pointer** for ASIO callback — no std::function overhead on RT thread
- **Minimal socket buffers** (4KB) — prevents OS-level queuing

## Build

```bash
# Requires: ASIO SDK in external/asio/, CMake 3.16+
mkdir build && cd build
cmake .. -DUSE_ASIO=ON
cmake --build . --config Release
```

## Run

```bash
# Peer A
musiconnect_5g --local-port 4464 --remote-host <PEER_B_IP> --remote-port 4465

# Peer B
musiconnect_5g --local-port 4465 --remote-host <PEER_A_IP> --remote-port 4464
```

## Options

```
--local-port PORT      Local UDP port (default: 4464)
--remote-host HOST     Remote peer IP (default: 127.0.0.1)
--remote-port PORT     Remote peer port (default: 4465)
--buffer-size N        ASIO buffer in samples (default: 64)
--bitrate N            CELT bitrate in bps (default: 64000)
--driver NAME          ASIO driver name (default: first available)
--list-drivers         List ASIO drivers and exit
```
