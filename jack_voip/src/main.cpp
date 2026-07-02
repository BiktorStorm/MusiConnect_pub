// main.cpp — JACK VoIP application entry point
  // Ties together: JACK audio → CELT encode → UDP send
  //                UDP recv → CELT decode → JACK playout
  //
  // Usage:
  //   jack_voip --local-port 4464 --remote-port 4465 [--remote-host 127.0.0.1]
  //             [--bitrate 128000] [--buffer-size 64]

  #include "audio_jack.h"
  #include "celt_codec.h"
  #include "network.h"
  #include "ring_buffer.h"

  #include <cstdio>
  #include <cstring>
  #include <csignal>
  #include <atomic>
  #include <string>
  #include <chrono>
  #include <thread>
  #include <vector>

  // ─── Global state ──────────────────────────────────────────────────────────────
  static std::atomic<bool> gRunning{true};

  void signalHandler(int) {
      gRunning.store(false, std::memory_order_release);
  }

  // ─── Command line parsing ──────────────────────────────────────────────────────
  struct AppConfig {
      uint16_t    localPort   = 4464;
      uint16_t    remotePort  = 4465;
      std::string remoteHost  = "127.0.0.1";
      int         bitrate     = 128000;
      uint32_t    bufferSize  = 64;      // JACK frames per period
      std::string clientName  = "jack_voip";
  };

  static AppConfig parseArgs(int argc, char* argv[]) {
      AppConfig cfg;
      for (int i = 1; i < argc; ++i) {
          std::string arg = argv[i];
          if (arg == "--local-port" && i + 1 < argc)
              cfg.localPort = static_cast<uint16_t>(std::stoi(argv[++i]));
          else if (arg == "--remote-port" && i + 1 < argc)
              cfg.remotePort = static_cast<uint16_t>(std::stoi(argv[++i]));
          else if (arg == "--remote-host" && i + 1 < argc)
              cfg.remoteHost = argv[++i];
          else if (arg == "--bitrate" && i + 1 < argc)
              cfg.bitrate = std::stoi(argv[++i]);
          else if (arg == "--buffer-size" && i + 1 < argc)
              cfg.bufferSize = static_cast<uint32_t>(std::stoi(argv[++i]));
          else if (arg == "--name" && i + 1 < argc)
              cfg.clientName = argv[++i];
          else if (arg == "--help" || arg == "-h") {
              printf("Usage: jack_voip [options]\n"
                     "  --local-port  PORT   Local UDP port (default: 4464)\n"
                     "  --remote-port PORT   Remote UDP port (default: 4465)\n"
                     "  --remote-host HOST   Remote IP (default: 127.0.0.1)\n"
                     "  --bitrate     BPS    CELT bitrate (default: 128000)\n"
                     "  --buffer-size FRAMES JACK buffer size hint (default: 64)\n"
                     "  --name        NAME   JACK client name (default: jack_voip)\n"
                     "  --help, -h           Show this help\n");
              exit(0);
          }
      }
      return cfg;
  }

  // ─── Main ──────────────────────────────────────────────────────────────────────
  int main(int argc, char* argv[]) {
      signal(SIGINT, signalHandler);
      signal(SIGTERM, signalHandler);

      AppConfig cfg = parseArgs(argc, argv);

      printf("======================================================\n");
      printf("         JACK VoIP - Ultra-Low Latency\n");
      printf("======================================================\n");
      printf("  Local port:  %u\n", cfg.localPort);
      printf("  Remote:      %s:%u\n", cfg.remoteHost.c_str(), cfg.remotePort);
      printf("  Bitrate:     %d bps\n", cfg.bitrate);
      printf("  Buffer:      %u frames\n", cfg.bufferSize);
      printf("======================================================\n\n");

      // ─── Initialize JACK ───────────────────────────────────────────────────────
      AudioJack jack;
      JackConfig jackCfg;
      jackCfg.client_name = cfg.clientName;
      jackCfg.buffer_size = cfg.bufferSize;

      if (!jack.init(jackCfg)) {
          fprintf(stderr, "FATAL: Cannot initialize JACK. Is jackd running?\n");
          return 1;
      }

      // ─── Initialize CELT Codec ─────────────────────────────────────────────────
      CeltCodec codec;
      CeltConfig celtCfg;
      celtCfg.sampleRate = static_cast<int>(jack.get_sample_rate());
      celtCfg.frameSize  = static_cast<int>(jack.get_buffer_size());
      celtCfg.bitrate    = cfg.bitrate;

      if (!codec.init(celtCfg)) {
          fprintf(stderr, "FATAL: Cannot initialize CELT codec\n");
          return 1;
      }

      // ─── Initialize Network ────────────────────────────────────────────────────
      UdpTransport network;
      NetworkConfig netCfg;
      netCfg.localPort  = cfg.localPort;
      netCfg.remotePort = cfg.remotePort;
      netCfg.remoteHost = cfg.remoteHost;

      if (!network.init(netCfg)) {
          fprintf(stderr, "FATAL: Cannot initialize network\n");
          return 1;
      }

      // ─── Ring buffer for received audio ────────────────────────────────────────
      // Network thread → JACK playout callback
      const size_t frameSize = jack.get_buffer_size();
      AudioRingBuffer playoutBuffer(32, frameSize);  // ~32 frames of jitter buffer

      // Temporary decode buffer (used by network receive thread)
      std::vector<float> decodeBuf(frameSize);

      // ─── Network receive callback ──────────────────────────────────────────────
      network.setReceiveCallback(
          [&codec, &playoutBuffer, &decodeBuf, frameSize]
          (const uint8_t* data, int bytes, uint32_t /*seq*/) {
              // Decode CELT → PCM
              int samples = codec.decode(data, bytes, decodeBuf.data());
              if (samples > 0) {
                  playoutBuffer.push_frame(decodeBuf.data());
              }
          }
      );

      // ─── Encode buffer for JACK callback ───────────────────────────────────────
      std::vector<uint8_t> encodeBuf(codec.getEncodedFrameSize() + 64);

      // ─── JACK audio callback ───────────────────────────────────────────────────
      jack.set_callback(
          [&codec, &network, &playoutBuffer, &encodeBuf, frameSize]
          (const float* input, float* output, uint32_t nframes) {
              // === CAPTURE PATH: mic → encode → network ===
              int encodedBytes = codec.encode(input, encodeBuf.data(),
                                              static_cast<int>(encodeBuf.size()));
              if (encodedBytes > 0) {
                  network.send(encodeBuf.data(), encodedBytes);
              }

              // === PLAYOUT PATH: ring buffer → output ===
              if (!playoutBuffer.pop_frame(output)) {
                  // No data available — output silence (avoids glitch)
                  memset(output, 0, sizeof(float) * nframes);
              }
          }
      );

      // ─── Start everything ──────────────────────────────────────────────────────
      if (!network.start()) {
          fprintf(stderr, "FATAL: Cannot start network\n");
          return 1;
      }

      if (!jack.start()) {
          fprintf(stderr, "FATAL: Cannot start JACK audio\n");
          return 1;
      }

      // ─── Calculate total latency budget ────────────────────────────────────────
      double audioLatencyMs = jack.get_latency_ms();
      double codecLatencyMs = (double)celtCfg.frameSize / celtCfg.sampleRate * 1000.0;
      printf("\n");
      printf("  Latency Budget (one-way estimate)\n");
      printf("  ------------------------------------------\n");
      printf("  JACK capture:     %6.2f ms\n", audioLatencyMs);
      printf("  CELT encode:      %6.2f ms (algorithmic)\n", codecLatencyMs);
      printf("  Network (LAN):    ~0.05-0.5 ms\n");
      printf("  CELT decode:      %6.2f ms (algorithmic)\n", codecLatencyMs);
      printf("  JACK playout:     %6.2f ms\n", audioLatencyMs);
      printf("  ------------------------------------------\n");
      printf("  TOTAL:            ~%.1f ms (LAN)\n",
             audioLatencyMs * 2 + codecLatencyMs * 2 + 0.3);
      printf("\n");

      printf("Running... Press Ctrl+C to stop.\n");
      printf("Stats will print every 5 seconds.\n\n");

      // ─── Main loop (stats printing) ────────────────────────────────────────────
      auto lastStats = std::chrono::steady_clock::now();
      while (gRunning.load(std::memory_order_acquire)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));

          auto now = std::chrono::steady_clock::now();
          if (now - lastStats > std::chrono::seconds(5)) {
              lastStats = now;
              auto stats = network.getStats();
              printf("[STATS] sent=%llu  recv=%llu  lost=%llu  buf=%zu frames\n",
                     (unsigned long long)stats.packetsSent,
                     (unsigned long long)stats.packetsReceived,
                     (unsigned long long)stats.packetsLost,
                     playoutBuffer.available_frames());
          }
      }

      // ─── Shutdown ──────────────────────────────────────────────────────────────
      printf("\nShutting down...\n");
      jack.stop();
      network.stop();

      auto finalStats = network.getStats();
      printf("Final stats: sent=%llu  recv=%llu  lost=%llu\n",
             (unsigned long long)finalStats.packetsSent,
             (unsigned long long)finalStats.packetsReceived,
             (unsigned long long)finalStats.packetsLost);

      return 0;
  }
