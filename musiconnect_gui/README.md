# MusiConnect GUI

Cross-platform GUI for MusiConnect using Dear ImGui + GLFW + OpenGL.

Replaces the command-line interface with a graphical settings window:
- Audio device selection (dropdown)
- Remote IP / port
- Local port  
- Buffer size (32, 64, 128, 256, 512 samples)
- Bitrate selection
- Connect / Disconnect button
- Live stats while connected

## Architecture

```
┌─────────────────────────────────────────────┐
│  GUI (Dear ImGui + GLFW + OpenGL)           │  ← main thread
│  - Settings form                            │
│  - Stats display                            │
│  - Connect/Disconnect                       │
└─────────────┬───────────────────────────────┘
              │ start(config) / stop()
┌─────────────▼───────────────────────────────┐
│  AudioEngine                                │
│  - Wraps the full pipeline                  │
│  - Platform audio (ASIO / Core Audio)       │  ← realtime thread
│  - CELT codec                               │
│  - UDP transport                            │  ← network thread
└─────────────────────────────────────────────┘
```

The GUI runs at 60fps (vsync). The audio pipeline runs independently on
its own realtime thread. **Zero impact on audio latency.**

## Build (macOS)

```bash
cd musiconnect_gui
cmake -B build -S .
cmake --build build
./build/musiconnect_gui
```

## Build (Windows)

Requires Visual Studio with C++ desktop workload:

```bash
cd musiconnect_gui
cmake -B build -S .
cmake --build build --config Release
.\build\Release\musiconnect_gui.exe
```

## Dependencies (fetched automatically by CMake)

- [Dear ImGui](https://github.com/ocornut/imgui) v1.90.4 — Immediate-mode GUI
- [GLFW](https://github.com/glfw/glfw) 3.3.9 — Cross-platform windowing
- [Opus](https://github.com/xiph/opus) v1.4 — CELT codec (custom modes)

## File Structure

```
musiconnect_gui/
├── CMakeLists.txt          # Build system
├── README.md               # This file
└── src/
    ├── main.cpp            # GUI application (ImGui window)
    ├── audio_engine.h      # Engine interface
    └── audio_engine.cpp    # Pipeline wrapper (connects to existing audio code)
```

The platform-specific audio sources (`audio_coreaudio.cpp`, `audio_asio.cpp`)
are referenced from the sibling project directories. The shared code
(`celt_codec`, `network`, `ring_buffer`) is referenced from `asio_app_test_mac/src/`.
