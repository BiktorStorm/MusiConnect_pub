// =============================================================================
// MUSICONNECT — Ultra Low Latency P2P Audio (macOS Native)
//
// This ties together:
//   1. Core Audio (capture + playout at 64 samples = 1.33ms)
//   2. CELT codec (encode/decode with zero algorithmic delay)
//   3. UDP transport (send/receive with no retransmission)
//
// DATA FLOW:
//   Core Audio capture callback → CELT encode → UDP send →
//   → UDP receive → CELT decode → Core Audio playout ring buffer
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

#include "audio_coreaudio.h"
#include "celt_codec.h"
#include "network.h"

#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <csignal>
// FIX 2: Headers needed for the lock-free send queue that moves encode+send
// off the Core Audio realtime thread onto a dedicated worker thread.
#include <array>
#include <cstring>

// Global shutdown flag
static std::atomic<bool> g_running{true};

// =============================================================================
// FIX 2: Lock-free SPSC send queue
//
// The Core Audio realtime thread must never call encode() or sendto() directly
// because both can stall (codec CPU spike, kernel socket buffer full, scheduler
// preemption). Instead the capture callback drops raw PCM frames into this
// queue and a dedicated send thread picks them up, encodes, and sends.
//
// Capacity of 4 frames (~5ms at 64 samples/48kHz) is enough to absorb a
// brief send-thread scheduling delay without dropping audio.
// =============================================================================
static constexpr int SEND_QUEUE_CAPACITY = 4;

struct SendFrame {
    float samples[256];  // Enough for up to 256-sample frames
    int count = 0;
};

struct SendQueue {
    std::array<SendFrame, SEND_QUEUE_CAPACITY> slots;
    std::atomic<int> writePos{0};
    std::atomic<int> readPos{0};

    // Called from realtime thread — must never block
    bool push(const float* data, int count) {
        int w = writePos.load(std::memory_order_relaxed);
        int next = (w + 1) % SEND_QUEUE_CAPACITY;
        if (next == readPos.load(std::memory_order_acquire))
            return false;  // Queue full — drop frame rather than stall
        std::memcpy(slots[w].samples, data, count * sizeof(float));
        slots[w].count = count;
        writePos.store(next, std::memory_order_release);
        return true;
    }

    // Called from send thread
    bool pop(SendFrame& out) {
        int r = readPos.load(std::memory_order_relaxed);
        if (r == writePos.load(std::memory_order_acquire))
            return false;  // Empty
        out = slots[r];
        readPos.store((r + 1) % SEND_QUEUE_CAPACITY, std::memory_order_release);
        return true;
    }
};
// =============================================================================

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
              << "  --buffer-size N       Buffer size in samples (default: 64)\n"
              << "  --bitrate N           CELT bitrate in bps (default: 64000)\n"
              << "  --list-devices        List available audio devices and exit\n"
              << "\n"
              << "Example (two instances on localhost):\n"
              << "  Instance A: musiconnect --local-port 4464 --remote-port 4465\n"
              << "  Instance B: musiconnect --local-port 4465 --remote-port 4464\n"
              << std::endl;
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

    // Parse command line
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") { printUsage(argv[0]); return 0; }
        else if (arg == "--list-devices") {
            auto devices = CoreAudioHandler::listDevices();
            std::cout << "Available audio devices:" << std::endl;
            for (size_t j = 0; j < devices.size(); j++) {
                std::cout << "  [" << j << "] " << devices[j] << std::endl;
            }
            if (devices.empty()) std::cout << "  (none found)" << std::endl;
            return 0;
        }
        else if (arg == "--local-port" && i+1 < argc) netConfig.localPort = std::stoi(argv[++i]);
        else if (arg == "--remote-host" && i+1 < argc) netConfig.remoteHost = argv[++i];
        else if (arg == "--remote-port" && i+1 < argc) netConfig.remotePort = std::stoi(argv[++i]);
        else if (arg == "--buffer-size" && i+1 < argc) {
            audioConfig.bufferSize = std::stoi(argv[++i]);
            celtConfig.frameSize = audioConfig.bufferSize;
        }
        else if (arg == "--bitrate" && i+1 < argc) celtConfig.bitrate = std::stoi(argv[++i]);
        else { std::cerr << "Unknown option: " << arg << std::endl; printUsage(argv[0]); return 1; }
    }

    // Handle Ctrl+C gracefully
    signal(SIGINT, signalHandler);

    std::cout << "╔══════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║  MusiConnect — Ultra Low Latency P2P Audio (macOS)  ║" << std::endl;
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
    // 3. Initialize Core Audio
    // =========================================================================
    CoreAudioHandler audio;
    if (!audio.init(audioConfig)) {
        std::cerr << "Failed to initialize Core Audio" << std::endl;
        return 1;
    }

    // Verify frame sizes match
    int actualBufSize = audio.getActualBufferSize();
    if (actualBufSize != celtConfig.frameSize) {
        std::cout << "[WARN] Core Audio buffer (" << actualBufSize
                  << ") != CELT frame size (" << celtConfig.frameSize << ")" << std::endl;
        std::cout << "       Reinitializing CELT to match Core Audio buffer..." << std::endl;
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

    // FIX 2: Instantiate the send queue. The capture callback writes raw PCM
    // into it; the send thread below reads, encodes, and transmits.
    SendQueue sendQueue;

    // CAPTURE → QUEUE (realtime thread — no encode, no syscall)
    // FIX 2: Capture callback now only copies samples into the lock-free queue.
    // Encoding and network send have been moved to the dedicated send thread.
    audio.setCaptureCallback([&](const float* samples, int count) {
        sendQueue.push(samples, count);  // Non-blocking — drops if queue full
    });

    // RECEIVE → DECODE → PLAYOUT (unchanged — network thread is already separate)
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
            // Write to Core Audio playout buffer
            audio.writePlayoutSamples(decodeBuffer.data(), decoded);
        }
    });

    // =========================================================================
    // 5. Start everything
    // =========================================================================

    // FIX 2: Start the dedicated send thread before audio so it is ready to
    // drain the queue as soon as the first capture callback fires.
    // QUEUE → ENCODE → SEND (dedicated thread — encode and sendto() live here)
    std::thread sendThread([&]() {
        SendFrame frame;
        while (g_running) {
            if (sendQueue.pop(frame)) {
                int encoded = encoder.encode(frame.samples, encodeBuffer.data(), maxEncoded);
                if (encoded > 0)
                    network.send(encodeBuffer.data(), encoded);
            } else {
                // Nothing to send — yield briefly to avoid burning a full CPU core.
                // std::this_thread::yield() is intentionally avoided here because
                // on macOS it can sleep for >1ms; a 50µs sleep keeps latency tight.
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }
    });

    network.start();

    if (!audio.start()) {
        std::cerr << "Failed to start Core Audio" << std::endl;
        return 1;
    }

    double frameMs = (double)actualBufSize / audioConfig.sampleRate * 1000.0;
    std::cout << "\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
    std::cout << "  RUNNING — Press Ctrl+C to stop" << std::endl;
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
    std::cout << "  Core Audio buffer: " << actualBufSize << " samples (" << frameMs << "ms)" << std::endl;
    std::cout << "  CELT frame:        " << celtConfig.frameSize << " samples ("
              << encoder.getFrameDurationMs() << "ms)" << std::endl;
    std::cout << "  Encoded size:      " << encoder.getEncodedFrameSize() << " bytes/frame" << std::endl;
    std::cout << "  Local:             0.0.0.0:" << netConfig.localPort << std::endl;
    std::cout << "  Remote:            " << netConfig.remoteHost << ":" << netConfig.remotePort << std::endl;
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
    // FIX 2: Join the send thread after g_running is cleared so it exits its loop.
    if (sendThread.joinable()) sendThread.join();

    std::cout << "Done." << std::endl;
    return 0;
}
