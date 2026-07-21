# MusiConnect GUI — Ultra Low Latency P2P Audio with Desktop Interface

A graphical front-end for the MusiConnect WASAPI audio engine. The GUI runs as a
separate Python process communicating with the C++ audio engine via JSON over
stdin/stdout — ensuring zero interference with the realtime audio pipeline.

## Architecture

```
┌─────────────────────┐       JSON stdin/stdout       ┌──────────────────────┐
│   PyQt6 GUI         │ ◄──────────────────────────── │  musiconnect.exe     │
│   (Python process)  │                               │  --json-mode         │
│                     │                               │                      │
│  • IP address bar   │  ───► {"cmd":"start",...}     │  [WASAPI threads]    │
│  • Device selectors │  ◄─── {"type":"stats",...}    │  [Network thread]    │
│  • Stats display    │                               │  [Codec]             │
└─────────────────────┘                               └──────────────────────┘
```

## Project Structure

```
wasapi_voip_gui/
├── engine/                 # C++ audio engine (builds musiconnect.exe)
│   ├── CMakeLists.txt
│   └── src/
│       ├── main.cpp        # Entry point with --json-mode support
│       ├── audio_wasapi.cpp/h
│       ├── celt_codec.cpp/h
│       ├── network.cpp/h
│       └── ring_buffer.h
├── gui/                    # Python GUI application
│   ├── main.py             # Entry point
│   ├── engine_bridge.py    # Subprocess management + JSON protocol
│   ├── ui/
│   │   ├── main_window.py
│   │   ├── connection_panel.py
│   │   ├── device_panel.py
│   │   └── stats_panel.py
│   ├── assets/
│   │   └── style.qss       # Custom stylesheet (CSS-like theming)
│   └── requirements.txt
└── README.md
```

## Requirements

### Engine (C++)
- Windows 10 or later
- CMake 3.16+
- C++17 compiler (MSVC 2019+ or MinGW)

### GUI (Python)
- Python 3.9+
- PyQt6

## Build & Run

```powershell
# 1. Build the C++ engine
cd engine
mkdir build && cd build
cmake ..
cmake --build . --config Release

# 2. Install Python dependencies
cd ../../gui
pip install -r requirements.txt

# 3. Run the GUI
python main.py
```

## Styling

The GUI uses Qt Style Sheets (QSS) — a CSS-like language that supports:
- Colors, gradients, borders, border-radius
- Hover/pressed/disabled states
- Font styling, padding, margins
- Custom widget pseudo-elements (::handle, ::groove, etc.)

Edit `gui/assets/style.qss` to customize the look.
