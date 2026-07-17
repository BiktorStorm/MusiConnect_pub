# audio_transceiver.cpp — Guide

## Overview

`audio_transceiver.cpp` is a cross-platform C++ implementation of the low-latency audio transceiver. It performs the same role as `audio_transceiver.py`: capturing audio from a local microphone, sending it over UDP to a remote peer, and simultaneously receiving audio from that peer and playing it back through the local speakers. Both peers run the same program.

The C++ version is wire-compatible with the Python version — they use the same packet format and port scheme, so a C++ instance can communicate with a Python instance seamlessly.

---

## Architecture

```
┌──────────────────────── This Machine ────────────────────────┐
│                                                              │
│  [Microphone]                                                │
│       │                                                      │
│       ▼                                                      │
│  PortAudio Input Stream (capture_callback)                   │
│       │                                                      │
│       ├─── pack header + PCM ──► UDP sendto ──► [Remote]     │
│       │                                                      │
│                                                              │
│  [Remote] ──► UDP recvfrom ──► net_rx_loop thread            │
│                                      │                       │
│                                      ▼                       │
│                               JitterBuffer                   │
│                                      │                       │
│                                      ▼                       │
│                  PortAudio Output Stream (playout_callback)   │
│                                      │                       │
│                                      ▼                       │
│                               [Speakers]                     │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

### Threads

| Thread | Role |
|--------|------|
| PortAudio input (realtime) | `capture_callback` — captures a block of audio, packs it into a UDP packet, sends immediately |
| PortAudio output (realtime) | `playout_callback` — pulls the next block from the jitter buffer, writes to speaker |
| `net_rx_loop` (std::thread) | Receives UDP packets, pushes audio into the jitter buffer, echoes headers back for RTT |
| Main thread | Drains RTT replies, prints meters/stats, handles shutdown |

---

## Packet Format

Identical to the Python version:

```
Offset  Size   Field
──────  ─────  ──────────────────────────────
0       4B     seq       (uint32_t, little-endian)
4       8B     timestamp (double, little-endian — time of capture in seconds)
12      N×2B   PCM audio (int16, interleaved stereo)
```

Both versions use native byte order (little-endian on x86/ARM), so interoperability works between any combination of Windows, macOS, and Linux on standard hardware.

---

## Key Differences from audio_transceiver.py

### 1. Audio Backend

| Python | C++ |
|--------|-----|
| `sounddevice` (Python wrapper around PortAudio) + custom `low_latency_backend.py` for WASAPI exclusive / ASIO fallback | PortAudio C API directly |
| `select_backend()` with interactive ASIO → WASAPI-exclusive → shared fallback | Simple device list + device index prompt; PortAudio negotiates the best backend internally via its host API system |

The Python version has explicit logic to try WASAPI exclusive mode first and fall back to shared mode. In the C++ version, PortAudio handles this internally based on the host API of the selected device. If you select a WASAPI device, PortAudio opens it accordingly. For explicit exclusive-mode control on Windows, you could add `PaWasapiStreamInfo` — but the current implementation relies on `defaultLowInputLatency` / `defaultLowOutputLatency` which gives PortAudio freedom to choose the lowest-latency configuration the device supports.

### 2. Jitter Buffer

| Python | C++ |
|--------|-----|
| `JitterBuffer` class in `low_latency_backend.py` using NumPy arrays (`np.concatenate`, `np.zeros`) | `JitterBuffer` class using `std::vector<int16_t>` with `memcpy` and `erase` |
| `threading.Lock` | `std::mutex` with `std::lock_guard` |

The logic is identical: push frames in, pull fixed-size blocks out, prebuffer to the target fill level, drop oldest frames if the buffer exceeds the max, re-enter filling mode on underrun.

### 3. Timer

| Python | C++ |
|--------|-----|
| `time.perf_counter()` — returns seconds as a float | `std::chrono::steady_clock` — converted to `double` seconds |

Both are monotonic high-resolution clocks suitable for latency measurement. The timestamp is embedded in packets as a raw `double` — this means one-way delay measurement only works between machines with synchronized clocks (or on the same machine for testing). RTT measurement works regardless.

### 4. Sockets

| Python | C++ |
|--------|-----|
| `socket` module, works the same on all platforms | Platform-specific: Winsock (`winsock2.h` + `WSAStartup`) on Windows, POSIX sockets (`sys/socket.h`) on macOS/Linux |

The C++ version uses `#ifdef _WIN32` to select the right headers and initialization. A `socket_t` typedef, `INVALID_SOCK`, and `CLOSE_SOCKET` macro unify the API so the rest of the code is platform-agnostic.

### 5. Signal Handling / Shutdown

| Python | C++ |
|--------|-----|
| `KeyboardInterrupt` exception from the main-thread `time.sleep()` loop | `std::signal(SIGINT, ...)` sets `g_running = false`, main loop exits naturally |

The C++ version also registers `SIGTERM` on POSIX systems for clean shutdown from process managers.

### 6. No External Dependencies Beyond PortAudio

The Python version requires: `numpy`, `sounddevice`, and the custom `low_latency_backend.py`.

The C++ version requires only: PortAudio (linked at compile time). Everything else — jitter buffer, networking, metering — is self-contained in the single source file.

### 7. iOS Support (Stub)

The C++ version includes a compile-time hook for iOS:

```cpp
#if TARGET_OS_IPHONE
    extern "C" void ios_configure_audio_session();
#endif
```

On iOS, you must configure `AVAudioSession` (set category to play-and-record, activate it) before PortAudio can open streams. This is done in a separate `.mm` (Objective-C++) file that you link in when building for iOS. On macOS, Windows, and Linux this is a no-op.

The Python version has no iOS equivalent since CPython doesn't run natively on iOS.

---

## Configuration

All tunable parameters are `static const` at the top of the file:

```cpp
static const int RATE = 48000;          // Sample rate (Hz)
static const int CHANNELS = 2;          // Stereo
static const int FRAME_SIZE = 96;       // Samples per callback (2ms @ 48kHz)
static const int REMOTE_PORT = 12345;   // Port the remote peer listens on
static const int LOCAL_PORT = 12345;    // Port this machine listens on
static const int RTT_PORT = 12346;      // Port for RTT echo replies
static const int JITTER_MS = 25;        // Target prebuffer (ms)
static const int MAX_JITTER_MS = 120;   // Max buffer before dropping (ms)
```

The remote IP is entered interactively at startup — not hardcoded.

---

## Building

The included `CMakeLists.txt` handles all platforms:

```bash
# macOS
brew install portaudio
mkdir build && cd build && cmake .. && make

# Linux (Debian/Ubuntu)
sudo apt install portaudio19-dev
mkdir build && cd build && cmake .. && make

# Windows (with vcpkg)
vcpkg install portaudio
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake
cmake --build . --config Release

# Windows (manual PortAudio)
cmake .. -DPORTAUDIO_ROOT=C:/path/to/portaudio
cmake --build . --config Release
```

---

## Interoperability

Because the packet format is byte-for-byte identical:

- **C++ ↔ C++** — works across any OS combination
- **C++ ↔ Python** — works; one machine can run `audio_transceiver.py`, the other runs the compiled `audio_transceiver` binary
- **Windows ↔ macOS ↔ Linux** — any mix works as long as both sides use the same port numbers and sample format

---

## Limitations & Future Work

1. **No WASAPI exclusive mode toggle** — The Python version explicitly attempts exclusive mode first. The C++ version could add this via `PaWasapiStreamInfo` with `AUDCLNT_SHAREMODE_EXCLUSIVE`.
2. **No ASIO device selection** — On Windows with ASIO drivers installed, PortAudio exposes ASIO devices in the device list. Selecting one will use ASIO automatically, but there's no explicit ASIO-first preference logic like the Python version has.
3. **Single-file simplicity** — The jitter buffer, networking, and audio are all in one file for ease of understanding and deployment. For a production application, these would typically be split into separate modules.
4. **No packet loss concealment** — On underrun the output is zero-filled (silence). A production version could interpolate or repeat the last frame.
