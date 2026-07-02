Same as asio app but with JACK. Should work for linux, iOS and Windows. <br />

  Here's the adapted project structure using JACK instead of ASIO:

  jack_voip/ <br />
  ├── CMakeLists.txt          — Build system (finds JACK, fetches libopus)
  ├── README.md               — Full build & usage instructions
  └── src/
      ├── main.cpp            — Application entry point
      ├── audio_jack.h/.cpp   — JACK audio driver (capture + playout)
      ├── celt_codec.h/.cpp   — CELT encoder/decoder (Opus Custom Mode)
      ├── network.h/.cpp      — UDP transport (raw, no retransmission)
      └── ring_buffer.h       — Lock-free SPSC ring buffer

  Key differences from the ASIO version:

  - No external/ folder needed — JACK is a system library (installed via package manager)
  - JACK provides the same low-latency callback model as ASIO but is cross-platform (Linux, macOS, Windows via JACK2)
  - JACK's callback runs in a realtime thread, giving the same deterministic timing as ASIO
