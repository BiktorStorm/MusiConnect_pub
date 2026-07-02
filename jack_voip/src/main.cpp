
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

  // ─── Global state ──────────────────────────────────────────────────────────────
  static std::atomic<bool> g_running{true};

  void signal_handler(int) {
      g_running.store(false, std::memory_order_release);
  }

  // ─── Command line parsing ──────────────────────────────────────────────────────
  struct AppConfig {
      uint16_t    local_port   = 4464;
      uint16_t    remote_port  = 4465;
      std::string remote_host  = "127.0.0.1";
      int         bitrate      = 128000;
      uint32_t    buffer_size  = 64;      // JACK frames per period
      std::string client_name  = "jack_voip";
  };

  static AppConfig parse_args(int argc, char* argv[]) {
      AppConfig cfg;
      for (int i = 1; i < argc; ++i) {
          std::string arg = argv[i];
          if (arg == "--local-port" && i + 1 < argc)
              cfg.local_port = static_cast<uint16_t>(std::stoi(argv[++i]));
          else if (arg == "--remote-port" && i + 1 < argc)
              cfg.remote_port = static_cast<uint16_t>(std::stoi(argv[++i]));
          else if (arg == "--remote-host" && i + 1 < argc)
              cfg.remote_host = argv[++i];
          else if (arg == "--bitrate" && i + 1 < argc)
              cfg.bitrate = std::stoi(argv[++i]);
          else if (arg == "--buffer-size" && i + 1 < argc)
              cfg.buffer_size = static_cast<uint32_t>(std::stoi(argv[++i]));
          else if (arg == "--name" && i + 1 < argc)
              cfg.client_name = argv[++i];
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
      signal(SIGINT, signal_handler);
      signal(SIGTERM, signal_handler);

      AppConfig cfg = parse_args(argc, argv);

      printf("╔══════════════════════════════════════════════════╗\n");
      printf("║         JACK VoIP — Ultra-Low Latency           ║\n");
      printf("╠══════════════════════════════════════════════════╣\n");
      printf("║  Local port:  %-10u                        ║\n", cfg.local_port);
      printf("║  Remote:      %s:%-5u                  ║\n",
             cfg.remote_host.c_str(), cfg.remote_port);
      printf("║  Bitrate:     %-6d bps                      ║\n", cfg.bitrate);
      printf("║  Buffer:      %-4u frames                      ║\n", cfg.buffer_size);
      printf("╚══════════════════════════════════════════════════╝\n\n");

      // ─── Initialize JACK ───────────────────────────────────────────────────────
      AudioJack jack;
      JackConfig jack_cfg;
      jack_cfg.client_name = cfg.client_name;
      jack_cfg.buffer_size = cfg.buffer_size;

      if (!jack.init(jack_cfg)) {
          fprintf(stderr, "FATAL: Cannot initialize JACK. Is jackd running?\n");
          return 1;
      }

      // ─── Initialize CELT Codec ─────────────────────────────────────────────────
      CeltCodec codec;
      CeltConfig celt_cfg;
      celt_cfg.sample_rate = jack.get_sample_rate();
      celt_cfg.frame_size  = jack.get_buffer_size();
      celt_cfg.bitrate     = cfg.bitrate;

      if (!codec.init(celt_cfg)) {
          fprintf(stderr, "FATAL: Cannot initialize CELT codec\n");
          return 1;
      }

      // ─── Initialize Network ────────────────────────────────────────────────────
      UdpTransport network;
      NetworkConfig net_cfg;
      net_cfg.local_port  = cfg.local_port;
      net_cfg.remote_port = cfg.remote_port;
      net_cfg.remote_host = cfg.remote_host;

      if (!network.init(net_cfg)) {
          fprintf(stderr, "FATAL: Cannot initialize network\n");
          return 1;
      }

      // ─── Ring buffer for received audio ────────────────────────────────────────
      // Network thread → JACK playout callback
      const size_t frame_size = jack.get_buffer_size();
      AudioRingBuffer playout_buffer(32, frame_size);  // ~32 frames of jitter buffer

      // Temporary decode buffer (used by network receive thread)
      std::vector<float> decode_buf(frame_size);

      // ─── Network receive callback ──────────────────────────────────────────────
      network.set_receive_callback(
          [&codec, &playout_buffer, &decode_buf, frame_size]
          (const uint8_t* data, int bytes, uint32_t /*seq*/) {
              // Decode CELT → PCM
              int samples = codec.decode(data, bytes, decode_buf.data());
              if (samples > 0) {
                  playout_buffer.push_frame(decode_buf.data());
              }
          }
      );

      // ─── Encode buffer for JACK callback ───────────────────────────────────────
      std::vector<uint8_t> encode_buf(codec.get_max_packet_size());

      // ─── JACK audio callback ───────────────────────────────────────────────────
      jack.set_callback(
          [&codec, &network, &playout_buffer, &encode_buf, frame_size]
          (const float* input, float* output, uint32_t nframes) {
              // === CAPTURE PATH: mic → encode → network ===
              int encoded_bytes = codec.encode(input, encode_buf.data(),
                                               static_cast<int>(encode_buf.size()));
              if (encoded_bytes > 0) {
                  network.send(encode_buf.data(), encoded_bytes);
              }

              // === PLAYOUT PATH: ring buffer → output ===
              if (!playout_buffer.pop_frame(output)) {
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
      double audio_latency_ms = jack.get_latency_ms();
      double codec_latency_ms = (double)celt_cfg.frame_size / celt_cfg.sample_rate * 1000.0;
      printf("\n┌─────────────────────────────────────────────────┐\n");
      printf("│ Latency Budget (one-way estimate)               │\n");
      printf("├─────────────────────────────────────────────────┤\n");
      printf("│ JACK capture:     %6.2f ms                     │\n", audio_latency_ms);
      printf("│ CELT encode:      %6.2f ms (algorithmic)       │\n", codec_latency_ms);
      printf("│ Network (LAN):    ~0.05-0.5 ms                  │\n");
      printf("│ CELT decode:      %6.2f ms (algorithmic)       │\n", codec_latency_ms);
      printf("│ JACK playout:     %6.2f ms                     │\n", audio_latency_ms);
      printf("├─────────────────────────────────────────────────┤\n");
      printf("│ TOTAL:            ~%.1f ms (LAN)                │\n",
             audio_latency_ms * 2 + codec_latency_ms * 2 + 0.3);
      printf("└─────────────────────────────────────────────────┘\n\n");

      printf("Running... Press Ctrl+C to stop.\n");
      printf("Stats will print every 5 seconds.\n\n");

      // ─── Main loop (stats printing) ────────────────────────────────────────────
      auto last_stats = std::chrono::steady_clock::now();
      while (g_running.load(std::memory_order_acquire)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));

          auto now = std::chrono::steady_clock::now();
          if (now - last_stats > std::chrono::seconds(5)) {
              last_stats = now;
              printf("[STATS] sent=%llu  recv=%llu  lost=%llu  buf=%zu frames\n",
                     (unsigned long long)network.get_packets_sent(),
                     (unsigned long long)network.get_packets_received(),
                     (unsigned long long)network.get_packets_lost(),
                     playout_buffer.available_frames());
          }
      }

      // ─── Shutdown ──────────────────────────────────────────────────────────────
      printf("\nShutting down...\n");
      jack.stop();
      network.stop();

      printf("Final stats: sent=%llu  recv=%llu  lost=%llu\n",
             (unsigned long long)network.get_packets_sent(),
             (unsigned long long)network.get_packets_received(),
             (unsigned long long)network.get_packets_lost());

      return 0;
  }
