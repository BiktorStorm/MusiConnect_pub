#pragma once
// CELT codec wrapper using Opus Custom Mode.
// Configured for minimum latency: small frames, low complexity, CBR.

#include <cstdint>

struct CeltConfig {
    int sampleRate = 48000;
    int frameSize = 64;       // samples per frame (64 = 1.33ms @ 48kHz)
    int channels = 1;
    int bitrate = 64000;      // bits per second
    int complexity = 1;       // 0-10, lower = faster encode
};

class CeltCodec {
public:
    CeltCodec();
    ~CeltCodec();

    bool init(const CeltConfig& config);
    int encode(const float* pcm, uint8_t* output, int maxBytes);
    int decode(const uint8_t* data, int len, float* pcm);

    int getFrameSize() const { return m_config.frameSize; }
    int getEncodedSize() const { return m_encodedSize; }
    double getFrameMs() const { return (double)m_config.frameSize / m_config.sampleRate * 1000.0; }

private:
    CeltConfig m_config{};
    void* m_mode = nullptr;
    void* m_encoder = nullptr;
    void* m_decoder = nullptr;
    int m_encodedSize = 0;
};
