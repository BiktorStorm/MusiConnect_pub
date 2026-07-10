Same as asio app but with JACK. Should work for linux, iOS and Windows. <br />
  JACK VoIP — Ultra-Low Latency Music Communication <br />

  <br />
  A peer-to-peer audio application using JACK Audio, Opus CELT codec, and raw UDP transport for sub-10ms round-trip
  latency on a LAN. <br />
  <br />

  Architecture <br />

  <br />

  jack_voip/ <br />
  ├── CMakeLists.txt          — Build system (finds JACK, fetches libopus) <br />
  ├── README.md               — Full build & usage instructions <br />
  └── src/ <br />
      ├── main.cpp            — Application entry point <br />
      ├── audio_jack.h/.cpp   — JACK audio driver (capture + playout) <br />
      ├── celt_codec.h/.cpp   — CELT encoder/decoder (Opus Custom Mode) <br /> 
      ├── network.h/.cpp      — UDP transport (raw, no retransmission) <br />
      └── ring_buffer.h       — Lock-free SPSC ring buffer <br /> 
      
  <br />

  Why JACK Instead of ASIO? <br />

  <br />

  ┌───────────────────┬─────────────────────────┬──────────────────────────────────┐
  │ Feature           │ ASIO                    │ JACK                             │
  ├───────────────────┼─────────────────────────┼──────────────────────────────────┤
  │ Platform          │ Windows only            │ Linux, macOS, Windows            │
  ├───────────────────┼─────────────────────────┼──────────────────────────────────┤
  │ SDK               │ Proprietary (Steinberg) │ Open source (LGPL)               │
  ├───────────────────┼─────────────────────────┼──────────────────────────────────┤
  │ Latency model     │ Callback-based          │ Callback-based (identical)       │
  ├───────────────────┼─────────────────────────┼──────────────────────────────────┤
  │ Buffer sizes      │ 32–4096 samples         │ 16–4096 samples                  │
  ├───────────────────┼─────────────────────────┼──────────────────────────────────┤
  │ Inter-app routing │ No                      │ Yes (connect any app to any app) │
  ├───────────────────┼─────────────────────────┼──────────────────────────────────┤
  │ Installation      │ SDK download + driver   │ Package manager / installer      │
  └───────────────────┴─────────────────────────┴──────────────────────────────────┘

  <br />
  Both achieve the same ~1.33ms audio latency at 64 samples / 48kHz. JACK is cross-platform and doesn't require a
  proprietary SDK. <br />
  <br />

  Latency Budget (64 samples @ 48kHz, LAN) <br />

  <br />

  ┌───────────────────────────┬─────────────┐
  │ Component                 │ Latency     │
  ├───────────────────────────┼─────────────┤
  │ JACK capture callback     │ 1.33 ms     │
  ├───────────────────────────┼─────────────┤
  │ CELT encode (algorithmic) │ 1.33 ms     │
  ├───────────────────────────┼─────────────┤
  │ UDP send → recv (LAN)     │ ~0.1–0.5 ms │
  ├───────────────────────────┼─────────────┤
  │ CELT decode (algorithmic) │ 1.33 ms     │
  ├───────────────────────────┼─────────────┤
  │ JACK playout callback     │ 1.33 ms     │
  ├───────────────────────────┼─────────────┤
  │ TOTAL one-way             │ ~5.5 ms     │
  ├───────────────────────────┼─────────────┤
  │ TOTAL round-trip          │ ~11 ms      │
  └───────────────────────────┴─────────────┘

  <br />

  Prerequisites <br />

  <br />

  Linux (recommended) <br />

  sudo apt install libjack-jackd2-dev cmake build-essential
  # or
  sudo pacman -S jack2 cmake base-devel

  <br />

  macOS <br />

  brew install jack cmake

  <br />

  Windows <br />

  <br />

  1. Install JACK2 for Windows (https://jackaudio.org/downloads/) <br />
  2. Set environment variable: JACK_DIR=C:\Program Files\JACK2 <br />
  3. Install CMake and a C++ compiler (Visual Studio or MinGW) <br />

  <br />

  Build <br />

  <br />

  cd jack_voip
  mkdir build && cd build
  cmake ..
  cmake --build . --config Release

  (Windows)
    Build (PowerShell):

  cd jack_voip <br />
  mkdir build <br />
  cd build <br />
  cmake .. <br />
  cmake --build . --config Release <br />

  <br />

  Usage <br />

  <br />

  Test on localhost (two terminals) <br />

  <br />

  Run (PowerShell) in the subfolder build:
  Set commands: <br />
  set JACK_DEFAULT_SERVER=default <br /> 
  set PATH=C:\Program Files\JACK2 <br />  

  # Terminal A:
  .\Release\jack_voip.exe --local-port 4464 --remote-port 4465

  # Terminal B:
  .\Release\jack_voip.exe --local-port 4465 --remote-port 4464 --name jack_voip_b

  LAN (PowerShell):

  # Machine A:
  .\Release\jack_voip.exe --local-port 4464 --remote-port 4464 --remote-host 192.168.1.20

  # Machine B:
  .\Release\jack_voip.exe --local-port 4464 --remote-port 4464 --remote-host 192.168.1.10

  Start JACK on Windows:

  # Using JACK's ASIO backend (not ALSA):
  "C:\Program Files\JACK2\jackd.exe" -d portaudio -r 48000 -p 64

  Or just use QjackCtl (the GUI that comes with JACK2 for Windows) — set sample rate to 48000, frames/period to 64, and select your ASIO driver.


  Connect over LAN <br />

  <br />

  Machine A (192.168.1.10): <br />

  ./jack_voip --local-port 4464 --remote-port 4464 --remote-host 192.168.1.20

  <br />

  Machine B (192.168.1.20): <br />

  ./jack_voip --local-port 4464 --remote-port 4464 --remote-host 192.168.1.10

  <br />

  Command Line Options <br />

  <br />

  ┌───────────────┬───────────┬────────────────────────────────┐
  │ Option        │ Default   │ Description                    │
  ├───────────────┼───────────┼────────────────────────────────┤
  │ --local-port  │ 4464      │ Local UDP port to bind         │
  ├───────────────┼───────────┼────────────────────────────────┤
  │ --remote-port │ 4465      │ Remote UDP port to send to     │
  ├───────────────┼───────────┼────────────────────────────────┤
  │ --remote-host │ 127.0.0.1 │ Remote IP address              │
  ├───────────────┼───────────┼────────────────────────────────┤
  │ --bitrate     │ 128000    │ CELT bitrate in bps            │
  ├───────────────┼───────────┼────────────────────────────────┤
  │ --buffer-size │ 64        │ JACK buffer size hint (frames) │
  ├───────────────┼───────────┼────────────────────────────────┤
  │ --name        │ jack_voip │ JACK client name               │
  └───────────────┴───────────┴────────────────────────────────┘

  <br />

  JACK Server Configuration <br />

  <br />

  For minimum latency, start JACK with small buffers: Or higher buffer at a start <br />

  # 64 frames at 48kHz = 1.33ms per period
  jackd -d alsa -r 48000 -p 64 -n 2

  # Or with QjackCtl / Cadence GUI:
  # Set: Sample Rate = 48000, Frames/Period = 64, Periods/Buffer = 2

  <br />

  On Windows, use JACK's ASIO backend for the same low-latency access to audio hardware. <br />
  <br />
  Troubleshooting: <br /> 
  How to diagnose crash, fetch exit code: 
  .\Release\jack_voip.exe --local-port 4464 --remote-port 4465; Write-Host "Exit code: $LASTEXITCODE" 
  
  Why jack_voip connects then disappears

  The connection graph change followed by connection change pattern (09:16:52 → 09:16:57) suggests jack_voip registers,
  creates ports, then crashes or exits. Check the terminal where you launched jack_voip — there should be an error
  message printed before it exits. Common reasons: <br />

  1. Port mismatch — jack_voip might expect a specific number of channels that doesn't match your 18-channel Focusrite
  setup, unlikely though since problem persists without Focusrite  <br />
  2. Immediate send failure — it tries to send to the remote port immediately and crashes when nothing is listening yet <br />
  3. Sample rate or buffer size mismatch — the client expects different parameters <br />

  How It Compares to Browser-Based Solutions <br />

  <br />

 
  │ Browser (WebRTC)                         │ JACK VoIP (this app)               │
  ├ ------------- | ------------┤
  │ getUserMedia → OS mixer (10–20ms)        │ JACK callback → zero-copy (1.33ms) │
  │ AudioContext output → OS mier (25–50ms) │ Direct to DAC via JACK (1.33ms)    │
  │ WebRTC SCTP DataChannel (1–5ms)          │ Raw UDP socket (0.05ms)            │
  │ WASM decode in event loop                │ Native C++ realtime thread         │
  ├──────────────────────────────────────────┼────────────────────────────────────┤
  │ Total: 60–100ms RTT                      │ Total: ~11ms RTT                   │
  └──────────────────────────────────────────┴────────────────────────────────────┘

  <br />

  Troubleshooting <br />

  <br />

  - "Cannot initialize JACK. Is jackd running?" — Start the JACK server first (jackd or QjackCtl) <br />
  - "Failed to create custom mode" — The frame size must be supported by Opus Custom. Use powers of 2: 32, 64, 128, 256.
  <br />
  - High packet loss — Check firewall rules. Ensure UDP ports are open. <br />
  - Xruns (buffer underruns) — Increase buffer size (--buffer-size 128) or configure realtime priorities. <br />

  <br />

  License <br />

  <br />
  MIT <br />
  

  Here's the adapted project structure using JACK instead of ASIO:

  jack_voip/ <br />
  ├── CMakeLists.txt          — Build system (finds JACK, fetches libopus) <br />
  ├── README.md               — Full build & usage instructions <br />
  └── src/ <br />
      ├── main.cpp            — Application entry point <br />
      ├── audio_jack.h/.cpp   — JACK audio driver (capture + playout) <br />
      ├── celt_codec.h/.cpp   — CELT encoder/decoder (Opus Custom Mode) <br /> 
      ├── network.h/.cpp      — UDP transport (raw, no retransmission) <br />
      └── ring_buffer.h       — Lock-free SPSC ring buffer <br />

  Key differences from the ASIO version: <br />

  - No external/ folder needed — JACK is a system library (installed via package manager) <br />
  - JACK provides the same low-latency callback model as ASIO but is cross-platform (Linux, macOS, Windows via JACK2) <br />
  - JACK's callback runs in a realtime thread, giving the same deterministic timing as ASIO <br />

    Machine A (e.g., IP = 192.168.1.10) — open PowerShell:

  .\Release\jack_voip.exe --local-port 4464 --remote-port 4464 --remote-host 192.168.1.20

  Machine B (e.g., IP = 192.168.1.20) — open PowerShell:

  .\Release\jack_voip.exe --local-port 4464 --remote-port 4464 --remote-host 192.168.1.10

  Both use port 4464. Each points --remote-host at the other machine's IP. 

    ──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────

  Localhost: Two terminals on the SAME computer


  - Both instances run on your PC
  - They talk via 127.0.0.1 (loopback — no real network)
  - You need different ports (4464 and 4465) because two programs can't listen on the same port on one machine
  - You need different JACK client names (--name jack_voip_b) because JACK requires unique names
  - Useful for testing that the code works before involving a second machine

  ──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────

  LAN: Two DIFFERENT computers on the same network

 

  - Each instance runs on a separate PC (e.g., you and a bandmate)
  - They talk via real network (WiFi or Ethernet)
  - They can use the same port (4464) because they're different machines
  - No need for different names — each machine has its own JACK server
  - --remote-host points to the other person's IP address
  - This is the actual use case — playing music together over a network

  ──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────

  Quick comparison


  The localhost test lets you verify everything compiles and runs correctly before you set up the real scenario with a second musician.
