# MusiConnect — macOS (Core Audio)

Ultra low-latency peer-to-peer audio streaming for real-time music collaboration on macOS. 
 
## Project structure

```
musiconnect_macos/
├── CMakeLists.txt            — Build system (fetches libopus automatically)
├── README.md                 — Build & usage instructions
└── src/
    ├── main.cpp              — Application entry point
    ├── audio_coreaudio.h/.cpp — Core Audio device management (capture + playout)
    ├── celt_codec.h/.cpp     — CELT encoder/decoder (Opus Custom Mode)
    ├── network.h/.cpp        — UDP transport (raw, no retransmission)
    └── ring_buffer.h         — Lock-free SPSC ring buffer
```

## Prerequisites

- macOS 11+ (Big Sur or later)
- CMake 3.16+
- A C++17 compiler (Xcode Command Line Tools or full Xcode)

Install prerequisites if needed:
```bash
xcode-select --install   # Xcode Command Line Tools (includes clang, make)
brew install cmake       # or download from cmake.org
```

## Build

```bash
cd ~/Documents/MusiConnect_pub/musiconnect_macos
mkdir build && cd build
cmake ..
cmake --build . --config Release
```

CMake will automatically fetch and build libopus with custom mode support. No external SDKs needed — Core Audio is part of macOS.

## Usage

### List audio devices

```bash
./musiconnect --list-devices
```

### Run on localhost (two terminals)

```bash
# Terminal A (senderId 0):
./musiconnect --local-port 4464 --remote-port 4465 --peer 127.0.0.1 --senderId 0

# Terminal B (senderId 1):
./musiconnect --local-port 4465 --remote-port 4464 --peer 127.0.0.1 --senderId 1
```

### Run over LAN (two peers)

```bash
# Machine A (192.168.1.10, senderId 0):
./musiconnect --local-port 4464 --peer 192.168.1.20 --remote-port 4464 --senderId 0

# Machine B (192.168.1.20, senderId 1):
./musiconnect --local-port 4464 --peer 192.168.1.10 --remote-port 4464 --senderId 1
```

### Run over LAN (three peers)

```bash
# Machine A (192.168.1.10, senderId 0):
./musiconnect --local-port 4464 --peer 192.168.1.20 --peer 192.168.1.30 --remote-port 4464 --senderId 0

# Machine B (192.168.1.20, senderId 1):
./musiconnect --local-port 4464 --peer 192.168.1.10 --peer 192.168.1.30 --remote-port 4464 --senderId 1

# Machine C (192.168.1.30, senderId 2):
./musiconnect --local-port 4464 --peer 192.168.1.10 --peer 192.168.1.20 --remote-port 4464 --senderId 2
```

Each instance sends its encoded audio to all `--peer` addresses and independently decodes incoming streams from each unique sender ID.

### All options

| Flag | Default | Description |
|------|---------|-------------|
| `--local-port PORT` | 4465 | Local UDP port to bind |
| `--peer HOST` | *(none)* | Remote peer IP address (repeatable) |
| `--remote-port PORT` | 4465 | Remote peer UDP port (shared by all peers) |
| `--senderId N` | 0 | This instance's sender ID (0–255, must be unique per peer) |
| `--buffer-size N` | 64 | Audio buffer size in samples |
| `--bitrate N` | 64000 | CELT bitrate in bits/sec |
| `--list-devices` | — | List audio devices and exit |

## Microphone permissions

macOS requires microphone access. On first run, the system will prompt you to grant permission. If running from Terminal, go to **System Settings → Privacy & Security → Microphone** and ensure Terminal (or your terminal app) is enabled.

## Latency

With 64-sample buffers at 48kHz:

| Stage | Time |
|-------|------|
| Capture buffer | 1.33ms |
| CELT encode | ~0.05ms |
| Network (localhost) | ~0.1ms |
| CELT decode | ~0.05ms |
| Playout buffer | 1.33ms |
| **Total** | **~3ms** |

Core Audio provides direct hardware access on macOS — no mixer layer or sample rate conversion in the path. This is equivalent to ASIO on Windows.

Adding more peers does not increase latency — the encoder runs once regardless of peer count, and each peer's decoder runs independently.

## Packet format

```
[0..3]  uint32_t  sequence number (per-sender, for loss detection)
[4]     uint8_t   sender ID (identifies which peer sent the packet)
[5..N]  bytes     CELT encoded audio payload
```

## Cross-platform compatibility

This macOS version is wire-compatible with the Windows ASIO version. Both use:
- The same CELT codec configuration (64-sample frames, 64kbps CBR)
- The same UDP packet format (4-byte sequence + 1-byte sender ID + CELT payload)
- The same sample rate (48kHz)

A macOS instance can stream to/from a Windows instance with no changes.
