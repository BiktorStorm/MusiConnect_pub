Same as asio app but with JACK. Should work for linux, iOS and Windows. <br />

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
