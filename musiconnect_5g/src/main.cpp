// MusiConnect 5G — Ultra low latency P2P audio
//
// Pipeline: Capture -> CELT encode -> UDP send -> UDP recv -> CELT decode -> Playout
//
// Latency budget (5G, same city):
//   Capture:  1.33ms (64 samples)
//   Encode:   ~0.05ms
//   5G:       ~5-10ms
//   Decode:   ~0.05ms
//   Playout:  1.33ms
//   Total:    ~8-13ms one-way

#include "audio.h"
#include "celt_codec.h"
#include "network.h"

#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <csignal>
#include <vector>
#include <chrono>
#include <memory>

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_muteMic{false};
static std::atomic<bool> g_muteSpk{false};

static void onSignal(int) { g_running = false; }

struct AppContext {
    CeltCodec* encoder;
    UdpTransport* network;
    std::vector<uint8_t> encBuf;
    int maxEncBytes;
};

static void captureCallback(const float* samples, int count, void* userData) {
    if (g_muteMic.load(std::memory_order_relaxed)) return;
    auto* ctx = static_cast<AppContext*>(userData);
    int encoded = ctx->encoder->encode(samples, ctx->encBuf.data(), ctx->maxEncBytes);
    if (encoded > 0) ctx->network->send(ctx->encBuf.data(), encoded);
}

static void printControls() {
    std::cout << "\n"
              << "  Controls:\n"
              << "    m = toggle mic mute\n"
              << "    s = toggle speaker mute\n"
              << "    q = quit\n\n";
}

static void printStatus() {
    std::cout << "  [MIC: " << (g_muteMic.load() ? "MUTED" : "LIVE")
              << "]  [SPEAKER: " << (g_muteSpk.load() ? "MUTED" : "LIVE")
              << "]\n";
}

int main(int argc, char* argv[]) {
    AudioConfig audioCfg;
    audioCfg.sampleRate = 48000;
    audioCfg.bufferSize = 64;

    CeltConfig celtCfg;
    celtCfg.sampleRate = 48000;
    celtCfg.frameSize = 64;
    celtCfg.channels = 1;
    celtCfg.bitrate = 64000;
    celtCfg.complexity = 1;

    NetworkConfig netCfg;
    netCfg.localPort = 4464;
    netCfg.remoteHost = "127.0.0.1";
    netCfg.remotePort = 4465;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--local-port" && i+1 < argc) netCfg.localPort = std::stoi(argv[++i]);
        else if (arg == "--remote-host" && i+1 < argc) netCfg.remoteHost = argv[++i];
        else if (arg == "--remote-port" && i+1 < argc) netCfg.remotePort = std::stoi(argv[++i]);
        else if (arg == "--buffer-size" && i+1 < argc) {
            audioCfg.bufferSize = std::stoi(argv[++i]);
            celtCfg.frameSize = audioCfg.bufferSize;
        }
        else if (arg == "--bitrate" && i+1 < argc) celtCfg.bitrate = std::stoi(argv[++i]);
        else if (arg == "--driver" && i+1 < argc) audioCfg.driverName = argv[++i];
        else if (arg == "--list-drivers") {
            std::unique_ptr<AudioDevice> dev(createAudioDevice());
            for (auto& d : dev->listDrivers()) std::cout << "  " << d << "\n";
            return 0;
        }
        else { std::cerr << "Unknown: " << arg << "\n"; return 1; }
    }

    signal(SIGINT, onSignal);

    // Init codec
    CeltCodec encoder, decoder;
    if (!encoder.init(celtCfg) || !decoder.init(celtCfg)) return 1;

    // Init network
    UdpTransport network;
    if (!network.init(netCfg)) return 1;

    // Init audio
    std::unique_ptr<AudioDevice> audio(createAudioDevice());
    if (!audio->init(audioCfg)) return 1;

    int bufSz = audio->getBufferSize();
    if (bufSz != celtCfg.frameSize) {
        celtCfg.frameSize = bufSz;
        encoder = CeltCodec();
        decoder = CeltCodec();
        if (!encoder.init(celtCfg) || !decoder.init(celtCfg)) return 1;
    }

    // Wire capture -> encode -> send
    AppContext ctx{&encoder, &network, std::vector<uint8_t>(encoder.getEncodedSize() + 16), encoder.getEncodedSize() + 16};
    audio->setCaptureCallback(captureCallback, &ctx);

    // Wire receive -> decode -> playout
    std::vector<float> decodeBuf(celtCfg.frameSize);
    network.setReceiveCallback([&](const uint8_t* data, int len, uint32_t) {
        if (g_muteSpk.load(std::memory_order_relaxed)) return;
        int decoded = decoder.decode(data, len, decodeBuf.data());
        if (decoded > 0) audio->writePlayoutSamples(decodeBuf.data(), decoded);
    });

    // Start
    network.start();
    if (!audio->start()) return 1;

    double frameMs = (double)bufSz / audioCfg.sampleRate * 1000.0;
    std::cout << "\n[RUNNING] " << bufSz << " samples (" << frameMs << "ms) | "
              << netCfg.remoteHost << ":" << netCfg.remotePort << "\n";
    printControls();
    printStatus();

    // Input loop — handles keyboard commands on main thread
    while (g_running) {
        // Non-blocking-ish: we use std::cin which blocks, but that's fine
        // for the UI thread — audio runs independently
        std::string input;
        if (!std::getline(std::cin, input)) break;

        if (input == "m") {
            g_muteMic.store(!g_muteMic.load(), std::memory_order_relaxed);
            printStatus();
        } else if (input == "s") {
            g_muteSpk.store(!g_muteSpk.load(), std::memory_order_relaxed);
            printStatus();
        } else if (input == "q") {
            break;
        } else if (input == "stats") {
            auto ns = network.getStats();
            auto as = audio->getStats();
            std::cout << "  Sent:" << ns.sent << " Recv:" << ns.received
                      << " Lost:" << ns.lost << " Underruns:" << as.underruns << "\n";
        }
    }

    std::cout << "Stopping...\n";
    audio->stop();
    network.stop();
    return 0;
}
