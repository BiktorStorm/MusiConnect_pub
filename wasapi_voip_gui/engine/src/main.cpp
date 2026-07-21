// =============================================================================
// MUSICONNECT — Ultra Low Latency P2P Audio (Windows WASAPI)
//
// Supports two modes:
//   1. CLI mode (default): interactive prompts, same as original
//   2. JSON mode (--json-mode): reads/writes JSON on stdin/stdout for GUI
//
// DATA FLOW (unchanged):
//   WASAPI capture thread → CELT encode → UDP send →
//   → UDP receive → CELT decode → WASAPI playout ring buffer
// =============================================================================

#include "audio_wasapi.h"
#include "celt_codec.h"
#include "network.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <objbase.h>

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <csignal>
#include <vector>
#include <mutex>
#include <sstream>

using json = nlohmann::json;

// Global shutdown flag
static std::atomic<bool> g_running{true};

BOOL WINAPI consoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

// =============================================================================
// JSON output helpers (thread-safe stdout writing)
// =============================================================================
static std::mutex g_stdout_mutex;

void jsonOut(const json& msg) {
    std::lock_guard<std::mutex> lock(g_stdout_mutex);
    std::cout << msg.dump() << "\n" << std::flush;
}

void jsonError(const std::string& message) {
    jsonOut({{"type", "error"}, {"message", message}});
}

// =============================================================================
// JSON MODE — GUI communication protocol
// =============================================================================
int runJsonMode() {
    SetConsoleCtrlHandler(consoleHandler, TRUE);

    // Announce ready
    jsonOut({{"type", "ready"}});

    // State
    bool audioRunning = false;
    std::unique_ptr<WasapiAudioHandler> audio;
    std::unique_ptr<CeltCodec> encoder, decoder;
    std::unique_ptr<UdpTransport> network;

    // Accumulation buffer for capture
    std::vector<float> captureAccum;
    std::vector<uint8_t> encodeBuffer;
    std::vector<float> decodeBuffer;
    int celtFrameSize = 128;
    int maxEncoded = 0;
    uint32_t lastRecvSeq = 0;

    // Stats thread
    std::thread statsThread;
    std::atomic<bool> statsRunning{false};

    auto stopAudio = [&]() {
        if (!audioRunning) return;
        statsRunning = false;
        if (statsThread.joinable()) statsThread.join();
        if (audio) audio->stop();
        if (network) network->stop();
        audio.reset();
        encoder.reset();
        decoder.reset();
        network.reset();
        audioRunning = false;
        jsonOut({{"type", "stopped"}});
    };

    // Read JSON commands from stdin
    std::string line;
    while (g_running && std::getline(std::cin, line)) {
        if (line.empty()) continue;

        json cmd;
        try {
            cmd = json::parse(line);
        } catch (const json::parse_error&) {
            jsonError("Invalid JSON");
            continue;
        }

        std::string cmdType = cmd.value("cmd", "");

        // ----- LIST DEVICES -----
        if (cmdType == "list-input-devices") {
            auto devices = WasapiAudioHandler::listInputDevices();
            jsonOut({{"type", "devices"}, {"direction", "input"}, {"devices", devices}});
        }
        else if (cmdType == "list-output-devices") {
            auto devices = WasapiAudioHandler::listOutputDevices();
            jsonOut({{"type", "devices"}, {"direction", "output"}, {"devices", devices}});
        }
        // ----- START -----
        else if (cmdType == "start") {
            if (audioRunning) {
                stopAudio();
            }

            // Parse config
            AudioConfig audioConfig;
            audioConfig.sampleRate = 48000;
            audioConfig.bufferSize = cmd.value("bufferSize", 128);
            audioConfig.inputDeviceIndex = cmd.value("inputDevice", 0);
            audioConfig.outputDeviceIndex = cmd.value("outputDevice", 0);

            CeltConfig celtConfig;
            celtConfig.sampleRate = 48000;
            celtConfig.frameSize = audioConfig.bufferSize;
            celtConfig.channels = 1;
            celtConfig.bitrate = cmd.value("bitrate", 64000);
            celtConfig.complexity = 1;

            NetworkConfig netConfig;
            netConfig.localPort = cmd.value("localPort", 4464);
            netConfig.remoteHost = cmd.value("remoteHost", "127.0.0.1");
            netConfig.remotePort = cmd.value("remotePort", 4465);

            // Init codec
            encoder = std::make_unique<CeltCodec>();
            decoder = std::make_unique<CeltCodec>();

            if (!encoder->init(celtConfig)) {
                jsonError("Failed to initialize CELT encoder");
                continue;
            }
            if (!decoder->init(celtConfig)) {
                jsonError("Failed to initialize CELT decoder");
                continue;
            }

            // Init network
            network = std::make_unique<UdpTransport>();
            if (!network->init(netConfig)) {
                jsonError("Failed to initialize network");
                continue;
            }

            // Init audio
            audio = std::make_unique<WasapiAudioHandler>();
            if (!audio->init(audioConfig)) {
                jsonError("Failed to initialize WASAPI audio");
                continue;
            }

            // Setup buffers
            celtFrameSize = celtConfig.frameSize;
            maxEncoded = encoder->getEncodedFrameSize() + 16;
            encodeBuffer.resize(maxEncoded);
            decodeBuffer.resize(celtConfig.frameSize * celtConfig.channels);
            captureAccum.clear();
            captureAccum.reserve(audio->getActualBufferSize() + celtFrameSize);
            lastRecvSeq = 0;

            // Wire capture callback
            audio->setCaptureCallback([&](const float* samples, int count) {
                captureAccum.insert(captureAccum.end(), samples, samples + count);
                while ((int)captureAccum.size() >= celtFrameSize) {
                    int encoded = encoder->encode(captureAccum.data(), encodeBuffer.data(), maxEncoded);
                    if (encoded > 0) {
                        network->send(encodeBuffer.data(), encoded);
                    }
                    captureAccum.erase(captureAccum.begin(), captureAccum.begin() + celtFrameSize);
                }
            });

            // Wire receive callback
            network->setReceiveCallback([&](const uint8_t* data, int length, uint32_t seq) {
                if (lastRecvSeq > 0 && seq > lastRecvSeq + 1) {
                    int lost = seq - lastRecvSeq - 1;
                    for (int i = 0; i < lost && i < 3; i++) {
                        decoder->decodePLC(decodeBuffer.data());
                        audio->writePlayoutSamples(decodeBuffer.data(), celtFrameSize);
                    }
                }
                lastRecvSeq = seq;
                int decoded = decoder->decode(data, length, decodeBuffer.data());
                if (decoded > 0) {
                    audio->writePlayoutSamples(decodeBuffer.data(), decoded);
                }
            });

            // Start
            network->start();
            if (!audio->start()) {
                jsonError("Failed to start WASAPI audio");
                network->stop();
                continue;
            }

            audioRunning = true;
            int actualBufSize = audio->getActualBufferSize();
            double frameMs = (double)actualBufSize / audioConfig.sampleRate * 1000.0;

            jsonOut({
                {"type", "started"},
                {"bufferSize", actualBufSize},
                {"celtFrameSize", celtFrameSize},
                {"latencyMs", frameMs * 2 + 0.2},
                {"remoteHost", netConfig.remoteHost},
                {"remotePort", netConfig.remotePort},
                {"localPort", netConfig.localPort}
            });

            // Start stats reporting thread
            statsRunning = true;
            statsThread = std::thread([&]() {
                while (statsRunning && g_running) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    if (!statsRunning || !audio || !network) break;
                    auto audioStats = audio->getStats();
                    auto netStats = network->getStats();
                    jsonOut({
                        {"type", "stats"},
                        {"sent", netStats.packetsSent},
                        {"recv", netStats.packetsReceived},
                        {"lost", netStats.packetsLost},
                        {"underruns", audioStats.underruns},
                        {"latencyMs", audioStats.inputLatencyMs + audioStats.outputLatencyMs}
                    });
                }
            });
        }
        // ----- STOP -----
        else if (cmdType == "stop") {
            stopAudio();
        }
        // ----- QUIT -----
        else if (cmdType == "quit") {
            stopAudio();
            break;
        }
        else {
            jsonError("Unknown command: " + cmdType);
        }
    }

    stopAudio();
    return 0;
}

// =============================================================================
// CLI MODE — original interactive behavior (unchanged)
// =============================================================================
void printUsage(const char* argv0) {
    std::cout << "Usage: " << argv0 << " [options]\n"
              << "\n"
              << "Options:\n"
              << "  --local-port PORT     Local UDP port (default: 4464)\n"
              << "  --remote-host HOST    Remote peer IP (default: 127.0.0.1)\n"
              << "  --remote-port PORT    Remote peer port (default: 4465)\n"
              << "  --buffer-size N       Buffer size in samples (default: 128)\n"
              << "  --bitrate N           CELT bitrate in bps (default: 64000)\n"
              << "  --list-devices        List available audio devices and exit\n"
              << "  --json-mode           Run in JSON mode for GUI integration\n"
              << "\n"
              << "Example (two instances on localhost):\n"
              << "  Instance A: musiconnect --local-port 4464 --remote-port 4465\n"
              << "  Instance B: musiconnect --local-port 4465 --remote-port 4464\n"
              << std::endl;
}

int runCliMode(int argc, char* argv[]) {
    // Defaults
    AudioConfig audioConfig;
    audioConfig.sampleRate = 48000;
    audioConfig.bufferSize = 128;

    CeltConfig celtConfig;
    celtConfig.sampleRate = 48000;
    celtConfig.frameSize = 128;
    celtConfig.channels = 1;
    celtConfig.bitrate = 64000;
    celtConfig.complexity = 1;

    NetworkConfig netConfig;
    netConfig.localPort = 4464;
    netConfig.remoteHost = "";
    netConfig.remotePort = 4465;

    // Parse command line
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") { printUsage(argv[0]); return 0; }
        else if (arg == "--list-devices") {
            auto devices = WasapiAudioHandler::listDevices();
            std::cout << "Available audio devices:" << std::endl;
            for (size_t j = 0; j < devices.size(); j++) {
                std::cout << "  [" << j << "] " << devices[j] << std::endl;
            }
            if (devices.empty()) std::cout << "  (none found)" << std::endl;
            return 0;
        }
        else if (arg == "--json-mode") { /* handled in main() */ }
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

    // Prompt for remote IP if not provided
    if (netConfig.remoteHost.empty()) {
        std::cout << "Enter remote peer IP address: ";
        std::getline(std::cin, netConfig.remoteHost);
        while (!netConfig.remoteHost.empty() && netConfig.remoteHost.back() == ' ')
            netConfig.remoteHost.pop_back();
        while (!netConfig.remoteHost.empty() && netConfig.remoteHost.front() == ' ')
            netConfig.remoteHost.erase(netConfig.remoteHost.begin());
        if (netConfig.remoteHost.empty()) {
            std::cerr << "Error: no IP address provided." << std::endl;
            return 1;
        }
    }

    SetConsoleCtrlHandler(consoleHandler, TRUE);

    std::cout << "\n===========================================================\n"
              << "  MusiConnect - Ultra Low Latency P2P Audio (Windows WASAPI)\n"
              << "===========================================================\n\n";

    // Initialize CELT codec
    CeltCodec encoder, decoder;
    if (!encoder.init(celtConfig)) { std::cerr << "Failed to initialize CELT encoder\n"; return 1; }
    if (!decoder.init(celtConfig)) { std::cerr << "Failed to initialize CELT decoder\n"; return 1; }

    // Initialize network
    UdpTransport network;
    if (!network.init(netConfig)) { std::cerr << "Failed to initialize network\n"; return 1; }

    // Select input device
    {
        auto inputDevices = WasapiAudioHandler::listInputDevices();
        if (inputDevices.empty()) { std::cerr << "No input devices found!\n"; return 1; }
        std::cout << "Available INPUT devices:\n";
        for (size_t j = 0; j < inputDevices.size(); j++)
            std::cout << "  [" << j << "] " << inputDevices[j] << "\n";
        std::cout << "Select input device [0]: ";
        std::string line; std::getline(std::cin, line);
        if (!line.empty()) {
            int idx = std::stoi(line);
            if (idx >= 0 && idx < (int)inputDevices.size()) audioConfig.inputDeviceIndex = idx;
        } else { audioConfig.inputDeviceIndex = 0; }
    }

    // Select output device
    {
        auto outputDevices = WasapiAudioHandler::listOutputDevices();
        if (outputDevices.empty()) { std::cerr << "No output devices found!\n"; return 1; }
        std::cout << "\nAvailable OUTPUT devices:\n";
        for (size_t j = 0; j < outputDevices.size(); j++)
            std::cout << "  [" << j << "] " << outputDevices[j] << "\n";
        std::cout << "Select output device [0]: ";
        std::string line; std::getline(std::cin, line);
        if (!line.empty()) {
            int idx = std::stoi(line);
            if (idx >= 0 && idx < (int)outputDevices.size()) audioConfig.outputDeviceIndex = idx;
        } else { audioConfig.outputDeviceIndex = 0; }
    }

    // Initialize WASAPI audio
    WasapiAudioHandler audio;
    if (!audio.init(audioConfig)) { std::cerr << "Failed to initialize WASAPI audio\n"; return 1; }

    int actualBufSize = audio.getActualBufferSize();
    int celtFrameSize = celtConfig.frameSize;
    int maxEncoded = encoder.getEncodedFrameSize() + 16;
    std::vector<uint8_t> encodeBuffer(maxEncoded);
    std::vector<float> captureAccum;
    captureAccum.reserve(actualBufSize + celtFrameSize);
    std::vector<float> decodeBuffer(celtConfig.frameSize * celtConfig.channels);
    uint32_t lastRecvSeq = 0;

    // Wire callbacks
    audio.setCaptureCallback([&](const float* samples, int count) {
        captureAccum.insert(captureAccum.end(), samples, samples + count);
        while ((int)captureAccum.size() >= celtFrameSize) {
            int encoded = encoder.encode(captureAccum.data(), encodeBuffer.data(), maxEncoded);
            if (encoded > 0) network.send(encodeBuffer.data(), encoded);
            captureAccum.erase(captureAccum.begin(), captureAccum.begin() + celtFrameSize);
        }
    });

    network.setReceiveCallback([&](const uint8_t* data, int length, uint32_t seq) {
        if (lastRecvSeq > 0 && seq > lastRecvSeq + 1) {
            int lost = seq - lastRecvSeq - 1;
            for (int i = 0; i < lost && i < 3; i++) {
                decoder.decodePLC(decodeBuffer.data());
                audio.writePlayoutSamples(decodeBuffer.data(), celtFrameSize);
            }
        }
        lastRecvSeq = seq;
        int decoded = decoder.decode(data, length, decodeBuffer.data());
        if (decoded > 0) audio.writePlayoutSamples(decodeBuffer.data(), decoded);
    });

    // Start
    network.start();
    if (!audio.start()) { std::cerr << "Failed to start WASAPI audio\n"; return 1; }

    double frameMs = (double)actualBufSize / audioConfig.sampleRate * 1000.0;
    std::cout << "\n-----------------------------------------------------------\n"
              << "  RUNNING - Press Ctrl+C to stop\n"
              << "-----------------------------------------------------------\n"
              << "  WASAPI buffer:     " << actualBufSize << " samples (" << frameMs << "ms)\n"
              << "  CELT frame:        " << celtFrameSize << " samples ("
              << encoder.getFrameDurationMs() << "ms)\n"
              << "  Local:             0.0.0.0:" << netConfig.localPort << "\n"
              << "  Remote:            " << netConfig.remoteHost << ":" << netConfig.remotePort << "\n"
              << "  Theoretical latency: ~" << (frameMs * 2 + 0.2) << "ms\n"
              << "-----------------------------------------------------------\n\n";

    // Stats loop
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        auto audioStats = audio.getStats();
        auto netStats = network.getStats();
        std::cout << "\r[STATS] Sent: " << netStats.packetsSent
                  << " | Recv: " << netStats.packetsReceived
                  << " | Lost: " << netStats.packetsLost
                  << " | Underruns: " << audioStats.underruns
                  << "    " << std::flush;
    }

    std::cout << "\n\nShutting down...\n";
    audio.stop();
    network.stop();
    std::cout << "Done.\n";
    return 0;
}

// =============================================================================
// ENTRY POINT
// =============================================================================
int main(int argc, char* argv[]) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "Failed to initialize Winsock\n";
        return 1;
    }

    // Check for --json-mode flag
    bool jsonMode = false;
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--json-mode") {
            jsonMode = true;
            break;
        }
    }

    int result;
    if (jsonMode) {
        result = runJsonMode();
    } else {
        result = runCliMode(argc, argv);
    }

    WSACleanup();
    CoUninitialize();
    return result;
}
