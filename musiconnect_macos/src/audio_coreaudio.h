#pragma once
// =============================================================================
// CORE AUDIO HANDLER (macOS)
//
// Manages the Core Audio AudioUnit lifecycle:
//   - Enumerate available audio devices
//   - Open the default (or specified) device with low-latency buffer size
//   - Provide capture (input) and playout (output) via render callbacks
//   - Feed captured audio to a callback for encoding
//   - Accept decoded audio for playout via ring buffer
//
// Core Audio's AudioUnit render callback runs on a realtime thread.
//   - Never block (no locks, no allocation, no I/O)
//   - Never take longer than the buffer duration
//   - Only read/write to lock-free data structures
// =============================================================================

#include "ring_buffer.h"
#include <functional>
#include <string>
#include <vector>
#include <cstdint>

// Callback type: called from Core Audio render thread with captured audio
// Parameters: pointer to float samples, number of samples
using CaptureCallback = std::function<void(const float*, int)>;

struct AudioConfig {
    int sampleRate = 48000;
    int bufferSize = 64;      // Requested buffer size in samples (64 = 1.33ms)
    int inputChannel = 0;     // Which input channel to capture
    int outputChannel = 0;    // Which output channel to play on
    std::string deviceName;   // Audio device name (empty = default device)
};

class CoreAudioHandler {
public:
    CoreAudioHandler();
    ~CoreAudioHandler();

    // List available audio devices
    static std::vector<std::string> listDevices();

    // Initialize with config
    bool init(const AudioConfig& config);

    // Start audio streaming
    bool start();

    // Stop audio streaming
    void stop();

    // Set callback for captured audio (called from Core Audio render thread!)
    void setCaptureCallback(CaptureCallback cb);

    // Write decoded audio for playout (thread-safe, called from network thread)
    void writePlayoutSamples(const float* samples, int count);

    // Get current stats
    struct Stats {
        int bufferSize;
        int sampleRate;
        double inputLatencyMs;
        double outputLatencyMs;
        uint64_t capturedFrames;
        uint64_t playedFrames;
        uint64_t underruns;
        uint64_t overflows;
    };
    Stats getStats() const;

    // Get the actual buffer size the system chose (may differ from requested)
    int getActualBufferSize() const;

private:
    struct Impl;
    Impl* m_impl;
};
