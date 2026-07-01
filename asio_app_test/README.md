 ──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────

  Project structure

  native/
  ├── CMakeLists.txt          — Build system (fetches libopus automatically) < br/>
  ├── README.md               — Full build & usage instructions <br />
  ├── external/
  │   └── asio/               — YOU place the ASIO SDK here (download from Steinberg)
  └── src/
      ├── main.cpp            — Application entry point, ties everything together
      ├── audio_asio.h/.cpp   — ASIO driver management (capture + playout)
      ├── celt_codec.h/.cpp   — CELT encoder/decoder (Opus Custom Mode)
      ├── network.h/.cpp      — UDP transport (raw, no retransmission)
      └── ring_buffer.h       — Lock-free SPSC ring buffer

  To build and run

  # 1. Download ASIO SDK from https://www.steinberg.net/asiosdk
  #    Extract to native/external/asio/

  # 2. Build
  cd native
  mkdir build
  cd build
  cmake .. -DUSE_ASIO=ON
  cmake --build . --config Release

  # 3. Test on localhost (two terminals)
  # Terminal A:
  .\Release\musiconnect.exe --local-port 4464 --remote-port 4465

  # Terminal B:
  .\Release\musiconnect.exe --local-port 4465 --remote-port 4464

  Why this achieves ~3ms instead of 60ms

  ┌─────────────────────────────────────────────────────┬────────────────────────────────────────────────────────┐ <br />
  │ What the browser does                               │ What native ASIO does                                  │ <br />
  ├─────────────────────────────────────────────────────┼────────────────────────────────────────────────────────┤ <br />
  │ getUserMedia → OS mixer → capture buffer (10-20ms)  │ Direct hardware interrupt → your callback (1.33ms)     │
  ├─────────────────────────────────────────────────────┼────────────────────────────────────────────────────────┤
  │ AudioContext output → OS mixer → speakers (25-50ms) │ Your callback → direct to DAC (1.33ms)                 │
  ├─────────────────────────────────────────────────────┼────────────────────────────────────────────────────────┤
  │ WebRTC SCTP DataChannel (1-5ms overhead)            │ Raw UDP socket (0.05ms)                                │
  ├─────────────────────────────────────────────────────┼────────────────────────────────────────────────────────┤
  │ WASM decode in JS event loop (can be delayed)       │ Native C++ on realtime-priority thread (deterministic) │
  └─────────────────────────────────────────────────────┴────────────────────────────────────────────────────────┘

  The fundamental difference: ASIO bypasses the entire Windows audio stack. There's no mixer, no shared buffer, no sample rate
  conversion — just your code talking directly to the hardware
