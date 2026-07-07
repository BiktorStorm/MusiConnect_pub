#include "celt_codec.h"
#include "opus_custom.h"
#include <iostream>
#include <vector>

CeltCodec::CeltCodec() = default;

CeltCodec::~CeltCodec() {
    if (m_encoder) opus_custom_encoder_destroy(static_cast<OpusCustomEncoder*>(m_encoder));
    if (m_decoder) opus_custom_decoder_destroy(static_cast<OpusCustomDecoder*>(m_decoder));
    if (m_mode)    opus_custom_mode_destroy(static_cast<OpusCustomMode*>(m_mode));
}

bool CeltCodec::init(const CeltConfig& config) {
    m_config = config;
    int err;

    auto* mode = opus_custom_mode_create(config.sampleRate, config.frameSize, &err);
    if (!mode) { std::cerr << "[CELT] mode failed: " << opus_strerror(err) << "\n"; return false; }
    m_mode = mode;

    auto* enc = opus_custom_encoder_create(mode, config.channels, &err);
    if (!enc) { std::cerr << "[CELT] encoder failed\n"; return false; }
    m_encoder = enc;

    auto* dec = opus_custom_decoder_create(mode, config.channels, &err);
    if (!dec) { std::cerr << "[CELT] decoder failed\n"; return false; }
    m_decoder = dec;

    opus_custom_encoder_ctl(enc, OPUS_SET_VBR(0));
    opus_custom_encoder_ctl(enc, OPUS_SET_BITRATE(config.bitrate));
    opus_custom_encoder_ctl(enc, OPUS_SET_APPLICATION(OPUS_APPLICATION_RESTRICTED_LOWDELAY));
    opus_custom_encoder_ctl(enc, OPUS_SET_COMPLEXITY(config.complexity));

    // Determine CBR frame size
    std::vector<float> silence(config.frameSize * config.channels, 0.0f);
    uint8_t tmp[512];
    m_encodedSize = opus_custom_encode_float(enc, silence.data(), config.frameSize, tmp, sizeof(tmp));
    if (m_encodedSize <= 0) { std::cerr << "[CELT] encode size probe failed\n"; return false; }

    std::cout << "[CELT] " << config.frameSize << " samples (" << getFrameMs() << "ms), "
              << config.bitrate / 1000 << "kbps, " << m_encodedSize << " bytes/frame\n";
    return true;
}

int CeltCodec::encode(const float* pcm, uint8_t* output, int maxBytes) {
    if (!m_encoder) return -1;
    return opus_custom_encode_float(
        static_cast<OpusCustomEncoder*>(m_encoder), pcm, m_config.frameSize, output, maxBytes);
}

int CeltCodec::decode(const uint8_t* data, int len, float* pcm) {
    if (!m_decoder) return -1;
    return opus_custom_decode_float(
        static_cast<OpusCustomDecoder*>(m_decoder), data, len, pcm, m_config.frameSize);
}
