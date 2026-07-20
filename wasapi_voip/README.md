# MusiConnect — Windows WASAPI Version

Ultra low-latency peer-to-peer audio over UDP, using WASAPI exclusive mode on Windows.

## Requirements

- Windows 10 or later
- CMake 3.16+
- A C++17 compiler (MSVC 2019+ or MinGW)
- Git (for FetchContent to download Opus)

## Build

```powershell
cd wasapi_voip
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

## Usage

```powershell
# List audio devices
.\Release\musiconnect.exe --list-devices

# Run two instances on localhost for testing:
# Terminal A:
.\Release\musiconnect.exe --local-port 4464 --remote-port 4465

# Terminal B:
.\Release\musiconnect.exe --local-port 4465 --remote-port 4464

# Connect to a remote peer:
.\Release\musiconnect.exe --local-port 4464 --remote-host 192.168.1.50 --remote-port 4464
```

## Options

| Flag | Default | Description |
|------|---------|-------------|
| `--local-port` | 4464 | UDP port to listen on |
| `--remote-host` | 127.0.0.1 | Peer's IP address |
| `--remote-port` | 4465 | Peer's UDP port |
| `--buffer-size` | 64 | Audio buffer size in samples (lower = less latency) |
| `--bitrate` | 64000 | CELT codec bitrate in bps |
| `--list-devices` | — | List audio devices and exit |

## Architecture

```
[Microphone] → WASAPI capture thread → CELT encode → UDP send → network
                                                                    ↓
network → UDP receive → CELT decode → ring buffer → WASAPI render thread → [Speaker]
```

- **WASAPI exclusive mode**: bypasses Windows audio engine for direct hardware access
- **MMCSS Pro Audio**: capture and render threads run at realtime priority
- **CELT codec**: Opus custom mode with zero algorithmic delay
- **Lock-free ring buffer**: thread-safe playout buffering

## Latency

With 64-sample buffers at 48kHz on a LAN:
- Capture buffer: ~1.3ms
- Encode: ~0.05ms
- Network (LAN): ~0.3ms
- Decode: ~0.05ms
- Playout buffer: ~1.3ms
- **Total: ~3ms one-way**

Note: WASAPI exclusive mode may not achieve 64 samples on all hardware.
The system will report the actual buffer size negotiated with your audio device.
