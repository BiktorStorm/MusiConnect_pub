// =============================================================================
// WASAPI AUDIO HANDLER IMPLEMENTATION (Windows)
//
// Uses WASAPI exclusive mode (event-driven) to provide ultra-low-latency
// audio capture and playout on Windows 10/11.
//
// WASAPI exclusive mode gives us:
//   - Direct hardware access (bypasses Windows audio engine/mixer)
//   - Buffer sizes as low as ~40 samples on modern hardware
//   - Event-driven callbacks with MMCSS realtime thread priority
//
// Architecture:
//   1. Capture thread waits on WASAPI event → reads input samples → calls callback
//   2. Playout thread waits on WASAPI event → reads from ring buffer → writes output
//   Both threads run at MMCSS "Pro Audio" priority for minimal scheduling jitter.
// =============================================================================

#include "audio_wasapi.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>

#include <iostream>
#include <cstring>
#include <algorithm>
#include <vector>
#include <thread>
#include <atomic>
#include <cmath>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

// =============================================================================
// GUID definitions (avoid linking ksuuids.lib)
// =============================================================================
// Already defined in mmdeviceapi.h / audioclient.h via __uuidof on MSVC

// =============================================================================
// Helper: RAII COM initializer
// =============================================================================
struct ComInit {
    ComInit() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ComInit() { CoUninitialize(); }
};

// =============================================================================
// Helper: Safe COM release
// =============================================================================
template <typename T>
void safeRelease(T*& ptr) {
    if (ptr) { ptr->Release(); ptr = nullptr; }
}

// =============================================================================
// Implementation struct
// =============================================================================
struct WasapiAudioHandler::Impl {
    AudioConfig config;
    CaptureCallback captureCallback;

    // Ring buffer for playout (network thread writes, render thread reads)
    RingBuffer<float> playoutBuffer{4096};

    // WASAPI interfaces — capture
    IMMDevice* captureDevice = nullptr;
    IAudioClient* captureClient = nullptr;
    IAudioCaptureClient* captureCaptureClient = nullptr;
    HANDLE captureEvent = nullptr;

    // WASAPI interfaces — render
    IMMDevice* renderDevice = nullptr;
    IAudioClient* renderClient = nullptr;
    IAudioRenderClient* renderRenderClient = nullptr;
    HANDLE renderEvent = nullptr;

    // Threads
    std::thread captureThread;
    std::thread renderThread;
    std::atomic<bool> running{false};

    // Actual parameters negotiated with hardware
    int actualBufferFrames = 0;   // WASAPI buffer size in frames
    int actualSampleRate = 0;
    int captureChannels = 0;
    int renderChannels = 0;

    // Stats
    std::atomic<uint64_t> capturedFrames{0};
    std::atomic<uint64_t> playedFrames{0};
    std::atomic<uint64_t> underruns{0};
    std::atomic<uint64_t> overflows{0};

    // Thread functions
    void captureLoop();
    void renderLoop();
};


// =============================================================================
// Helper: Get default device
// =============================================================================
static IMMDevice* getDefaultDevice(EDataFlow flow) {
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr) || !enumerator) return nullptr;

    IMMDevice* device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device);
    enumerator->Release();

    if (FAILED(hr)) return nullptr;
    return device;
}

// =============================================================================
// Helper: Get device friendly name
// =============================================================================
static std::string getDeviceName(IMMDevice* device) {
    if (!device) return "(unknown)";

    IPropertyStore* props = nullptr;
    HRESULT hr = device->OpenPropertyStore(STGM_READ, &props);
    if (FAILED(hr) || !props) return "(unknown)";

    PROPVARIANT varName;
    PropVariantInit(&varName);
    hr = props->GetValue(PKEY_Device_FriendlyName, &varName);

    std::string name = "(unknown)";
    if (SUCCEEDED(hr) && varName.vt == VT_LPWSTR && varName.pwszVal) {
        int len = WideCharToMultiByte(CP_UTF8, 0, varName.pwszVal, -1, nullptr, 0, nullptr, nullptr);
        if (len > 0) {
            std::vector<char> buf(len);
            WideCharToMultiByte(CP_UTF8, 0, varName.pwszVal, -1, buf.data(), len, nullptr, nullptr);
            name = buf.data();
        }
    }

    PropVariantClear(&varName);
    props->Release();
    return name;
}

// =============================================================================
// Helper: Build WAVEFORMATEX for exclusive mode (float32, mono)
// =============================================================================
static WAVEFORMATEX buildExclusiveFormat(int sampleRate) {
    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = (DWORD)sampleRate;
    wfx.wBitsPerSample = 32;
    wfx.nBlockAlign = wfx.nChannels * (wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize = 0;
    return wfx;
}

// =============================================================================
// Helper: Build WAVEFORMATEXTENSIBLE for exclusive mode
// =============================================================================
static WAVEFORMATEXTENSIBLE buildExclusiveFormatEx(int sampleRate, int channels) {
    WAVEFORMATEXTENSIBLE wfxe{};
    wfxe.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfxe.Format.nChannels = (WORD)channels;
    wfxe.Format.nSamplesPerSec = (DWORD)sampleRate;
    wfxe.Format.wBitsPerSample = 32;
    wfxe.Format.nBlockAlign = wfxe.Format.nChannels * (wfxe.Format.wBitsPerSample / 8);
    wfxe.Format.nAvgBytesPerSec = wfxe.Format.nSamplesPerSec * wfxe.Format.nBlockAlign;
    wfxe.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfxe.Samples.wValidBitsPerSample = 32;
    if (channels == 1)
        wfxe.dwChannelMask = SPEAKER_FRONT_CENTER;
    else if (channels == 2)
        wfxe.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    else
        wfxe.dwChannelMask = (1 << channels) - 1;
    wfxe.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return wfxe;
}

// =============================================================================
// Helper: Try to initialize audio client in exclusive mode
// Returns S_OK on success, or the HRESULT error
// =============================================================================
static HRESULT tryInitExclusive(IAudioClient* client, WAVEFORMATEX* format,
                                 REFERENCE_TIME requestedDuration, HANDLE event) {
    HRESULT hr = client->Initialize(
        AUDCLNT_SHAREMODE_EXCLUSIVE,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        requestedDuration,
        requestedDuration,
        format,
        nullptr);
    return hr;
}


// =============================================================================
// Capture thread — waits for WASAPI event, reads samples, calls callback
// =============================================================================
void WasapiAudioHandler::Impl::captureLoop() {
    // Boost thread priority via MMCSS
    DWORD taskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    while (running.load(std::memory_order_acquire)) {
        DWORD waitResult = WaitForSingleObject(captureEvent, 100);
        if (waitResult != WAIT_OBJECT_0) continue;
        if (!running.load(std::memory_order_relaxed)) break;

        UINT32 packetLength = 0;
        HRESULT hr = captureCaptureClient->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) break;

        while (packetLength > 0) {
            BYTE* data = nullptr;
            UINT32 framesAvailable = 0;
            DWORD flags = 0;

            hr = captureCaptureClient->GetBuffer(&data, &framesAvailable, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;

            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data && captureCallback) {
                if (captureChannels == 1) {
                    // Mono — pass directly
                    captureCallback(reinterpret_cast<const float*>(data), (int)framesAvailable);
                } else {
                    // Multi-channel — extract requested channel (de-interleave)
                    const float* interleaved = reinterpret_cast<const float*>(data);
                    std::vector<float> mono(framesAvailable);
                    int ch = config.inputChannel;
                    if (ch >= captureChannels) ch = 0;
                    for (UINT32 i = 0; i < framesAvailable; ++i) {
                        mono[i] = interleaved[i * captureChannels + ch];
                    }
                    captureCallback(mono.data(), (int)framesAvailable);
                }
                capturedFrames.fetch_add(framesAvailable, std::memory_order_relaxed);
            }

            captureCaptureClient->ReleaseBuffer(framesAvailable);

            hr = captureCaptureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) break;
        }
    }

    if (mmcssHandle) AvRevertMmThreadCharacteristics(mmcssHandle);
}

// =============================================================================
// Render thread — waits for WASAPI event, fills output from ring buffer
// =============================================================================
void WasapiAudioHandler::Impl::renderLoop() {
    // Boost thread priority via MMCSS
    DWORD taskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    while (running.load(std::memory_order_acquire)) {
        DWORD waitResult = WaitForSingleObject(renderEvent, 100);
        if (waitResult != WAIT_OBJECT_0) continue;
        if (!running.load(std::memory_order_relaxed)) break;

        // How many frames does WASAPI need?
        UINT32 padding = 0;
        HRESULT hr = renderClient->GetCurrentPadding(&padding);
        if (FAILED(hr)) break;

        UINT32 framesToWrite = (UINT32)actualBufferFrames - padding;
        if (framesToWrite == 0) continue;

        BYTE* data = nullptr;
        hr = renderRenderClient->GetBuffer(framesToWrite, &data);
        if (FAILED(hr)) continue;

        float* output = reinterpret_cast<float*>(data);

        if (renderChannels == 1) {
            // Mono — read directly from ring buffer
            size_t read = playoutBuffer.read(output, framesToWrite);
            if (read < framesToWrite) {
                // Underrun — remaining samples are already zero from read()
                underruns.fetch_add(framesToWrite - read, std::memory_order_relaxed);
            }
        } else {
            // Multi-channel — read mono from ring buffer, place into correct channel
            std::vector<float> mono(framesToWrite);
            size_t read = playoutBuffer.read(mono.data(), framesToWrite);
            if (read < framesToWrite) {
                underruns.fetch_add(framesToWrite - read, std::memory_order_relaxed);
            }

            // Zero the entire output first
            memset(output, 0, framesToWrite * renderChannels * sizeof(float));

            // Place mono into the requested output channel
            int ch = config.outputChannel;
            if (ch >= renderChannels) ch = 0;
            for (UINT32 i = 0; i < framesToWrite; ++i) {
                output[i * renderChannels + ch] = mono[i];
            }
        }

        renderRenderClient->ReleaseBuffer(framesToWrite, 0);
        playedFrames.fetch_add(framesToWrite, std::memory_order_relaxed);
    }

    if (mmcssHandle) AvRevertMmThreadCharacteristics(mmcssHandle);
}


// =============================================================================
// Public interface
// =============================================================================

WasapiAudioHandler::WasapiAudioHandler() : m_impl(new Impl()) {}

WasapiAudioHandler::~WasapiAudioHandler() {
    stop();
    if (m_impl->captureEvent) CloseHandle(m_impl->captureEvent);
    if (m_impl->renderEvent) CloseHandle(m_impl->renderEvent);
    safeRelease(m_impl->captureCaptureClient);
    safeRelease(m_impl->captureClient);
    safeRelease(m_impl->captureDevice);
    safeRelease(m_impl->renderRenderClient);
    safeRelease(m_impl->renderClient);
    safeRelease(m_impl->renderDevice);
    delete m_impl;
}

std::vector<std::string> WasapiAudioHandler::listDevices() {
    std::vector<std::string> result;
    ComInit com;

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr) || !enumerator) return result;

    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr) || !collection) {
        enumerator->Release();
        return result;
    }

    UINT count = 0;
    collection->GetCount(&count);

    for (UINT i = 0; i < count; ++i) {
        IMMDevice* device = nullptr;
        if (SUCCEEDED(collection->Item(i, &device)) && device) {
            result.push_back(getDeviceName(device));
            device->Release();
        }
    }

    collection->Release();
    enumerator->Release();
    return result;
}

std::vector<std::string> WasapiAudioHandler::listInputDevices() {
    std::vector<std::string> result;
    ComInit com;

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr) || !enumerator) return result;

    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr) || !collection) {
        enumerator->Release();
        return result;
    }

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        IMMDevice* device = nullptr;
        if (SUCCEEDED(collection->Item(i, &device)) && device) {
            result.push_back(getDeviceName(device));
            device->Release();
        }
    }

    collection->Release();
    enumerator->Release();
    return result;
}

std::vector<std::string> WasapiAudioHandler::listOutputDevices() {
    std::vector<std::string> result;
    ComInit com;

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr) || !enumerator) return result;

    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr) || !collection) {
        enumerator->Release();
        return result;
    }

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        IMMDevice* device = nullptr;
        if (SUCCEEDED(collection->Item(i, &device)) && device) {
            result.push_back(getDeviceName(device));
            device->Release();
        }
    }

    collection->Release();
    enumerator->Release();
    return result;
}

// Helper: get device by index from a collection
static IMMDevice* getDeviceByIndex(EDataFlow flow, int index) {
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr) || !enumerator) return nullptr;

    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr) || !collection) {
        enumerator->Release();
        return nullptr;
    }

    UINT count = 0;
    collection->GetCount(&count);

    IMMDevice* device = nullptr;
    if (index >= 0 && (UINT)index < count) {
        collection->Item((UINT)index, &device);
    }

    collection->Release();
    enumerator->Release();
    return device;
}

bool WasapiAudioHandler::init(const AudioConfig& config) {
    m_impl->config = config;

    // Create events for WASAPI callbacks
    m_impl->captureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_impl->renderEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_impl->captureEvent || !m_impl->renderEvent) {
        std::cerr << "[WASAPI] Failed to create events" << std::endl;
        return false;
    }

    // --- Get devices (by index or default) ---
    if (config.inputDeviceIndex >= 0) {
        m_impl->captureDevice = getDeviceByIndex(eCapture, config.inputDeviceIndex);
    } else {
        m_impl->captureDevice = getDefaultDevice(eCapture);
    }

    if (config.outputDeviceIndex >= 0) {
        m_impl->renderDevice = getDeviceByIndex(eRender, config.outputDeviceIndex);
    } else {
        m_impl->renderDevice = getDefaultDevice(eRender);
    }

    if (!m_impl->captureDevice) {
        std::cerr << "[WASAPI] No capture device found!" << std::endl;
        return false;
    }
    if (!m_impl->renderDevice) {
        std::cerr << "[WASAPI] No render device found!" << std::endl;
        return false;
    }

    std::cout << "[WASAPI] Capture device: " << getDeviceName(m_impl->captureDevice) << std::endl;
    std::cout << "[WASAPI] Render device:  " << getDeviceName(m_impl->renderDevice) << std::endl;

    // --- Activate capture audio client ---
    HRESULT hr = m_impl->captureDevice->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_impl->captureClient);
    if (FAILED(hr)) {
        std::cerr << "[WASAPI] Failed to activate capture client: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    // --- Activate render audio client ---
    hr = m_impl->renderDevice->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_impl->renderClient);
    if (FAILED(hr)) {
        std::cerr << "[WASAPI] Failed to activate render client: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    // --- Compute requested buffer duration ---
    // REFERENCE_TIME is in 100ns units. 10,000,000 = 1 second
    REFERENCE_TIME requestedDuration =
        (REFERENCE_TIME)(10000000.0 * config.bufferSize / config.sampleRate + 0.5);

    // Minimum period the device supports
    REFERENCE_TIME defaultPeriod = 0, minPeriod = 0;
    m_impl->captureClient->GetDevicePeriod(&defaultPeriod, &minPeriod);
    if (requestedDuration < minPeriod) {
        requestedDuration = minPeriod;
        std::cout << "[WASAPI] Requested buffer too small, using device minimum: "
                  << (double)minPeriod / 10000.0 << " ms" << std::endl;
    }

    // --- Try exclusive mode with mono float32 first ---
    WAVEFORMATEXTENSIBLE wfxCapture = buildExclusiveFormatEx(config.sampleRate, 1);
    hr = tryInitExclusive(m_impl->captureClient, (WAVEFORMATEX*)&wfxCapture,
                          requestedDuration, m_impl->captureEvent);

    if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) {
        // Try stereo
        std::cout << "[WASAPI] Mono not supported for capture, trying stereo..." << std::endl;
        safeRelease(m_impl->captureClient);
        m_impl->captureDevice->Activate(
            __uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_impl->captureClient);
        m_impl->captureClient->GetDevicePeriod(&defaultPeriod, &minPeriod);
        if (requestedDuration < minPeriod) requestedDuration = minPeriod;

        wfxCapture = buildExclusiveFormatEx(config.sampleRate, 2);
        hr = tryInitExclusive(m_impl->captureClient, (WAVEFORMATEX*)&wfxCapture,
                              requestedDuration, m_impl->captureEvent);
    }

    if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) {
        // Fall back to shared mode
        std::cout << "[WASAPI] Exclusive mode not supported, falling back to shared mode..." << std::endl;
        safeRelease(m_impl->captureClient);
        m_impl->captureDevice->Activate(
            __uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_impl->captureClient);

        WAVEFORMATEX* mixFormat = nullptr;
        m_impl->captureClient->GetMixFormat(&mixFormat);
        if (!mixFormat) {
            std::cerr << "[WASAPI] Cannot get mix format for capture" << std::endl;
            return false;
        }

        hr = m_impl->captureClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            requestedDuration,
            0,
            mixFormat,
            nullptr);

        if (SUCCEEDED(hr)) {
            m_impl->captureChannels = mixFormat->nChannels;
            m_impl->actualSampleRate = mixFormat->nSamplesPerSec;
        }
        CoTaskMemFree(mixFormat);
    } else if (SUCCEEDED(hr)) {
        m_impl->captureChannels = wfxCapture.Format.nChannels;
        m_impl->actualSampleRate = wfxCapture.Format.nSamplesPerSec;
    }

    if (FAILED(hr)) {
        std::cerr << "[WASAPI] Failed to initialize capture client: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    // Set capture event
    hr = m_impl->captureClient->SetEventHandle(m_impl->captureEvent);
    if (FAILED(hr)) {
        std::cerr << "[WASAPI] Failed to set capture event: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    // Get capture buffer size
    UINT32 capBufSize = 0;
    m_impl->captureClient->GetBufferSize(&capBufSize);
    m_impl->actualBufferFrames = (int)capBufSize;

    // Get capture service
    hr = m_impl->captureClient->GetService(
        __uuidof(IAudioCaptureClient), (void**)&m_impl->captureCaptureClient);
    if (FAILED(hr)) {
        std::cerr << "[WASAPI] Failed to get capture service: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    std::cout << "[WASAPI] Capture: " << m_impl->captureChannels << "ch @ "
              << m_impl->actualSampleRate << "Hz, buffer=" << m_impl->actualBufferFrames
              << " frames (" << (double)m_impl->actualBufferFrames / m_impl->actualSampleRate * 1000.0
              << " ms)" << std::endl;

    // --- Initialize render client ---
    m_impl->renderClient->GetDevicePeriod(&defaultPeriod, &minPeriod);
    if (requestedDuration < minPeriod) requestedDuration = minPeriod;

    WAVEFORMATEXTENSIBLE wfxRender = buildExclusiveFormatEx(config.sampleRate, 1);
    hr = tryInitExclusive(m_impl->renderClient, (WAVEFORMATEX*)&wfxRender,
                          requestedDuration, m_impl->renderEvent);

    if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) {
        std::cout << "[WASAPI] Mono not supported for render, trying stereo..." << std::endl;
        safeRelease(m_impl->renderClient);
        m_impl->renderDevice->Activate(
            __uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_impl->renderClient);
        m_impl->renderClient->GetDevicePeriod(&defaultPeriod, &minPeriod);
        if (requestedDuration < minPeriod) requestedDuration = minPeriod;

        wfxRender = buildExclusiveFormatEx(config.sampleRate, 2);
        hr = tryInitExclusive(m_impl->renderClient, (WAVEFORMATEX*)&wfxRender,
                              requestedDuration, m_impl->renderEvent);
    }

    if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) {
        std::cout << "[WASAPI] Exclusive mode not supported for render, falling back to shared..." << std::endl;
        safeRelease(m_impl->renderClient);
        m_impl->renderDevice->Activate(
            __uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_impl->renderClient);

        WAVEFORMATEX* mixFormat = nullptr;
        m_impl->renderClient->GetMixFormat(&mixFormat);
        if (!mixFormat) {
            std::cerr << "[WASAPI] Cannot get mix format for render" << std::endl;
            return false;
        }

        hr = m_impl->renderClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            requestedDuration,
            0,
            mixFormat,
            nullptr);

        if (SUCCEEDED(hr)) {
            m_impl->renderChannels = mixFormat->nChannels;
        }
        CoTaskMemFree(mixFormat);
    } else if (SUCCEEDED(hr)) {
        m_impl->renderChannels = wfxRender.Format.nChannels;
    }

    if (FAILED(hr)) {
        std::cerr << "[WASAPI] Failed to initialize render client: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    // Set render event
    hr = m_impl->renderClient->SetEventHandle(m_impl->renderEvent);
    if (FAILED(hr)) {
        std::cerr << "[WASAPI] Failed to set render event: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    // Get render buffer size
    UINT32 renBufSize = 0;
    m_impl->renderClient->GetBufferSize(&renBufSize);

    // Get render service
    hr = m_impl->renderClient->GetService(
        __uuidof(IAudioRenderClient), (void**)&m_impl->renderRenderClient);
    if (FAILED(hr)) {
        std::cerr << "[WASAPI] Failed to get render service: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    std::cout << "[WASAPI] Render:  " << m_impl->renderChannels << "ch @ "
              << config.sampleRate << "Hz, buffer=" << renBufSize
              << " frames (" << (double)renBufSize / config.sampleRate * 1000.0
              << " ms)" << std::endl;

    // Resize playout ring buffer (8x buffer size for safety)
    m_impl->playoutBuffer.resize(m_impl->actualBufferFrames * 8);

    // Report latency
    double capMs = (double)m_impl->actualBufferFrames / m_impl->actualSampleRate * 1000.0;
    double renMs = (double)renBufSize / config.sampleRate * 1000.0;
    std::cout << "[WASAPI] Latency — Capture: " << capMs << "ms, Render: " << renMs << "ms" << std::endl;

    return true;
}


bool WasapiAudioHandler::start() {
    m_impl->running.store(true, std::memory_order_release);

    // Start capture stream
    HRESULT hr = m_impl->captureClient->Start();
    if (FAILED(hr)) {
        std::cerr << "[WASAPI] Failed to start capture: 0x"
                  << std::hex << hr << std::dec << std::endl;
        m_impl->running.store(false, std::memory_order_release);
        return false;
    }

    // Pre-fill render buffer with silence to avoid initial glitch
    UINT32 renBufSize = 0;
    m_impl->renderClient->GetBufferSize(&renBufSize);
    BYTE* data = nullptr;
    hr = m_impl->renderRenderClient->GetBuffer(renBufSize, &data);
    if (SUCCEEDED(hr)) {
        memset(data, 0, renBufSize * m_impl->renderChannels * sizeof(float));
        m_impl->renderRenderClient->ReleaseBuffer(renBufSize, AUDCLNT_BUFFERFLAGS_SILENT);
    }

    // Start render stream
    hr = m_impl->renderClient->Start();
    if (FAILED(hr)) {
        std::cerr << "[WASAPI] Failed to start render: 0x"
                  << std::hex << hr << std::dec << std::endl;
        m_impl->captureClient->Stop();
        m_impl->running.store(false, std::memory_order_release);
        return false;
    }

    // Launch threads
    m_impl->captureThread = std::thread(&Impl::captureLoop, m_impl);
    m_impl->renderThread = std::thread(&Impl::renderLoop, m_impl);

    double frameMs = (double)m_impl->actualBufferFrames / m_impl->actualSampleRate * 1000.0;
    std::cout << "[WASAPI] Started — " << m_impl->actualBufferFrames << " samples @ "
              << m_impl->actualSampleRate << "Hz (" << frameMs << "ms per callback)" << std::endl;
    return true;
}

void WasapiAudioHandler::stop() {
    if (m_impl->running.load(std::memory_order_acquire)) {
        m_impl->running.store(false, std::memory_order_release);

        // Signal events to unblock threads
        if (m_impl->captureEvent) SetEvent(m_impl->captureEvent);
        if (m_impl->renderEvent) SetEvent(m_impl->renderEvent);

        // Join threads
        if (m_impl->captureThread.joinable()) m_impl->captureThread.join();
        if (m_impl->renderThread.joinable()) m_impl->renderThread.join();

        // Stop WASAPI streams
        if (m_impl->captureClient) m_impl->captureClient->Stop();
        if (m_impl->renderClient) m_impl->renderClient->Stop();

        std::cout << "[WASAPI] Stopped" << std::endl;
    }
}

void WasapiAudioHandler::setCaptureCallback(CaptureCallback cb) {
    m_impl->captureCallback = std::move(cb);
}

void WasapiAudioHandler::writePlayoutSamples(const float* samples, int count) {
    m_impl->playoutBuffer.write(samples, count);
}

int WasapiAudioHandler::getActualBufferSize() const {
    return m_impl->actualBufferFrames;
}

WasapiAudioHandler::Stats WasapiAudioHandler::getStats() const {
    double bufMs = (double)m_impl->actualBufferFrames / m_impl->config.sampleRate * 1000.0;
    return {
        m_impl->actualBufferFrames,
        m_impl->actualSampleRate,
        bufMs,
        bufMs,
        m_impl->capturedFrames.load(std::memory_order_relaxed),
        m_impl->playedFrames.load(std::memory_order_relaxed),
        m_impl->underruns.load(std::memory_order_relaxed),
        m_impl->overflows.load(std::memory_order_relaxed)
    };
}
