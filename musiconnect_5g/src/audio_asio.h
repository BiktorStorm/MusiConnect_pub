#pragma once
// ASIO audio handler — direct hardware access, bypasses OS mixer.
// Capture callback uses a raw function pointer (no std::function on RT thread).

#include "ring_buffer.h"
#include <string>
#include <vector>
#include <cstdint>

// Raw function pointer — safe to call from ASIO realtime thread
using CaptureCallback = void(*)(const float* samples, int count, void* userData);

struct AudioConfig {
    int sampleRate = 48000;
    int bufferSize = 64;
    int inputChannel = 0;
    int outputChannel = 0;
    std::string driverName;
};

class AsioAudio {
public:
    AsioAudio();
    ~AsioAudio();

    static std::vector<std::string> listDrivers();

    bool init(const AudioConfig& config);
    bool start();
    void stop();

    void setCaptureCallback(CaptureCallback cb, void* userData);
    void writePlayoutSamples(const float* samples, int count);

    int getBufferSize() const;

    struct Stats {
        uint64_t captured = 0;
        uint64_t played = 0;
        uint64_t underruns = 0;
    };
    Stats getStats() const;

private:
    struct Impl;
    Impl* m_impl;
};
