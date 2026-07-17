// =============================================================================
// CORE AUDIO HANDLER IMPLEMENTATION (macOS)
//
// Uses AudioUnit (AUHAL — Audio Unit Hardware Abstraction Layer) to provide
// ultra-low-latency audio capture and playout on macOS.
//
// Core Audio gives us:
//   - Direct hardware access (no mixer when using AUHAL)
//   - Buffer sizes as low as 16 samples on modern Macs
//   - Predictable callback timing on realtime thread
//
// The AudioUnit render callback architecture:
//   1. Core Audio calls our input callback when capture data is ready
//   2. We read input samples → send to capture callback (for encoding)
//   3. Core Audio calls our output callback when it needs playout data
//   4. We read from playout ring buffer → fill the output buffer
// =============================================================================

#include "audio_coreaudio.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>

#include <iostream>
#include <cstring>
#include <algorithm>
#include <vector>

// =============================================================================
// Implementation struct
// =============================================================================
struct CoreAudioHandler::Impl {
    AudioConfig config;
    CaptureCallback captureCallback;

    // Ring buffer for playout (network thread writes, render thread reads)
    RingBuffer<float> playoutBuffer{4096};

    // Audio Units
    AudioComponentInstance inputUnit = nullptr;
    AudioComponentInstance outputUnit = nullptr;

    // Device IDs
    AudioDeviceID inputDevice = kAudioObjectUnknown;
    AudioDeviceID outputDevice = kAudioObjectUnknown;

    int actualBufferSize = 0;
    bool running = false;

    // Stats
    uint64_t capturedFrames = 0;
    uint64_t playedFrames = 0;
    uint64_t underruns = 0;
    uint64_t overflows = 0;

    // Temporary buffer for input rendering
    AudioBufferList* inputBufferList = nullptr;
};

// =============================================================================
// Helper: Get default device
// =============================================================================
static AudioDeviceID getDefaultDevice(bool isInput) {
    AudioObjectPropertyAddress addr;
    addr.mSelector = isInput ? kAudioHardwarePropertyDefaultInputDevice
                             : kAudioHardwarePropertyDefaultOutputDevice;
    addr.mScope = kAudioObjectPropertyScopeGlobal;
    addr.mElement = kAudioObjectPropertyElementMain;

    AudioDeviceID deviceId = kAudioObjectUnknown;
    UInt32 size = sizeof(deviceId);

    OSStatus status = AudioObjectGetPropertyData(
        kAudioObjectSystemObject, &addr, 0, nullptr, &size, &deviceId);

    if (status != noErr) {
        std::cerr << "[CoreAudio] Failed to get default "
                  << (isInput ? "input" : "output") << " device: " << status << std::endl;
        return kAudioObjectUnknown;
    }
    return deviceId;
}

// =============================================================================
// Helper: Get device name
// =============================================================================
static std::string getDeviceName(AudioDeviceID deviceId) {
    AudioObjectPropertyAddress addr;
    addr.mSelector = kAudioObjectPropertyName;
    addr.mScope = kAudioObjectPropertyScopeGlobal;
    addr.mElement = kAudioObjectPropertyElementMain;

    CFStringRef nameRef = nullptr;
    UInt32 size = sizeof(nameRef);

    OSStatus status = AudioObjectGetPropertyData(deviceId, &addr, 0, nullptr, &size, &nameRef);
    if (status != noErr || !nameRef) return "(unknown)";

    char nameBuf[256];
    CFStringGetCString(nameRef, nameBuf, sizeof(nameBuf), kCFStringEncodingUTF8);
    CFRelease(nameRef);
    return std::string(nameBuf);
}

// =============================================================================
// Helper: Set device buffer size
// =============================================================================
static bool setDeviceBufferSize(AudioDeviceID deviceId, UInt32 frames, bool isInput) {
    AudioObjectPropertyAddress addr;
    addr.mSelector = kAudioDevicePropertyBufferFrameSize;
    addr.mScope = isInput ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput;
    addr.mElement = kAudioObjectPropertyElementMain;

    OSStatus status = AudioObjectSetPropertyData(
        deviceId, &addr, 0, nullptr, sizeof(frames), &frames);

    if (status != noErr) {
        std::cerr << "[CoreAudio] Failed to set buffer size to " << frames
                  << " on " << (isInput ? "input" : "output") << ": " << status << std::endl;
        return false;
    }
    return true;
}

// =============================================================================
// Helper: Get actual device buffer size
// =============================================================================
static UInt32 getDeviceBufferSize(AudioDeviceID deviceId, bool isInput) {
    AudioObjectPropertyAddress addr;
    addr.mSelector = kAudioDevicePropertyBufferFrameSize;
    addr.mScope = isInput ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput;
    addr.mElement = kAudioObjectPropertyElementMain;

    UInt32 frames = 0;
    UInt32 size = sizeof(frames);

    AudioObjectGetPropertyData(deviceId, &addr, 0, nullptr, &size, &frames);
    return frames;
}

// =============================================================================
// Helper: Get device latency in frames
// =============================================================================
static UInt32 getDeviceLatency(AudioDeviceID deviceId, bool isInput) {
    AudioObjectPropertyAddress addr;
    addr.mSelector = kAudioDevicePropertyLatency;
    addr.mScope = isInput ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput;
    addr.mElement = kAudioObjectPropertyElementMain;

    UInt32 latency = 0;
    UInt32 size = sizeof(latency);
    AudioObjectGetPropertyData(deviceId, &addr, 0, nullptr, &size, &latency);
    return latency;
}

// =============================================================================
// Input render callback — called by Core Audio when input samples are available
// =============================================================================
static OSStatus inputCallback(
    void* inRefCon,
    AudioUnitRenderActionFlags* ioActionFlags,
    const AudioTimeStamp* inTimeStamp,
    UInt32 inBusNumber,
    UInt32 inNumberFrames,
    AudioBufferList* ioData)
{
    auto* impl = static_cast<CoreAudioHandler::Impl*>(inRefCon);
    if (!impl || !impl->running) return noErr;

    // Allocate/reuse buffer list for rendering input
    if (!impl->inputBufferList) {
        impl->inputBufferList = (AudioBufferList*)calloc(1,
            sizeof(AudioBufferList) + sizeof(AudioBuffer));
        impl->inputBufferList->mNumberBuffers = 1;
    }

    // Set up buffer to receive audio
    impl->inputBufferList->mBuffers[0].mNumberChannels = 1;
    impl->inputBufferList->mBuffers[0].mDataByteSize = inNumberFrames * sizeof(float);

    // We need a temporary buffer for the render
    std::vector<float> tempBuf(inNumberFrames);
    impl->inputBufferList->mBuffers[0].mData = tempBuf.data();

    // Render input audio into our buffer
    OSStatus status = AudioUnitRender(
        impl->inputUnit, ioActionFlags, inTimeStamp,
        1,  // Input bus (element 1 on AUHAL)
        inNumberFrames,
        impl->inputBufferList);

    if (status != noErr) return status;

    // Send to capture callback
    if (impl->captureCallback) {
        impl->captureCallback(tempBuf.data(), (int)inNumberFrames);
        impl->capturedFrames += inNumberFrames;
    }

    return noErr;
}

// =============================================================================
// Output render callback — called by Core Audio when it needs output samples
// =============================================================================
static OSStatus outputCallback(
    void* inRefCon,
    AudioUnitRenderActionFlags* ioActionFlags,
    const AudioTimeStamp* inTimeStamp,
    UInt32 inBusNumber,
    UInt32 inNumberFrames,
    AudioBufferList* ioData)
{
    auto* impl = static_cast<CoreAudioHandler::Impl*>(inRefCon);
    if (!impl || !impl->running || !ioData || ioData->mNumberBuffers == 0) {
        // Fill with silence
        if (ioData) {
            for (UInt32 i = 0; i < ioData->mNumberBuffers; i++) {
                memset(ioData->mBuffers[i].mData, 0, ioData->mBuffers[i].mDataByteSize);
            }
        }
        return noErr;
    }

    float* output = static_cast<float*>(ioData->mBuffers[0].mData);
    int framesToRead = (int)inNumberFrames;

    // Read from playout ring buffer
    size_t read = impl->playoutBuffer.read(output, framesToRead);

    if (read < (size_t)framesToRead) {
        impl->underruns += (framesToRead - read);
    }

    impl->playedFrames += inNumberFrames;
    return noErr;
}

// =============================================================================
// Public interface
// =============================================================================

CoreAudioHandler::CoreAudioHandler() : m_impl(new Impl()) {}

CoreAudioHandler::~CoreAudioHandler() {
    stop();
    if (m_impl->inputBufferList) {
        free(m_impl->inputBufferList);
    }
    delete m_impl;
}

std::vector<std::string> CoreAudioHandler::listDevices() {
    std::vector<std::string> result;

    AudioObjectPropertyAddress addr;
    addr.mSelector = kAudioHardwarePropertyDevices;
    addr.mScope = kAudioObjectPropertyScopeGlobal;
    addr.mElement = kAudioObjectPropertyElementMain;

    UInt32 size = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(
        kAudioObjectSystemObject, &addr, 0, nullptr, &size);
    if (status != noErr) return result;

    int deviceCount = size / sizeof(AudioDeviceID);
    std::vector<AudioDeviceID> devices(deviceCount);

    status = AudioObjectGetPropertyData(
        kAudioObjectSystemObject, &addr, 0, nullptr, &size, devices.data());
    if (status != noErr) return result;

    for (auto deviceId : devices) {
        result.push_back(getDeviceName(deviceId));
    }

    return result;
}

bool CoreAudioHandler::init(const AudioConfig& config) {
    m_impl->config = config;

    // --- Get audio devices ---
    m_impl->inputDevice = getDefaultDevice(true);
    m_impl->outputDevice = getDefaultDevice(false);

    if (m_impl->inputDevice == kAudioObjectUnknown) {
        std::cerr << "[CoreAudio] No input device found!" << std::endl;
        return false;
    }
    if (m_impl->outputDevice == kAudioObjectUnknown) {
        std::cerr << "[CoreAudio] No output device found!" << std::endl;
        return false;
    }

    std::cout << "[CoreAudio] Input device: " << getDeviceName(m_impl->inputDevice) << std::endl;
    std::cout << "[CoreAudio] Output device: " << getDeviceName(m_impl->outputDevice) << std::endl;

    // --- Set buffer size on devices ---
    setDeviceBufferSize(m_impl->inputDevice, config.bufferSize, true);
    setDeviceBufferSize(m_impl->outputDevice, config.bufferSize, false);

    m_impl->actualBufferSize = (int)getDeviceBufferSize(m_impl->inputDevice, true);
    if (m_impl->actualBufferSize != config.bufferSize) {
        std::cout << "[CoreAudio] Requested " << config.bufferSize
                  << " samples, got " << m_impl->actualBufferSize << std::endl;
    }

    // --- Create Input AudioUnit (AUHAL) ---
    AudioComponentDescription inputDesc{};
    inputDesc.componentType = kAudioUnitType_Output;
    inputDesc.componentSubType = kAudioUnitSubType_HALOutput;
    inputDesc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent inputComponent = AudioComponentFindNext(nullptr, &inputDesc);
    if (!inputComponent) {
        std::cerr << "[CoreAudio] Cannot find AUHAL component for input" << std::endl;
        return false;
    }

    OSStatus status = AudioComponentInstanceNew(inputComponent, &m_impl->inputUnit);
    if (status != noErr) {
        std::cerr << "[CoreAudio] Failed to create input AudioUnit: " << status << std::endl;
        return false;
    }

    // Enable input on the input unit
    UInt32 enableIO = 1;
    status = AudioUnitSetProperty(m_impl->inputUnit,
        kAudioOutputUnitProperty_EnableIO,
        kAudioUnitScope_Input, 1, &enableIO, sizeof(enableIO));
    if (status != noErr) {
        std::cerr << "[CoreAudio] Failed to enable input: " << status << std::endl;
        return false;
    }

    // Disable output on the input unit (we have a separate unit for output)
    UInt32 disableIO = 0;
    status = AudioUnitSetProperty(m_impl->inputUnit,
        kAudioOutputUnitProperty_EnableIO,
        kAudioUnitScope_Output, 0, &disableIO, sizeof(disableIO));
    if (status != noErr) {
        std::cerr << "[CoreAudio] Failed to disable output on input unit: " << status << std::endl;
        return false;
    }

    // Set input device
    status = AudioUnitSetProperty(m_impl->inputUnit,
        kAudioOutputUnitProperty_CurrentDevice,
        kAudioUnitScope_Global, 0, &m_impl->inputDevice, sizeof(m_impl->inputDevice));
    if (status != noErr) {
        std::cerr << "[CoreAudio] Failed to set input device: " << status << std::endl;
        return false;
    }

    // Set input stream format (32-bit float, mono, non-interleaved)
    AudioStreamBasicDescription inputFormat{};
    inputFormat.mSampleRate = config.sampleRate;
    inputFormat.mFormatID = kAudioFormatLinearPCM;
    inputFormat.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
    inputFormat.mBitsPerChannel = 32;
    inputFormat.mChannelsPerFrame = 1;
    inputFormat.mFramesPerPacket = 1;
    inputFormat.mBytesPerFrame = sizeof(float);
    inputFormat.mBytesPerPacket = sizeof(float);

    status = AudioUnitSetProperty(m_impl->inputUnit,
        kAudioUnitProperty_StreamFormat,
        kAudioUnitScope_Output, 1, &inputFormat, sizeof(inputFormat));
    if (status != noErr) {
        std::cerr << "[CoreAudio] Failed to set input format: " << status << std::endl;
        return false;
    }

    // Set input callback
    AURenderCallbackStruct inputCb{};
    inputCb.inputProc = inputCallback;
    inputCb.inputProcRefCon = m_impl;

    status = AudioUnitSetProperty(m_impl->inputUnit,
        kAudioOutputUnitProperty_SetInputCallback,
        kAudioUnitScope_Global, 0, &inputCb, sizeof(inputCb));
    if (status != noErr) {
        std::cerr << "[CoreAudio] Failed to set input callback: " << status << std::endl;
        return false;
    }

    // Initialize input unit
    status = AudioUnitInitialize(m_impl->inputUnit);
    if (status != noErr) {
        std::cerr << "[CoreAudio] Failed to initialize input unit: " << status << std::endl;
        return false;
    }

    // --- Create Output AudioUnit (AUHAL) ---
    AudioComponentDescription outputDesc{};
    outputDesc.componentType = kAudioUnitType_Output;
    outputDesc.componentSubType = kAudioUnitSubType_HALOutput;
    outputDesc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent outputComponent = AudioComponentFindNext(nullptr, &outputDesc);
    if (!outputComponent) {
        std::cerr << "[CoreAudio] Cannot find AUHAL component for output" << std::endl;
        return false;
    }

    status = AudioComponentInstanceNew(outputComponent, &m_impl->outputUnit);
    if (status != noErr) {
        std::cerr << "[CoreAudio] Failed to create output AudioUnit: " << status << std::endl;
        return false;
    }

    // Set output device
    status = AudioUnitSetProperty(m_impl->outputUnit,
        kAudioOutputUnitProperty_CurrentDevice,
        kAudioUnitScope_Global, 0, &m_impl->outputDevice, sizeof(m_impl->outputDevice));
    if (status != noErr) {
        std::cerr << "[CoreAudio] Failed to set output device: " << status << std::endl;
        return false;
    }

    // Set output stream format (32-bit float, mono, non-interleaved)
    AudioStreamBasicDescription outputFormat{};
    outputFormat.mSampleRate = config.sampleRate;
    outputFormat.mFormatID = kAudioFormatLinearPCM;
    outputFormat.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
    outputFormat.mBitsPerChannel = 32;
    outputFormat.mChannelsPerFrame = 1;
    outputFormat.mFramesPerPacket = 1;
    outputFormat.mBytesPerFrame = sizeof(float);
    outputFormat.mBytesPerPacket = sizeof(float);

    status = AudioUnitSetProperty(m_impl->outputUnit,
        kAudioUnitProperty_StreamFormat,
        kAudioUnitScope_Input, 0, &outputFormat, sizeof(outputFormat));
    if (status != noErr) {
        std::cerr << "[CoreAudio] Failed to set output format: " << status << std::endl;
        return false;
    }

    // Set output render callback
    AURenderCallbackStruct outputCb{};
    outputCb.inputProc = outputCallback;
    outputCb.inputProcRefCon = m_impl;

    status = AudioUnitSetProperty(m_impl->outputUnit,
        kAudioUnitProperty_SetRenderCallback,
        kAudioUnitScope_Input, 0, &outputCb, sizeof(outputCb));
    if (status != noErr) {
        std::cerr << "[CoreAudio] Failed to set output callback: " << status << std::endl;
        return false;
    }

    // Initialize output unit
    status = AudioUnitInitialize(m_impl->outputUnit);
    if (status != noErr) {
        std::cerr << "[CoreAudio] Failed to initialize output unit: " << status << std::endl;
        return false;
    }

    // Report latency
    UInt32 inLatency = getDeviceLatency(m_impl->inputDevice, true);
    UInt32 outLatency = getDeviceLatency(m_impl->outputDevice, false);
    double inMs = (double)(inLatency + m_impl->actualBufferSize) / config.sampleRate * 1000.0;
    double outMs = (double)(outLatency + m_impl->actualBufferSize) / config.sampleRate * 1000.0;
    std::cout << "[CoreAudio] Latency — Input: " << inMs << "ms, Output: " << outMs << "ms" << std::endl;

    // Resize playout ring buffer (8x buffer size for safety)
    m_impl->playoutBuffer = RingBuffer<float>(m_impl->actualBufferSize * 8);

    return true;
}

bool CoreAudioHandler::start() {
    m_impl->running = true;

    OSStatus status = AudioOutputUnitStart(m_impl->inputUnit);
    if (status != noErr) {
        std::cerr << "[CoreAudio] Failed to start input unit: " << status << std::endl;
        m_impl->running = false;
        return false;
    }

    status = AudioOutputUnitStart(m_impl->outputUnit);
    if (status != noErr) {
        std::cerr << "[CoreAudio] Failed to start output unit: " << status << std::endl;
        AudioOutputUnitStop(m_impl->inputUnit);
        m_impl->running = false;
        return false;
    }

    double frameMs = (double)m_impl->actualBufferSize / m_impl->config.sampleRate * 1000.0;
    std::cout << "[CoreAudio] Started — " << m_impl->actualBufferSize << " samples @ "
              << m_impl->config.sampleRate << "Hz (" << frameMs << "ms per callback)" << std::endl;
    return true;
}

void CoreAudioHandler::stop() {
    if (m_impl->running) {
        m_impl->running = false;

        if (m_impl->inputUnit) {
            AudioOutputUnitStop(m_impl->inputUnit);
            AudioUnitUninitialize(m_impl->inputUnit);
            AudioComponentInstanceDispose(m_impl->inputUnit);
            m_impl->inputUnit = nullptr;
        }

        if (m_impl->outputUnit) {
            AudioOutputUnitStop(m_impl->outputUnit);
            AudioUnitUninitialize(m_impl->outputUnit);
            AudioComponentInstanceDispose(m_impl->outputUnit);
            m_impl->outputUnit = nullptr;
        }

        std::cout << "[CoreAudio] Stopped" << std::endl;
    }
}

void CoreAudioHandler::setCaptureCallback(CaptureCallback cb) {
    m_impl->captureCallback = std::move(cb);
}

void CoreAudioHandler::writePlayoutSamples(const float* samples, int count) {
    m_impl->playoutBuffer.write(samples, count);
}

int CoreAudioHandler::getActualBufferSize() const {
    return m_impl->actualBufferSize;
}

CoreAudioHandler::Stats CoreAudioHandler::getStats() const {
    double bufMs = (double)m_impl->actualBufferSize / m_impl->config.sampleRate * 1000.0;
    return {
        m_impl->actualBufferSize,
        m_impl->config.sampleRate,
        bufMs,
        bufMs,
        m_impl->capturedFrames,
        m_impl->playedFrames,
        m_impl->underruns,
        m_impl->overflows
    };
}
