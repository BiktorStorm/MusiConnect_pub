#pragma once
#include "audio.h"

#ifdef USE_COREAUDIO

class CoreAudioDevice : public AudioDevice {
public:
    CoreAudioDevice();
    ~CoreAudioDevice() override;

    std::vector<std::string> listDrivers() override;
    bool init(const AudioConfig& config) override;
    bool start() override;
    void stop() override;

    void setCaptureCallback(CaptureCallback cb, void* userData) override;
    void writePlayoutSamples(const float* samples, int count) override;
    int getBufferSize() const override;
    Stats getStats() const override;

private:
    struct Impl;
    Impl* m_impl;
};

#endif
