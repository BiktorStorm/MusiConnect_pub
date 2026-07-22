// =============================================================================
// CELT CODEC IMPLEMENTATION
// =============================================================================

#include "celt_codec.h"
#include "opus_custom.h"
#include <iostream>
#include <cstring>

CeltCodec::CeltCodec() = default;

CeltCodec::~CeltCodec() {
    if (m_encoder) opus_custom_encoder_destroy(static_cast<OpusCustomEncoder*>(m_encoder));
    if (m_decoder) opus_custom_decoder_destroy(static_cast<OpusCustomDecoder*>(m_decoder));
    if (m_mode)    opus_custom_mode_destroy(static_cast<OpusCustomMode*>(m_mode));
}

bool CeltCodec::init(const CeltConfig& config) {
    m_config = config;
    int err;

    // Create custom mode with our exact frame size
    auto* mode = opus_custom_mode_create(config.sampleRate, config.frameSize, &err);
    if (err != OPUS_OK || !mode) {
        std::cerr << "[CELT] Failed to create custom mode: " << opus_strerror(err) << std::endl;
        return false;
    }
    m_mode = mode;

    // Create encoder
    auto* encoder = opus_custom_encoder_create(mode, config.channels, &err);
    if (err != OPUS_OK || !encoder) {
        std::cerr << "[CELT] Failed to create encoder: " << opus_strerror(err) << std::endl;
        return false;
    }
    m_encoder = encoder;

    // Create decoder
    auto* decoder = opus_custom_decoder_create(mode, config.channels, &err);
    if (err != OPUS_OK || !decoder) {
        std::cerr << "[CELT] Failed to create decoder: " << opus_strerror(err) << std::endl;
        return false;
    }
    m_decoder = decoder;

    // Configure encoder (same settings as Jamulus)
    opus_custom_encoder_ctl(encoder, OPUS_SET_VBR(0));                                    // CBR
    opus_custom_encoder_ctl(encoder, OPUS_SET_BITRATE(config.bitrate));                   // Target bitrate
    opus_custom_encoder_ctl(encoder, OPUS_SET_APPLICATION(OPUS_APPLICATION_RESTRICTED_LOWDELAY)); // CELT only
    opus_custom_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(config.complexity));             // Low CPU

    if (config.frameSize <= 128) {
        opus_custom_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(config.packetLossPercent));
    }

    // Determine encoded frame size by encoding a silent frame
    std::vector<float> silence(config.frameSize * config.channels, 0.0f);
    uint8_t tempOutput[512];
    m_encodedFrameSize = opus_custom_encode_float(encoder, silence.data(), config.frameSize, tempOutput, sizeof(tempOutput));
    if (m_encodedFrameSize <= 0) {
        std::cerr << "[CELT] Failed to determine encoded frame size" << std::endl;
        return false;
    }

    std::cout << "[CELT] Initialized: " << config.frameSize << " samples/frame (" << getFrameDurationMs() << "ms), " << config.bitrate / 1000 << "kbps, " << m_encodedFrameSize << " bytes/frame, " << "complexity=" << config.complexity << std::endl;

    return true;
}

int CeltCodec::encode(const float* pcm, uint8_t* output, int maxOutputBytes) {
    if (!m_encoder) return -1;

    int encoded = opus_custom_encode_float(static_cast<OpusCustomEncoder*>(m_encoder), pcm, m_config.frameSize, output, maxOutputBytes);

    return encoded;
}

int CeltCodec::decode(const uint8_t* encoded, int encodedLen, float* pcm) {
    if (!m_decoder) return -1;

    int decoded = opus_custom_decode_float(static_cast<OpusCustomDecoder*>(m_decoder), encoded, encodedLen, pcm, m_config.frameSize);

    return decoded;
}

int CeltCodec::decodePLC(float* pcm) {
    if (!m_decoder) return -1;

    // Passing NULL for data triggers packet loss concealment
    int decoded = opus_custom_decode_float(static_cast<OpusCustomDecoder*>(m_decoder), nullptr, 0, pcm, m_config.frameSize);

    return decoded;
}
