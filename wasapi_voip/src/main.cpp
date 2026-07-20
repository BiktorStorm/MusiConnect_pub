// =============================================================================
// MUSICONNECT — Ultra Low Latency P2P Audio (Windows WASAPI)
//
// This ties together:
//   1. WASAPI exclusive mode (capture + playout at ~64-96 samples)
//   2. CELT codec (encode/decode with zero algorithmic delay)
//   3. UDP transport (send/receive with no retransmission)
//
// DATA FLOW:
//   WASAPI capture thread → CELT encode → UDP send →
//   → UDP receive → CELT decode → WASAPI playout ring buffer
//
// LATENCY BUDGET (localhost):
//   Capture:  ~1.3ms (64 samples @ 48kHz, device-dependent)
//   Encode:   ~0.05ms
//   Network:  ~0.1ms (localhost)
//   Decode:   ~0.05ms
//   Playout:  ~1.3ms (64 samples)
//   ─────────────────────────
//   TOTAL:    ~3ms
//
// With a real network (LAN): add RTT/2 (~0.5ms LAN, 5-50ms internet)
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

#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <csignal>
#include <vector>

// Global shutdown flag
static std::atomic<bool> g_running{true};

BOOL WINAPI consoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
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
    // Initialize COM for WASAPI
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // Initialize Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "Failed to initialize Winsock" << std::endl;
        return 1;
    }

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
            CoUninitialize();
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

    // Prompt for remote IP if not provided via command line
    if (netConfig.remoteHost.empty()) {
        std::cout << "Enter remote peer IP address: ";
        std::getline(std::cin, netConfig.remoteHost);
        // Trim whitespace
        while (!netConfig.remoteHost.empty() && netConfig.remoteHost.back() == ' ')
            netConfig.remoteHost.pop_back();
        while (!netConfig.remoteHost.empty() && netConfig.remoteHost.front() == ' ')
            netConfig.remoteHost.erase(netConfig.remoteHost.begin());
        if (netConfig.remoteHost.empty()) {
            std::cerr << "Error: no IP address provided." << std::endl;
            return 1;
        }
    }

    // Handle Ctrl+C gracefully
    SetConsoleCtrlHandler(consoleHandler, TRUE);

    std::cout << std::endl;
    std::cout << "===========================================================" << std::endl;
    std::cout << "  MusiConnect - Ultra Low Latency P2P Audio (Windows WASAPI)" << std::endl;
    std::cout << "===========================================================" << std::endl;
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
    // 3. Initialize WASAPI audio
    // =========================================================================

    // --- Select input device ---
    {
        auto inputDevices = WasapiAudioHandler::listInputDevices();
        if (inputDevices.empty()) {
            std::cerr << "No input (capture) devices found!" << std::endl;
            return 1;
        }
        std::cout << std::endl;
        std::cout << "Available INPUT devices:" << std::endl;
        for (size_t j = 0; j < inputDevices.size(); j++) {
            std::cout << "  [" << j << "] " << inputDevices[j] << std::endl;
        }
        std::cout << "Select input device [0]: ";
        std::string line;
        std::getline(std::cin, line);
        if (!line.empty()) {
            int idx = std::stoi(line);
            if (idx >= 0 && idx < (int)inputDevices.size()) {
                audioConfig.inputDeviceIndex = idx;
            } else {
                std::cerr << "Invalid selection, using default." << std::endl;
            }
        } else {
            audioConfig.inputDeviceIndex = 0;
        }
    }

    // --- Select output device ---
    {
        auto outputDevices = WasapiAudioHandler::listOutputDevices();
        if (outputDevices.empty()) {
            std::cerr << "No output (render) devices found!" << std::endl;
            return 1;
        }
        std::cout << std::endl;
        std::cout << "Available OUTPUT devices:" << std::endl;
        for (size_t j = 0; j < outputDevices.size(); j++) {
            std::cout << "  [" << j << "] " << outputDevices[j] << std::endl;
        }
        std::cout << "Select output device [0]: ";
        std::string line;
        std::getline(std::cin, line);
        if (!line.empty()) {
            int idx = std::stoi(line);
            if (idx >= 0 && idx < (int)outputDevices.size()) {
                audioConfig.outputDeviceIndex = idx;
            } else {
                std::cerr << "Invalid selection, using default." << std::endl;
            }
        } else {
            audioConfig.outputDeviceIndex = 0;
        }
    }

    std::cout << std::endl;

    WasapiAudioHandler audio;
    if (!audio.init(audioConfig)) {
        std::cerr << "Failed to initialize WASAPI audio" << std::endl;
        return 1;
    }

    // Report buffer size mismatch (common on shared mode — not an error)
    int actualBufSize = audio.getActualBufferSize();
    if (actualBufSize != celtConfig.frameSize) {
        std::cout << "[INFO] WASAPI buffer (" << actualBufSize
                  << ") != CELT frame size (" << celtConfig.frameSize << ")" << std::endl;
        std::cout << "       Will encode/decode in " << celtConfig.frameSize
                  << "-sample chunks." << std::endl;
    }


    // =========================================================================
    // 4. Wire up the pipeline
    // =========================================================================

    // Encode buffer (allocated once, reused every callback)
    int maxEncoded = encoder.getEncodedFrameSize() + 16;  // Some headroom
    std::vector<uint8_t> encodeBuffer(maxEncoded);

    // Accumulation buffer for capture (handles WASAPI giving us more samples
    // than one CELT frame at a time)
    int celtFrameSize = celtConfig.frameSize;
    std::vector<float> captureAccum;
    captureAccum.reserve(actualBufSize + celtFrameSize);

    // CAPTURE → ENCODE → SEND
    // WASAPI may deliver buffers larger than celtFrameSize (e.g., 1056 samples
    // in shared mode). We accumulate and encode in celtFrameSize chunks.
    audio.setCaptureCallback([&](const float* samples, int count) {
        // Append new samples to accumulation buffer
        captureAccum.insert(captureAccum.end(), samples, samples + count);

        // Encode as many complete CELT frames as we have
        while ((int)captureAccum.size() >= celtFrameSize) {
            int encoded = encoder.encode(captureAccum.data(), encodeBuffer.data(), maxEncoded);
            if (encoded > 0) {
                network.send(encodeBuffer.data(), encoded);
            }
            // Remove the consumed samples
            captureAccum.erase(captureAccum.begin(), captureAccum.begin() + celtFrameSize);
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
            // Write to WASAPI playout buffer
            audio.writePlayoutSamples(decodeBuffer.data(), decoded);
        }
    });

    // =========================================================================
    // 5. Start everything
    // =========================================================================
    network.start();

    if (!audio.start()) {
        std::cerr << "Failed to start WASAPI audio" << std::endl;
        return 1;
    }

    double frameMs = (double)actualBufSize / audioConfig.sampleRate * 1000.0;
    std::cout << std::endl;
    std::cout << "-----------------------------------------------------------" << std::endl;
    std::cout << "  RUNNING - Press Ctrl+C to stop" << std::endl;
    std::cout << "-----------------------------------------------------------" << std::endl;
    std::cout << "  WASAPI buffer:     " << actualBufSize << " samples (" << frameMs << "ms)" << std::endl;
    std::cout << "  CELT frame:        " << celtConfig.frameSize << " samples ("
              << encoder.getFrameDurationMs() << "ms)" << std::endl;
    std::cout << "  Encoded size:      " << encoder.getEncodedFrameSize() << " bytes/frame" << std::endl;
    std::cout << "  Local:             0.0.0.0:" << netConfig.localPort << std::endl;
    std::cout << "  Remote:            " << netConfig.remoteHost << ":" << netConfig.remotePort << std::endl;
    std::cout << "  Theoretical latency: ~" << (frameMs * 2 + 0.2) << "ms (capture + playout + codec)" << std::endl;
    std::cout << "-----------------------------------------------------------" << std::endl;
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

    WSACleanup();
    CoUninitialize();

    std::cout << "Done." << std::endl;
    return 0;
}
