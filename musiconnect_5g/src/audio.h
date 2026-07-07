#pragma once
// Platform-agnostic audio device interface.
// Windows: ASIO (audio_asio.cpp)
// macOS:   CoreAudio AudioUnit (audio_coreaudio.cpp)

#include "ring_buffer.h"
#include <string>
#include <vector>
#include <cstdint>

using CaptureCallback = void(*)(const float* samples, int count, void* userData);

struct AudioConfig {
    int sampleRate = 48000;
    int bufferSize = 64;
    int inputChannel = 0;
    int outputChannel = 0;
    std::string driverName;
};

class AudioDevice {
public:
    virtual ~AudioDevice() = default;

    virtual std::vector<std::string> listDrivers() = 0;
    virtual bool init(const AudioConfig& config) = 0;
    virtual bool start() = 0;
    virtual void stop() = 0;

    virtual void setCaptureCallback(CaptureCallback cb, void* userData) = 0;
    virtual void writePlayoutSamples(const float* samples, int count) = 0;
    virtual int getBufferSize() const = 0;

    struct Stats {
        uint64_t captured = 0;
        uint64_t played = 0;
        uint64_t underruns = 0;
    };
    virtual Stats getStats() const = 0;
};

// Factory — returns the correct backend for the current platform
AudioDevice* createAudioDevice();
