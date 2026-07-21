#pragma once
// =============================================================================
// WASAPI AUDIO HANDLER (Windows)
//
// Manages the Windows Audio Session API (WASAPI) lifecycle:
//   - Enumerate available audio devices
//   - Open default (or specified) device in exclusive mode for low latency
//   - Provide capture (input) and playout (output) via event-driven threads
//   - Feed captured audio to a callback for encoding
//   - Accept decoded audio for playout via ring buffer
//
// WASAPI exclusive mode bypasses the Windows audio engine (mixer/APO),
// giving direct hardware access with buffer sizes as low as ~40 samples.
//
// The capture and playout threads run at MMCSS "Pro Audio" priority.
// Same constraints as Core Audio / ASIO:
//   - Callbacks should not block, allocate, or do heavy I/O
//   - Only read/write to lock-free data structures
// =============================================================================

#include "ring_buffer.h"
#include <functional>
#include <string>
#include <vector>
#include <cstdint>

// Callback type: called from WASAPI capture thread with captured audio
// Parameters: pointer to float samples (mono), number of samples
using CaptureCallback = std::function<void(const float*, int)>;

struct AudioConfig {
    int sampleRate = 48000;
    int bufferSize = 128;     // Requested buffer size in samples (128 = 2.67ms)
    int inputChannel = 0;     // Which input channel to capture (from multi-ch device)
    int outputChannel = 0;    // Which output channel to play on
    int inputDeviceIndex = -1;  // -1 = default device, otherwise index from listInputDevices()
    int outputDeviceIndex = -1; // -1 = default device, otherwise index from listOutputDevices()
};

class WasapiAudioHandler {
public:
    WasapiAudioHandler();
    ~WasapiAudioHandler();

    // List all audio devices (input + output combined)
    static std::vector<std::string> listDevices();

    // List input (capture) devices only
    static std::vector<std::string> listInputDevices();

    // List output (render) devices only
    static std::vector<std::string> listOutputDevices();

    // Initialize with config
    bool init(const AudioConfig& config);

    // Start audio streaming
    bool start();

    // Stop audio streaming
    void stop();

    // Set callback for captured audio (called from WASAPI capture thread!)
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
