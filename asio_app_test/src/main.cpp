
  // =============================================================================
  // MUSICONNECT — Ultra Low Latency P2P Audio (Native)
  //
  // This ties together:
  //   1. ASIO audio (capture + playout at 64 samples = 1.33ms)
  //   2. CELT codec (encode/decode with zero algorithmic delay)
  //   3. UDP transport (send/receive with no retransmission)
  //
  // DATA FLOW:
  //   ASIO capture callback → CELT encode → UDP send →
  //   → UDP receive → CELT decode → ASIO playout ring buffer
  //
  // LATENCY BUDGET (localhost):
  //   Capture:  1.33ms (64 samples)
  //   Encode:   ~0.05ms
  //   Network:  ~0.1ms (localhost)
  //   Decode:   ~0.05ms
  //   Playout:  1.33ms (64 samples)
  //   ─────────────────────────
  //   TOTAL:    ~3ms
  //
  // With a real network (LAN): add RTT/2 (~0.5ms LAN, 5-50ms internet)
  // =============================================================================

  #include "audio_asio.h"
  #include "celt_codec.h"
  #include "network.h"

  #include <iostream>
  #include <string>
  #include <chrono>
  #include <thread>
  #include <atomic>
  #include <csignal>

  // Global shutdown flag
  static std::atomic<bool> g_running{true};

  void signalHandler(int) {
      g_running = false;
  }

  void printUsage(const char* argv0) {
      std::cout << "Usage: " << argv0 << " [options]\n"
                << "\n"
                << "Options:\n"
                << "  --local-port PORT     Local UDP port (default: 4464)\n"
                << "  --remote-host HOST    Remote peer IP (default: 127.0.0.1)\n"
                << "  --remote-port PORT    Remote peer port (default: 4465)\n"
                << "  --buffer-size N       ASIO buffer size in samples (default: 64)\n"
                << "  --bitrate N           CELT bitrate in bps (default: 64000)\n"
                << "  --driver NAME         ASIO driver name (default: first available)\n"
                << "  --input-channel N     Input channel to capture from (default: 0)\n"
                << "  --output-channel N    Output channel to play to (default: 0)\n"
                << "  --interactive, -i     Interactively pick driver + input/output channels\n"
                << "  --list-drivers        List available ASIO drivers and exit\n"
                << "\n"
                << "Example (two instances on localhost):\n"
                << "  Instance A: musiconnect --local-port 4464 --remote-port 4465\n"
                << "  Instance B: musiconnect --local-port 4465 --remote-port 4464\n"
                << "\n"
                << "Example (pick your Scarlett input/output interactively):\n"
                << "  musiconnect --interactive --remote-host 192.168.1.42\n"
                << std::endl;
  }

  // Read an integer selection from stdin. Returns `def` on empty/invalid input,
  // clamped to [0, count).
  static int promptIndex(int count, int def) {
      std::cout << "> " << std::flush;
      std::string line;
      std::getline(std::cin, line);
      if (line.empty()) return def;
      try {
          int idx = std::stoi(line);
          if (idx < 0 || idx >= count) {
              std::cout << "  (out of range — using " << def << ")" << std::endl;
              return def;
          }
          return idx;
      } catch (...) {
          std::cout << "  (invalid — using " << def << ")" << std::endl;
          return def;
      }
  }

  // Interactively select ASIO driver + input/output channel.
  // Fills audioConfig.driverName / inputChannel / outputChannel.
  // Returns false if no drivers found or channel query fails.
  static bool runInteractiveSelection(AudioConfig& audioConfig) {
      // --- 1. Pick driver ---
      auto drivers = AsioAudioHandler::listDrivers();
      if (drivers.empty()) {
          std::cerr << "No ASIO drivers found.\n"
                    << "Make sure your audio interface is connected and its ASIO driver is installed."
                    << std::endl;
          return false;
      }

      std::cout << "\n=== Select ASIO driver ===" << std::endl;
      for (size_t i = 0; i < drivers.size(); i++)
          std::cout << "  [" << i << "] " << drivers[i] << std::endl;

      size_t driverIdx = 0;
      if (drivers.size() == 1) {
          std::cout << "  (only one driver — auto-selected)" << std::endl;
      } else {
          driverIdx = (size_t)promptIndex((int)drivers.size(), 0);
      }
      audioConfig.driverName = drivers[driverIdx];

      // --- 2. Query that driver's channels ---
      auto channels = AsioAudioHandler::queryChannels(audioConfig.driverName);
      if (!channels.success) {
          std::cerr << "Failed to query channels for driver: " << audioConfig.driverName << std::endl;
          return false;
      }

      // --- 3. Pick input channel (audio source) ---
      std::cout << "\n=== Select INPUT channel (audio source) ===" << std::endl;
      for (size_t i = 0; i < channels.inputs.size(); i++)
          std::cout << "  [" << i << "] " << channels.inputs[i] << std::endl;
      audioConfig.inputChannel = promptIndex((int)channels.inputs.size(), 0);

      // --- 4. Pick output channel (playback destination) ---
      std::cout << "\n=== Select OUTPUT channel (playback destination) ===" << std::endl;
      for (size_t i = 0; i < channels.outputs.size(); i++)
          std::cout << "  [" << i << "] " << channels.outputs[i] << std::endl;
      audioConfig.outputChannel = promptIndex((int)channels.outputs.size(), 0);

      // --- Confirm ---
      std::cout << "\nSelected:" << std::endl;
      std::cout << "  Driver: " << audioConfig.driverName << std::endl;
      std::cout << "  Input:  [" << audioConfig.inputChannel << "] "
                << channels.inputs[audioConfig.inputChannel] << std::endl;
      std::cout << "  Output: [" << audioConfig.outputChannel << "] "
                << channels.outputs[audioConfig.outputChannel] << std::endl;
      std::cout << std::endl;
      return true;
  }


  int main(int argc, char* argv[]) {
      // Defaults
      AudioConfig audioConfig;
      audioConfig.sampleRate = 48000;
      audioConfig.bufferSize = 64;

      CeltConfig celtConfig;
      celtConfig.sampleRate = 48000;
      celtConfig.frameSize = 64;
      celtConfig.channels = 1;
      celtConfig.bitrate = 64000;
      celtConfig.complexity = 1;

      NetworkConfig netConfig;
      netConfig.localPort = 4464;
      netConfig.remoteHost = "127.0.0.1";
      netConfig.remotePort = 4465;

      bool interactive = false;

      // Parse command line
      for (int i = 1; i < argc; i++) {
          std::string arg = argv[i];
          if (arg == "--help" || arg == "-h") { printUsage(argv[0]); return 0; }
          else if (arg == "--list-drivers") {
              auto drivers = AsioAudioHandler::listDrivers();
              std::cout << "Available ASIO drivers:" << std::endl;
              for (size_t j = 0; j < drivers.size(); j++) {
                  std::cout << "  [" << j << "] " << drivers[j] << std::endl;
              }
              if (drivers.empty()) std::cout << "  (none found)" << std::endl;
              return 0;
          }
          else if (arg == "--interactive" || arg == "-i") interactive = true;
          else if (arg == "--input-channel" && i+1 < argc) audioConfig.inputChannel = std::stoi(argv[++i]);
          else if (arg == "--output-channel" && i+1 < argc) audioConfig.outputChannel = std::stoi(argv[++i]);
          else if (arg == "--local-port" && i+1 < argc) netConfig.localPort = std::stoi(argv[++i]);
          else if (arg == "--remote-host" && i+1 < argc) netConfig.remoteHost = argv[++i];
          else if (arg == "--remote-port" && i+1 < argc) netConfig.remotePort = std::stoi(argv[++i]);
          else if (arg == "--buffer-size" && i+1 < argc) {
              audioConfig.bufferSize = std::stoi(argv[++i]);
              celtConfig.frameSize = audioConfig.bufferSize;
          }
          else if (arg == "--bitrate" && i+1 < argc) celtConfig.bitrate = std::stoi(argv[++i]);
          else if (arg == "--driver" && i+1 < argc) audioConfig.driverName = argv[++i];
          else { std::cerr << "Unknown option: " << arg << std::endl; printUsage(argv[0]); return 1; }
      }

      // Handle Ctrl+C gracefully
      signal(SIGINT, signalHandler);

      // Interactive driver + channel selection (fills driverName / input / output)
      if (interactive) {
          if (!runInteractiveSelection(audioConfig)) {
              std::cerr << "Interactive selection failed — aborting." << std::endl;
              return 1;
          }
      }

      std::cout << "╔══════════════════════════════════════════════════════╗" << std::endl;
      std::cout << "║  MusiConnect — Ultra Low Latency P2P Audio          ║" << std::endl;
      std::cout << "╚══════════════════════════════════════════════════════╝" << std::endl;
      std::cout << std::endl;

      // =========================================================================
      // 1. Initialize CELT codec
      // =========================================================================
      CeltCodec encoder, decoder;

      if (!encoder.init(celtConfig)) {
          std::cerr << "Failed to initialize CELT encoder" << std::endl;
          return 1;
      }
      if (!decoder.init(celtConfig)) {
          std::cerr << "Failed to initialize CELT decoder" << std::endl;
          return 1;
      }

      // =========================================================================
      // 2. Initialize network
      // =========================================================================
      UdpTransport network;
      if (!network.init(netConfig)) {
          std::cerr << "Failed to initialize network" << std::endl;
          return 1;
      }

      // =========================================================================
      // 3. Initialize ASIO audio
      // =========================================================================
      AsioAudioHandler audio;
      if (!audio.init(audioConfig)) {
          std::cerr << "Failed to initialize ASIO" << std::endl;
          return 1;
      }

      // Verify frame sizes match
      int actualBufSize = audio.getActualBufferSize();
      if (actualBufSize != celtConfig.frameSize) {
          std::cout << "[WARN] ASIO buffer (" << actualBufSize
                    << ") != CELT frame size (" << celtConfig.frameSize << ")" << std::endl;
          std::cout << "       Reinitializing CELT to match ASIO buffer..." << std::endl;
          celtConfig.frameSize = actualBufSize;
          encoder = CeltCodec();
          decoder = CeltCodec();
          if (!encoder.init(celtConfig) || !decoder.init(celtConfig)) {
              std::cerr << "Failed to reinitialize CELT with matching frame size" << std::endl;
              return 1;
          }
      }

      // =========================================================================
      // 4. Wire up the pipeline
      // =========================================================================

      // Encode buffer (allocated once, reused every callback)
      int maxEncoded = encoder.getEncodedFrameSize() + 16;  // Some headroom
      std::vector<uint8_t> encodeBuffer(maxEncoded);

      // CAPTURE → ENCODE → SEND
      // This runs on the ASIO callback thread (realtime priority)
      audio.setCaptureCallback([&](const float* samples, int count) {
          // Encode the captured audio
          int encoded = encoder.encode(samples, encodeBuffer.data(), maxEncoded);
          if (encoded > 0) {
              // Send over network
              network.send(encodeBuffer.data(), encoded);
          }
      });

      // RECEIVE → DECODE → PLAYOUT
      // Decode buffer
      std::vector<float> decodeBuffer(celtConfig.frameSize * celtConfig.channels);
      uint32_t lastRecvSeq = 0;

      network.setReceiveCallback([&](const uint8_t* data, int length, uint32_t seq) {
          // Check for packet loss — use PLC for missing frames
          if (lastRecvSeq > 0 && seq > lastRecvSeq + 1) {
              int lost = seq - lastRecvSeq - 1;
              for (int i = 0; i < lost && i < 3; i++) {  // Max 3 PLC frames
                  decoder.decodePLC(decodeBuffer.data());
                  audio.writePlayoutSamples(decodeBuffer.data(), celtConfig.frameSize);
              }
          }
          lastRecvSeq = seq;

          // Decode received frame
          int decoded = decoder.decode(data, length, decodeBuffer.data());
          if (decoded > 0) {
              // Write to ASIO playout buffer
              audio.writePlayoutSamples(decodeBuffer.data(), decoded);
          }
      });

      // =========================================================================
      // 5. Start everything
      // =========================================================================
      network.start();

      if (!audio.start()) {
          std::cerr << "Failed to start ASIO" << std::endl;
          return 1;
      }

      double frameMs = (double)actualBufSize / audioConfig.sampleRate * 1000.0;
      std::cout << "\n";
      std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
      std::cout << "  RUNNING — Press Ctrl+C to stop" << std::endl;
      std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
      std::cout << "  ASIO buffer:  " << actualBufSize << " samples (" << frameMs << "ms)" << std::endl;
      std::cout << "  CELT frame:   " << celtConfig.frameSize << " samples ("
                << encoder.getFrameDurationMs() << "ms)" << std::endl;
      std::cout << "  Encoded size: " << encoder.getEncodedFrameSize() << " bytes/frame" << std::endl;
      std::cout << "  Local:        0.0.0.0:" << netConfig.localPort << std::endl;
      std::cout << "  Remote:       " << netConfig.remoteHost << ":" << netConfig.remotePort << std::endl;
      std::cout << "  Theoretical latency: ~" << (frameMs * 2 + 0.2) << "ms (capture + playout + codec)" << std::endl;
      std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
      std::cout << std::endl;

      // =========================================================================
      // 6. Stats loop (print every 2 seconds)
      // =========================================================================
      while (g_running) {
          std::this_thread::sleep_for(std::chrono::seconds(2));

          auto audioStats = audio.getStats();
          auto netStats = network.getStats();

          std::cout << "\r[STATS] "
                    << "Sent: " << netStats.packetsSent
                    << " | Recv: " << netStats.packetsReceived
                    << " | Lost: " << netStats.packetsLost
                    << " | Underruns: " << audioStats.underruns
                    << "    " << std::flush;
      }

      // =========================================================================
      // 7. Shutdown
      // =========================================================================
      std::cout << "\n\nShutting down..." << std::endl;
      audio.stop();
      network.stop();

      std::cout << "Done." << std::endl;
      return 0;
  }
