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

#include "audio_coreaudio.h"
#include "celt_codec.h"
#include "network.h"

#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <csignal>
#include <unordered_map>
#include <memory>

struct PeerInfo{
    std::unique_ptr<CeltCodec> decoder_ptr = std::make_unique<CeltCodec>();
    uint32_t lastRecvSeq = 0;
};

// Global shutdown flag
static std::atomic<bool> g_running{true};

void signalHandler(int) {
    g_running = false;
}

void printUsage(const char* argv0) {
    std::cout << "Usage: " << argv0 << " [options]\n" << "\n" << "Options:\n" << "  --local-port PORT     Local UDP port (default: 4464)\n" << "  --remote-host HOST    Remote peer IP (default: 127.0.0.1)\n" << "  --remote-port PORT    Remote peer port (default: 4465)\n" << "  --buffer-size N       Buffer size in samples (default: 64)\n" << "  --bitrate N           CELT bitrate in bps (default: 64000)\n" << "  --list-devices        List available audio devices and exit\n" << "\n" << "Example (two instances on localhost):\n" << "  Instance A: musiconnect --local-port 4464 --remote-port 4465\n" << "  Instance B: musiconnect --local-port 4465 --remote-port 4464\n" << std::endl;
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
    netConfig.localPort = 4465;
    netConfig.peers = {};
    netConfig.remotePort = 4465;
    netConfig.senderId = 0;

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
        else if (arg == "--peer" && i+1 < argc) netConfig.peers.push_back(argv[++i]);
        else if (arg == "--remote-port" && i+1 < argc) netConfig.remotePort = std::stoi(argv[++i]);
        else if (arg == "--buffer-size" && i+1 < argc) {
            audioConfig.bufferSize = std::stoi(argv[++i]);
            celtConfig.frameSize = audioConfig.bufferSize;
        }
        else if (arg == "--senderId" && i+1 < argc) netConfig.senderId = std::stoi(argv[++i]);
        else if (arg == "--bitrate" && i+1 < argc) celtConfig.bitrate = std::stoi(argv[++i]);
        else { std::cerr << "Unknown option: " << arg << std::endl; printUsage(argv[0]); return 1; }
    }

    // Handle Ctrl+C gracefully
    signal(SIGINT, signalHandler);

    std::cout << "╔══════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║                 MusiConnect (macOS)                  ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════════════╝" << std::endl;
    std::cout << std::endl;

    // =========================================================================
    // 1. Initialize network
    // =========================================================================
    UdpTransport network;
    if (!network.init(netConfig)) {
        std::cerr << "Failed to initialize network" << std::endl;
        return 1;
    }

    // =========================================================================
    // 2. Initialize Core Audio
    // =========================================================================
    CoreAudioHandler audio;
    if (!audio.init(audioConfig)) {
        std::cerr << "Failed to initialize Core Audio" << std::endl;
        return 1;
    }

    // Verify frame sizes match
    int actualBufSize = audio.getActualBufferSize();
    if (actualBufSize != celtConfig.frameSize) {
        std::cout << "[WARN] Core Audio buffer (" << actualBufSize << ") != CELT frame size (" << celtConfig.frameSize << ")" << std::endl;
        celtConfig.frameSize = actualBufSize;
    }

    // =========================================================================
    // 3. Initialize CELT codec
    // =========================================================================
    CeltCodec encoder;

    if (!encoder.init(celtConfig)) {
        std::cerr << "Failed to initialize CELT encoder" << std::endl;
        return 1;
    }    

    std::unordered_map<uint8_t, PeerInfo> peerInfos;    //senderId -> decoder

    // =========================================================================
    // 4. Wire up the pipeline
    // =========================================================================

    // Encode buffer (allocated once, reused every callback)
    int maxEncoded = encoder.getEncodedFrameSize() + 16;  // Some headroom
    std::vector<uint8_t> encodeBuffer(maxEncoded);

    // CAPTURE → ENCODE → SEND
    // This runs on the Core Audio render thread (realtime priority)
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

    network.setReceiveCallback([&](const uint8_t* data, int length, uint32_t seq, uint8_t id) {
        // Get or create decoder for this sender
        CeltCodec& decoder = *peerInfos[id].decoder_ptr;
        if (!decoder.isInitialized()) {
            if (!decoder.init(celtConfig)) {
                std::cerr << "Failed to initialize decoder for sender " << (int)id << std::endl;
                return;
            }
        } 
        // Check for packet loss — use PLC for missing frames
        uint32_t& lastRecvSeq = peerInfos[id].lastRecvSeq;
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
    std::cout << "  CELT frame:        " << celtConfig.frameSize << " samples (" << encoder.getFrameDurationMs() << "ms)" << std::endl;
    std::cout << "  Encoded size:      " << encoder.getEncodedFrameSize() << " bytes/frame" << std::endl;
    std::cout << "  Local:             0.0.0.0:" << netConfig.localPort << std::endl;
    std::cout << "  Peers:            ";
    for(const auto& peer : netConfig.peers){
        std::cout << peer << ", ";
    }
    std::cout << netConfig.remotePort << std::endl;
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

        std::cout << "\r[STATS] " << "Sent: " << netStats.packetsSent << " | Recv: " << netStats.packetsReceived << " | Lost: " << netStats.packetsLost << " | Underruns: " << audioStats.underruns << "    " << std::flush;
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
