#include "audio_asio.h"
#include <iostream>
#include <algorithm>
#include <cstring>
#include <vector>

#ifdef USE_ASIO

#include "asio.h"
#include "asiodrivers.h"

static AsioAudio::Impl* g_impl = nullptr;

struct AsioAudio::Impl {
    AudioConfig config;
    CaptureCallback captureCallback = nullptr;
    void* captureUserData = nullptr;

    RingBuffer<float> playoutBuf{2048};

    ASIODriverInfo driverInfo{};
    ASIOBufferInfo bufInfos[2]{};
    ASIOCallbacks callbacks{};
    ASIOChannelInfo inChanInfo{};
    ASIOChannelInfo outChanInfo{};

    int bufferSize = 0;
    bool running = false;

    uint64_t captured = 0;
    uint64_t played = 0;
    uint64_t underruns = 0;

    std::vector<float> tmpBuf;
};

static float toFloat(void* buf, int i, ASIOSampleType type) {
    switch (type) {
        case ASIOSTInt16LSB: return static_cast<int16_t*>(buf)[i] / 32768.0f;
        case ASIOSTInt32LSB: return static_cast<int32_t*>(buf)[i] / 2147483648.0f;
        case ASIOSTFloat32LSB: return static_cast<float*>(buf)[i];
        case ASIOSTFloat64LSB: return static_cast<float>(static_cast<double*>(buf)[i]);
        default: return 0.0f;
    }
}

static void fromFloat(void* buf, int i, float v, ASIOSampleType type) {
    v = std::max(-1.0f, std::min(1.0f, v));
    switch (type) {
        case ASIOSTInt16LSB: static_cast<int16_t*>(buf)[i] = (int16_t)(v * 32767.0f); break;
        case ASIOSTInt32LSB: static_cast<int32_t*>(buf)[i] = (int32_t)(v * 2147483647.0f); break;
        case ASIOSTFloat32LSB: static_cast<float*>(buf)[i] = v; break;
        case ASIOSTFloat64LSB: static_cast<double*>(buf)[i] = (double)v; break;
        default: break;
    }
}

static void bufferSwitch(long idx, ASIOBool) {
    if (!g_impl || !g_impl->running) return;
    int n = g_impl->bufferSize;

    // Capture input
    if (g_impl->captureCallback) {
        g_impl->tmpBuf.resize(n);
        void* inBuf = g_impl->bufInfos[0].buffers[idx];
        for (int i = 0; i < n; i++)
            g_impl->tmpBuf[i] = toFloat(inBuf, i, g_impl->inChanInfo.type);
        g_impl->captureCallback(g_impl->tmpBuf.data(), n, g_impl->captureUserData);
        g_impl->captured += n;
    }

    // Playout output
    {
        g_impl->tmpBuf.resize(n);
        size_t got = g_impl->playoutBuf.read(g_impl->tmpBuf.data(), n);
        if (got < (size_t)n) g_impl->underruns++;

        void* outBuf = g_impl->bufInfos[1].buffers[idx];
        for (int i = 0; i < n; i++)
            fromFloat(outBuf, i, g_impl->tmpBuf[i], g_impl->outChanInfo.type);
        g_impl->played += n;
    }
}

static void sampleRateChanged(ASIOSampleRate) {}
static long asioMessage(long sel, long, void*, double*) {
    if (sel == kAsioSelectorSupported) return 1;
    if (sel == kAsioEngineVersion) return 2;
    return 0;
}
static ASIOTime* bufferSwitchTimeInfo(ASIOTime* p, long, ASIOBool) { return p; }

AsioAudio::AsioAudio() : m_impl(new Impl()) {}
AsioAudio::~AsioAudio() { stop(); if (g_impl == m_impl) g_impl = nullptr; delete m_impl; }

std::vector<std::string> AsioAudio::listDrivers() {
    std::vector<std::string> result;
    AsioDrivers drivers;
    char names[16][32];
    char* ptrs[16];
    for (int i = 0; i < 16; i++) ptrs[i] = names[i];
    long count = drivers.getDriverNames(ptrs, 16);
    for (long i = 0; i < count; i++) result.emplace_back(names[i]);
    return result;
}

bool AsioAudio::init(const AudioConfig& config) {
    m_impl->config = config;
    g_impl = m_impl;

    AsioDrivers drivers;
    char name[64];
    if (config.driverName.empty()) {
        auto avail = listDrivers();
        if (avail.empty()) { std::cerr << "[ASIO] No drivers\n"; return false; }
        strncpy(name, avail[0].c_str(), 63);
    } else {
        strncpy(name, config.driverName.c_str(), 63);
    }

    if (!drivers.loadDriver(name)) { std::cerr << "[ASIO] Load failed: " << name << "\n"; return false; }

    m_impl->driverInfo.asioVersion = 2;
    if (ASIOInit(&m_impl->driverInfo) != ASE_OK) return false;
    if (ASIOSetSampleRate(config.sampleRate) != ASE_OK) return false;

    long minSz, maxSz, prefSz, gran;
    ASIOGetBufferSize(&minSz, &maxSz, &prefSz, &gran);
    m_impl->bufferSize = std::clamp((long)config.bufferSize, minSz, maxSz);

    m_impl->inChanInfo.channel = config.inputChannel;
    m_impl->inChanInfo.isInput = ASIOTrue;
    ASIOGetChannelInfo(&m_impl->inChanInfo);

    m_impl->outChanInfo.channel = config.outputChannel;
    m_impl->outChanInfo.isInput = ASIOFalse;
    ASIOGetChannelInfo(&m_impl->outChanInfo);

    m_impl->bufInfos[0] = {ASIOTrue, config.inputChannel, {nullptr, nullptr}};
    m_impl->bufInfos[1] = {ASIOFalse, config.outputChannel, {nullptr, nullptr}};

    m_impl->callbacks = {bufferSwitch, sampleRateChanged, asioMessage, bufferSwitchTimeInfo};

    if (ASIOCreateBuffers(m_impl->bufInfos, 2, m_impl->bufferSize, &m_impl->callbacks) != ASE_OK)
        return false;

    // Size playout buffer: 6x ASIO buffer (tight but enough for 5G jitter)
    m_impl->playoutBuf = RingBuffer<float>(m_impl->bufferSize * 6);

    std::cout << "[ASIO] " << name << " @ " << config.sampleRate << "Hz, "
              << m_impl->bufferSize << " samples ("
              << (double)m_impl->bufferSize / config.sampleRate * 1000.0 << "ms)\n";
    return true;
}

bool AsioAudio::start() {
    m_impl->running = true;
    if (ASIOStart() != ASE_OK) { m_impl->running = false; return false; }
    return true;
}

void AsioAudio::stop() {
    if (m_impl->running) { m_impl->running = false; ASIOStop(); ASIODisposeBuffers(); ASIOExit(); }
}

void AsioAudio::setCaptureCallback(CaptureCallback cb, void* ud) {
    m_impl->captureCallback = cb;
    m_impl->captureUserData = ud;
}

void AsioAudio::writePlayoutSamples(const float* samples, int count) {
    m_impl->playoutBuf.write(samples, count);
}

int AsioAudio::getBufferSize() const { return m_impl->bufferSize; }

AsioAudio::Stats AsioAudio::getStats() const {
    return { m_impl->captured, m_impl->played, m_impl->underruns };
}

#else
// Stub when ASIO SDK not available
AsioAudio::AsioAudio() : m_impl(nullptr) {}
AsioAudio::~AsioAudio() {}
std::vector<std::string> AsioAudio::listDrivers() { return {}; }
bool AsioAudio::init(const AudioConfig&) { std::cerr << "[ASIO] Not compiled with USE_ASIO\n"; return false; }
bool AsioAudio::start() { return false; }
void AsioAudio::stop() {}
void AsioAudio::setCaptureCallback(CaptureCallback, void*) {}
void AsioAudio::writePlayoutSamples(const float*, int) {}
int AsioAudio::getBufferSize() const { return 0; }
AsioAudio::Stats AsioAudio::getStats() const { return {}; }
#endif
