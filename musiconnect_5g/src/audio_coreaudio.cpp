#include "audio_coreaudio.h"

#ifdef USE_COREAUDIO

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <iostream>
#include <vector>
#include <cstring>

struct CoreAudioDevice::Impl {
    AudioConfig config;
    CaptureCallback captureCallback = nullptr;
    void* captureUserData = nullptr;

    RingBuffer<float> playoutBuf{2048};

    AudioComponentInstance inputUnit = nullptr;
    AudioComponentInstance outputUnit = nullptr;

    int bufferSize = 0;
    bool running = false;

    uint64_t captured = 0;
    uint64_t played = 0;
    uint64_t underruns = 0;

    std::vector<float> captureBuf;
};

// Input callback — called by CoreAudio when input data is available
static OSStatus inputCallback(void* inRefCon,
                              AudioUnitRenderActionFlags* ioActionFlags,
                              const AudioTimeStamp* inTimeStamp,
                              UInt32 inBusNumber,
                              UInt32 inNumberFrames,
                              AudioBufferList* /*ioData*/) {
    auto* impl = static_cast<CoreAudioDevice::Impl*>(inRefCon);
    if (!impl->running || !impl->captureCallback) return noErr;

    impl->captureBuf.resize(inNumberFrames);

    AudioBufferList bufList;
    bufList.mNumberBuffers = 1;
    bufList.mBuffers[0].mNumberChannels = 1;
    bufList.mBuffers[0].mDataByteSize = inNumberFrames * sizeof(float);
    bufList.mBuffers[0].mData = impl->captureBuf.data();

    OSStatus err = AudioUnitRender(impl->inputUnit, ioActionFlags, inTimeStamp,
                                   inBusNumber, inNumberFrames, &bufList);
    if (err != noErr) return err;

    impl->captureCallback(impl->captureBuf.data(), (int)inNumberFrames, impl->captureUserData);
    impl->captured += inNumberFrames;
    return noErr;
}

// Output callback — called by CoreAudio when it needs audio to play
static OSStatus outputCallback(void* inRefCon,
                               AudioUnitRenderActionFlags* /*ioActionFlags*/,
                               const AudioTimeStamp* /*inTimeStamp*/,
                               UInt32 /*inBusNumber*/,
                               UInt32 inNumberFrames,
                               AudioBufferList* ioData) {
    auto* impl = static_cast<CoreAudioDevice::Impl*>(inRefCon);

    float* out = static_cast<float*>(ioData->mBuffers[0].mData);
    size_t got = impl->playoutBuf.read(out, inNumberFrames);
    if (got < inNumberFrames) impl->underruns++;
    impl->played += inNumberFrames;
    return noErr;
}

CoreAudioDevice::CoreAudioDevice() : m_impl(new Impl()) {}

CoreAudioDevice::~CoreAudioDevice() {
    stop();
    delete m_impl;
}

std::vector<std::string> CoreAudioDevice::listDrivers() {
    // CoreAudio doesn't have "drivers" like ASIO — list audio devices instead
    std::vector<std::string> result;

    AudioObjectPropertyAddress prop = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    UInt32 size = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &prop, 0, nullptr, &size);
    int count = size / sizeof(AudioDeviceID);
    std::vector<AudioDeviceID> devices(count);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop, 0, nullptr, &size, devices.data());

    for (auto devId : devices) {
        CFStringRef name = nullptr;
        UInt32 nameSize = sizeof(name);
        AudioObjectPropertyAddress nameProp = {
            kAudioDevicePropertyDeviceNameCFString,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        AudioObjectGetPropertyData(devId, &nameProp, 0, nullptr, &nameSize, &name);
        if (name) {
            char buf[256];
            CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
            result.emplace_back(buf);
            CFRelease(name);
        }
    }
    return result;
}

bool CoreAudioDevice::init(const AudioConfig& config) {
    m_impl->config = config;

    // Find the HAL output audio unit (gives low-level access)
    AudioComponentDescription desc{};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (!comp) { std::cerr << "[CoreAudio] No HAL output unit\n"; return false; }

    // Create input unit
    if (AudioComponentInstanceNew(comp, &m_impl->inputUnit) != noErr) return false;

    // Enable input on the input unit
    UInt32 enableIO = 1;
    AudioUnitSetProperty(m_impl->inputUnit, kAudioOutputUnitProperty_EnableIO,
                         kAudioUnitScope_Input, 1, &enableIO, sizeof(enableIO));
    // Disable output on the input unit
    UInt32 disableIO = 0;
    AudioUnitSetProperty(m_impl->inputUnit, kAudioOutputUnitProperty_EnableIO,
                         kAudioUnitScope_Output, 0, &disableIO, sizeof(disableIO));

    // Create output unit
    if (AudioComponentInstanceNew(comp, &m_impl->outputUnit) != noErr) return false;

    // Set audio format: 32-bit float, mono, native sample rate
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate = config.sampleRate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    fmt.mBitsPerChannel = 32;
    fmt.mChannelsPerFrame = 1;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = sizeof(float);
    fmt.mBytesPerPacket = sizeof(float);

    // Set format on input unit (output scope of input bus)
    AudioUnitSetProperty(m_impl->inputUnit, kAudioUnitProperty_StreamFormat,
                         kAudioUnitScope_Output, 1, &fmt, sizeof(fmt));

    // Set format on output unit (input scope of output bus)
    AudioUnitSetProperty(m_impl->outputUnit, kAudioUnitProperty_StreamFormat,
                         kAudioUnitScope_Input, 0, &fmt, sizeof(fmt));

    // Set buffer size
    UInt32 bufFrames = config.bufferSize;
    AudioDeviceID defaultInput = 0, defaultOutput = 0;
    UInt32 devSize = sizeof(AudioDeviceID);

    AudioObjectPropertyAddress defaultInputProp = {
        kAudioHardwarePropertyDefaultInputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &defaultInputProp, 0, nullptr, &devSize, &defaultInput);

    AudioObjectPropertyAddress defaultOutputProp = {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &defaultOutputProp, 0, nullptr, &devSize, &defaultOutput);

    // Set buffer size on both devices
    AudioObjectPropertyAddress bufProp = {
        kAudioDevicePropertyBufferFrameSize,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioObjectSetPropertyData(defaultInput, &bufProp, 0, nullptr, sizeof(bufFrames), &bufFrames);
    AudioObjectSetPropertyData(defaultOutput, &bufProp, 0, nullptr, sizeof(bufFrames), &bufFrames);

    // Verify actual buffer size
    AudioObjectGetPropertyData(defaultOutput, &bufProp, 0, nullptr, &devSize, &bufFrames);
    m_impl->bufferSize = (int)bufFrames;

    // Set input callback
    AURenderCallbackStruct inputCb{inputCallback, m_impl};
    AudioUnitSetProperty(m_impl->inputUnit, kAudioOutputUnitProperty_SetInputCallback,
                         kAudioUnitScope_Global, 0, &inputCb, sizeof(inputCb));

    // Set output callback
    AURenderCallbackStruct outputCb{outputCallback, m_impl};
    AudioUnitSetProperty(m_impl->outputUnit, kAudioUnitProperty_SetRenderCallback,
                         kAudioUnitScope_Input, 0, &outputCb, sizeof(outputCb));

    // Initialize both units
    if (AudioUnitInitialize(m_impl->inputUnit) != noErr) return false;
    if (AudioUnitInitialize(m_impl->outputUnit) != noErr) return false;

    // Size playout buffer
    m_impl->playoutBuf = RingBuffer<float>(m_impl->bufferSize * 6);

    std::cout << "[CoreAudio] " << config.sampleRate << "Hz, "
              << m_impl->bufferSize << " samples ("
              << (double)m_impl->bufferSize / config.sampleRate * 1000.0 << "ms)\n";
    return true;
}

bool CoreAudioDevice::start() {
    m_impl->running = true;
    if (AudioOutputUnitStart(m_impl->inputUnit) != noErr) { m_impl->running = false; return false; }
    if (AudioOutputUnitStart(m_impl->outputUnit) != noErr) { m_impl->running = false; return false; }
    return true;
}

void CoreAudioDevice::stop() {
    if (m_impl->running) {
        m_impl->running = false;
        AudioOutputUnitStop(m_impl->inputUnit);
        AudioOutputUnitStop(m_impl->outputUnit);
        AudioComponentInstanceDispose(m_impl->inputUnit);
        AudioComponentInstanceDispose(m_impl->outputUnit);
        m_impl->inputUnit = nullptr;
        m_impl->outputUnit = nullptr;
    }
}

void CoreAudioDevice::setCaptureCallback(CaptureCallback cb, void* ud) {
    m_impl->captureCallback = cb;
    m_impl->captureUserData = ud;
}

void CoreAudioDevice::writePlayoutSamples(const float* samples, int count) {
    m_impl->playoutBuf.write(samples, count);
}

int CoreAudioDevice::getBufferSize() const { return m_impl->bufferSize; }

AudioDevice::Stats CoreAudioDevice::getStats() const {
    return { m_impl->captured, m_impl->played, m_impl->underruns };
}

AudioDevice* createAudioDevice() { return new CoreAudioDevice(); }

#endif // USE_COREAUDIO
