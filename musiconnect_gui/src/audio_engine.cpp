// =============================================================================
// AUDIO ENGINE — Implementation
//
// This is the same pipeline logic from main.cpp, wrapped in a class so the
// GUI can start/stop it without command-line args.
// =============================================================================

#include "audio_engine.h"

#ifdef _WIN32
#include "audio_asio.h"
#else
#include "audio_coreaudio.h"
#endif

#include "celt_codec.h"
#include "network.h"

#include <vector>
#include <mutex>

struct AudioEngine::Impl {
    std::atomic<bool> running{false};
    std::string lastError;

    std::unique_ptr<CeltCodec> encoder;
    std::unique_ptr<CeltCodec> decoder;
    std::unique_ptr<UdpTransport> network;

#ifdef _WIN32
    std::unique_ptr<AsioAudioHandler> audio;
#else
    std::unique_ptr<CoreAudioHandler> audio;
#endif

    // Pipeline buffers
    std::vector<uint8_t> encodeBuffer;
    std::vector<float> decodeBuffer;
    uint32_t lastRecvSeq = 0;

    // Stats
    std::atomic<int> statActualBufferSize{0};
};

AudioEngine::AudioEngine() : m_impl(std::make_unique<Impl>()) {}
AudioEngine::~AudioEngine() { stop(); }

std::vector<std::string> AudioEngine::listDevices() {
#ifdef _WIN32
    return AsioAudioHandler::listDrivers();
#else
    return CoreAudioHandler::listDevices();
#endif
}

bool AudioEngine::start(const EngineConfig& config) {
    if (m_impl->running) {
        m_impl->lastError = "Engine already running";
        return false;
    }

    // Reset state
    m_impl->lastRecvSeq = 0;
    m_impl->lastError.clear();

    // =========================================================================
    // 1. Initialize CELT codec
    // =========================================================================
    CeltConfig celtConfig;
    celtConfig.sampleRate = config.sampleRate;
    celtConfig.frameSize = config.bufferSize;
    celtConfig.channels = 1;
    celtConfig.bitrate = config.bitrate;
    celtConfig.complexity = 1;

    m_impl->encoder = std::make_unique<CeltCodec>();
    m_impl->decoder = std::make_unique<CeltCodec>();

    if (!m_impl->encoder->init(celtConfig)) {
        m_impl->lastError = "Failed to initialize CELT encoder";
        return false;
    }
    if (!m_impl->decoder->init(celtConfig)) {
        m_impl->lastError = "Failed to initialize CELT decoder";
        return false;
    }

    // =========================================================================
    // 2. Initialize network
    // =========================================================================
    NetworkConfig netConfig;
    netConfig.localPort = config.localPort;
    netConfig.remoteHost = config.remoteHost;
    netConfig.remotePort = config.remotePort;

    m_impl->network = std::make_unique<UdpTransport>();
    if (!m_impl->network->init(netConfig)) {
        m_impl->lastError = "Failed to initialize network (port " +
                            std::to_string(config.localPort) + " may be in use)";
        return false;
    }

    // =========================================================================
    // 3. Initialize audio
    // =========================================================================
    AudioConfig audioConfig;
    audioConfig.sampleRate = config.sampleRate;
    audioConfig.bufferSize = config.bufferSize;
#ifdef _WIN32
    audioConfig.driverName = config.audioDevice;
#else
    audioConfig.deviceName = config.audioDevice;
#endif

#ifdef _WIN32
    m_impl->audio = std::make_unique<AsioAudioHandler>();
#else
    m_impl->audio = std::make_unique<CoreAudioHandler>();
#endif

    if (!m_impl->audio->init(audioConfig)) {
        m_impl->lastError = "Failed to initialize audio device";
        return false;
    }

    // Verify frame sizes match
    int actualBufSize = m_impl->audio->getActualBufferSize();
    if (actualBufSize != celtConfig.frameSize) {
        celtConfig.frameSize = actualBufSize;
        m_impl->encoder = std::make_unique<CeltCodec>();
        m_impl->decoder = std::make_unique<CeltCodec>();
        if (!m_impl->encoder->init(celtConfig) || !m_impl->decoder->init(celtConfig)) {
            m_impl->lastError = "Failed to reinitialize CELT with buffer size " +
                                std::to_string(actualBufSize);
            return false;
        }
    }
    m_impl->statActualBufferSize = actualBufSize;

    // =========================================================================
    // 4. Wire up the pipeline
    // =========================================================================
    int maxEncoded = m_impl->encoder->getEncodedFrameSize() + 16;
    m_impl->encodeBuffer.resize(maxEncoded);
    m_impl->decodeBuffer.resize(celtConfig.frameSize * celtConfig.channels);

    // CAPTURE → ENCODE → SEND
    m_impl->audio->setCaptureCallback([this, maxEncoded](const float* samples, int count) {
        int encoded = m_impl->encoder->encode(samples, m_impl->encodeBuffer.data(), maxEncoded);
        if (encoded > 0) {
            m_impl->network->send(m_impl->encodeBuffer.data(), encoded);
        }
    });

    // RECEIVE → DECODE → PLAYOUT
    int frameSize = celtConfig.frameSize;
    m_impl->network->setReceiveCallback([this, frameSize](const uint8_t* data, int length, uint32_t seq) {
        // Packet loss concealment
        if (m_impl->lastRecvSeq > 0 && seq > m_impl->lastRecvSeq + 1) {
            int lost = seq - m_impl->lastRecvSeq - 1;
            for (int i = 0; i < lost && i < 3; i++) {
                m_impl->decoder->decodePLC(m_impl->decodeBuffer.data());
                m_impl->audio->writePlayoutSamples(m_impl->decodeBuffer.data(), frameSize);
            }
        }
        m_impl->lastRecvSeq = seq;

        int decoded = m_impl->decoder->decode(data, length, m_impl->decodeBuffer.data());
        if (decoded > 0) {
            m_impl->audio->writePlayoutSamples(m_impl->decodeBuffer.data(), decoded);
        }
    });

    // =========================================================================
    // 5. Start
    // =========================================================================
    m_impl->network->start();

    if (!m_impl->audio->start()) {
        m_impl->network->stop();
        m_impl->lastError = "Failed to start audio streaming";
        return false;
    }

    m_impl->running = true;
    return true;
}

void AudioEngine::stop() {
    if (!m_impl->running) return;

    m_impl->audio->stop();
    m_impl->network->stop();
    m_impl->running = false;
}

bool AudioEngine::isRunning() const {
    return m_impl->running;
}

EngineStats AudioEngine::getStats() const {
    EngineStats stats;
    stats.connected = m_impl->running.load();

    if (m_impl->running) {
        auto netStats = m_impl->network->getStats();
        auto audioStats = m_impl->audio->getStats();

        stats.packetsSent = netStats.packetsSent;
        stats.packetsReceived = netStats.packetsReceived;
        stats.packetsLost = netStats.packetsLost;
        stats.underruns = audioStats.underruns;
        stats.actualBufferSize = m_impl->statActualBufferSize.load();
        stats.latencyMs = (double)stats.actualBufferSize / 48000.0 * 1000.0 * 2.0 + 0.2;
    }

    return stats;
}

std::string AudioEngine::getLastError() const {
    return m_impl->lastError;
}
