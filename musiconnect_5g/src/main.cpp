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
static void onSignal(int) { g_running = false; }

struct AppContext {
    CeltCodec* encoder;
    UdpTransport* network;
    std::vector<uint8_t> encBuf;
    int maxEncBytes;
};

static void captureCallback(const float* samples, int count, void* userData) {
    auto* ctx = static_cast<AppContext*>(userData);
    int encoded = ctx->encoder->encode(samples, ctx->encBuf.data(), ctx->maxEncBytes);
    if (encoded > 0) ctx->network->send(ctx->encBuf.data(), encoded);
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

    // Init audio (platform-specific via factory)
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

    // Wire receive -> decode -> playout (immediate)
    std::vector<float> decodeBuf(celtCfg.frameSize);
    network.setReceiveCallback([&](const uint8_t* data, int len, uint32_t) {
        int decoded = decoder.decode(data, len, decodeBuf.data());
        if (decoded > 0) audio->writePlayoutSamples(decodeBuf.data(), decoded);
    });

    // Start
    network.start();
    if (!audio->start()) return 1;

    double frameMs = (double)bufSz / audioCfg.sampleRate * 1000.0;
    std::cout << "\n[RUNNING] " << bufSz << " samples (" << frameMs << "ms) | "
              << netCfg.remoteHost << ":" << netCfg.remotePort << " | Ctrl+C to stop\n\n";

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        auto ns = network.getStats();
        auto as = audio->getStats();
        std::cout << "\r  Sent:" << ns.sent << " Recv:" << ns.received
                  << " Lost:" << ns.lost << " Underruns:" << as.underruns << "   " << std::flush;
    }

    std::cout << "\nStopping...\n";
    audio->stop();
    network.stop();
    return 0;
}
