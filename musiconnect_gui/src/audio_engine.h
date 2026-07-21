#pragma once
// =============================================================================
// AUDIO ENGINE — Bridges the GUI to the audio/codec/network pipeline
//
// This wraps the entire pipeline (audio capture/playout, CELT codec, UDP
// transport) into a simple start/stop interface that the GUI controls.
// =============================================================================

#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <utility>

struct EngineConfig {
    // Audio
    std::string audioDevice;     // Device/driver name (empty = default)
    int sampleRate = 48000;
    int bufferSize = 64;

    // Codec
    int bitrate = 64000;

    // Network
    std::string remoteHost = "127.0.0.1";
    int remotePort = 4465;
    int localPort = 4464;
};

struct EngineStats {
    uint64_t packetsSent = 0;
    uint64_t packetsReceived = 0;
    uint64_t packetsLost = 0;
    uint64_t underruns = 0;
    int actualBufferSize = 0;
    double latencyMs = 0.0;
    bool connected = false;
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    // List available audio devices/drivers for the current platform
    static std::vector<std::string> listDevices();

    // Get the names of the system's current default input and output devices
    // Returns: {inputDeviceName, outputDeviceName}
    static std::pair<std::string, std::string> getDefaultDeviceNames();

    // Start the full pipeline with given config
    bool start(const EngineConfig& config);

    // Stop the pipeline
    void stop();

    // Is the engine currently running?
    bool isRunning() const;

    // Get current stats (safe to call from GUI thread)
    EngineStats getStats() const;

    // Get last error message
    std::string getLastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
