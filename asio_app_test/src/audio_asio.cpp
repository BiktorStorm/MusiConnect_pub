
  // =============================================================================
  // ASIO AUDIO HANDLER IMPLEMENTATION
  //
  // This interfaces with the Steinberg ASIO SDK to provide ultra-low-latency
  // audio capture and playout on Windows.
  //
  // ASIO gives us:
  //   - Direct hardware access (bypasses Windows mixer)
  //   - Buffer sizes as low as 32 samples (0.67ms at 48kHz)
  //   - Predictable callback timing (hardware interrupt driven)
  //
  // The ASIO callback architecture:
  //   1. Driver calls bufferSwitch() on its own high-priority thread
  //   2. We read input samples → send to capture callback (for encoding)
  //   3. We read from playout ring buffer → write to output
  //   4. Return — must complete before next buffer period
  // =============================================================================

  #include "audio_asio.h"

  #ifdef USE_ASIO

  #include "asio.h"
  #include "asiodrivers.h"
  #include <iostream>
  #include <cstring>
  #include <algorithm>

  // ASIO uses global callbacks — we need a global pointer to our handler
  static AsioAudioHandler::Impl* g_impl = nullptr;

  // =============================================================================
  // Implementation struct
  // =============================================================================
  struct AsioAudioHandler::Impl {
      AudioConfig config;
      CaptureCallback captureCallback;

      // Ring buffer for playout (network thread writes, ASIO thread reads)
      // 4x buffer size gives ~5ms of runway at 64-sample buffers
      RingBuffer<float> playoutBuffer{4096};

      // ASIO driver state
      ASIODriverInfo driverInfo{};
      ASIOBufferInfo bufferInfos[2]{};  // [0]=input, [1]=output
      ASIOCallbacks asioCallbacks{};
      ASIOChannelInfo inputChannelInfo{};
      ASIOChannelInfo outputChannelInfo{};

      int actualBufferSize = 0;
      bool running = false;

      // Stats
      uint64_t capturedFrames = 0;
      uint64_t playedFrames = 0;
      uint64_t underruns = 0;
      uint64_t overflows = 0;

      // Temporary buffer for format conversion
      std::vector<float> convertBuffer;
  };

  // =============================================================================
  // ASIO Callbacks (global, called by the driver)
  // =============================================================================

  // Convert ASIO sample to float based on sample type
  static float asioSampleToFloat(void* buffer, int index, ASIOSampleType type) {
      switch (type) {
          case ASIOSTInt16LSB: {
              int16_t* buf = static_cast<int16_t*>(buffer);
              return buf[index] / 32768.0f;
          }
          case ASIOSTInt24LSB: {
              uint8_t* buf = static_cast<uint8_t*>(buffer) + index * 3;
              int32_t val = (buf[0]) | (buf[1] << 8) | (static_cast<int8_t>(buf[2]) << 16);
              return val / 8388608.0f;
          }
          case ASIOSTInt32LSB: {
              int32_t* buf = static_cast<int32_t*>(buffer);
              return buf[index] / 2147483648.0f;
          }
          case ASIOSTFloat32LSB: {
              float* buf = static_cast<float*>(buffer);
              return buf[index];
          }
          case ASIOSTFloat64LSB: {
              double* buf = static_cast<double*>(buffer);
              return static_cast<float>(buf[index]);
          }
          default:
              return 0.0f;
      }
  }

  // Convert float to ASIO sample format and write to buffer
  static void floatToAsioSample(void* buffer, int index, float value, ASIOSampleType type) {
      // Clamp
      value = std::max(-1.0f, std::min(1.0f, value));

      switch (type) {
          case ASIOSTInt16LSB: {
              int16_t* buf = static_cast<int16_t*>(buffer);
              buf[index] = static_cast<int16_t>(value * 32767.0f);
              break;
          }
          case ASIOSTInt24LSB: {
              int32_t val = static_cast<int32_t>(value * 8388607.0f);
              uint8_t* buf = static_cast<uint8_t*>(buffer) + index * 3;
              buf[0] = val & 0xFF;
              buf[1] = (val >> 8) & 0xFF;
              buf[2] = (val >> 16) & 0xFF;
              break;
          }
          case ASIOSTInt32LSB: {
              int32_t* buf = static_cast<int32_t*>(buffer);
              buf[index] = static_cast<int32_t>(value * 2147483647.0f);
              break;
          }
          case ASIOSTFloat32LSB: {
              float* buf = static_cast<float*>(buffer);
              buf[index] = value;
              break;
          }
          case ASIOSTFloat64LSB: {
              double* buf = static_cast<double*>(buffer);
              buf[index] = static_cast<double>(value);
              break;
          }
          default:
              break;
      }
  }

  // Main ASIO callback — called every buffer period (~1.33ms at 64 samples)
  static void bufferSwitch(long bufferIndex, ASIOBool directProcess) {
      if (!g_impl || !g_impl->running) return;

      int bufSize = g_impl->actualBufferSize;

      // --- READ INPUT (capture) ---
      if (g_impl->captureCallback) {
          g_impl->convertBuffer.resize(bufSize);
          void* inputBuf = g_impl->bufferInfos[0].buffers[bufferIndex];

          for (int i = 0; i < bufSize; i++) {
              g_impl->convertBuffer[i] = asioSampleToFloat(
                  inputBuf, i, g_impl->inputChannelInfo.type);
          }

          g_impl->captureCallback(g_impl->convertBuffer.data(), bufSize);
          g_impl->capturedFrames += bufSize;
      }

      // --- WRITE OUTPUT (playout) ---
      {
          g_impl->convertBuffer.resize(bufSize);
          size_t read = g_impl->playoutBuffer.read(g_impl->convertBuffer.data(), bufSize);

          if (read < static_cast<size_t>(bufSize)) {
              g_impl->underruns += (bufSize - read);
          }

          void* outputBuf = g_impl->bufferInfos[1].buffers[bufferIndex];
          for (int i = 0; i < bufSize; i++) {
              floatToAsioSample(outputBuf, i, g_impl->convertBuffer[i],
                              g_impl->outputChannelInfo.type);
          }

          g_impl->playedFrames += bufSize;
      }
  }

  static void sampleRateChanged(ASIOSampleRate rate) {
      std::cout << "[ASIO] Sample rate changed to " << rate << std::endl;
  }

  static long asioMessage(long selector, long value, void*, double*) {
      switch (selector) {
          case kAsioSelectorSupported:
              return (value == kAsioEngineVersion) ? 1 : 0;
          case kAsioEngineVersion:
              return 2;
          default:
              return 0;
      }
  }

  static ASIOTime* bufferSwitchTimeInfo(ASIOTime* params, long, ASIOBool) {
      // Not used — we use the simpler bufferSwitch callback
      return params;
  }

  // =============================================================================
  // Public interface
  // =============================================================================

  AsioAudioHandler::AsioAudioHandler() : m_impl(new Impl()) {}

  AsioAudioHandler::~AsioAudioHandler() {
      stop();
      if (g_impl == m_impl) g_impl = nullptr;
      delete m_impl;
  }

  std::vector<std::string> AsioAudioHandler::listDrivers() {
      std::vector<std::string> result;

      // AsioDrivers enumerates installed ASIO drivers from the registry
      AsioDrivers drivers;
      char names[16][32];
      char* namePointers[16];
      for (int i = 0; i < 16; i++) namePointers[i] = names[i];

      long count = drivers.getDriverNames(namePointers, 16);
      for (long i = 0; i < count; i++) {
          result.emplace_back(names[i]);
      }

      return result;
  }

  bool AsioAudioHandler::init(const AudioConfig& config) {
      m_impl->config = config;
      g_impl = m_impl;

      // Load the ASIO driver
      AsioDrivers drivers;
      char driverName[64];

      if (config.driverName.empty()) {
          auto available = listDrivers();
          if (available.empty()) {
              std::cerr << "[ASIO] No ASIO drivers found!" << std::endl;
              return false;
          }
          strncpy(driverName, available[0].c_str(), 63);
          std::cout << "[ASIO] Using first available driver: " << driverName << std::endl;
      } else {
          strncpy(driverName, config.driverName.c_str(), 63);
      }

      if (!drivers.loadDriver(driverName)) {
          std::cerr << "[ASIO] Failed to load driver: " << driverName << std::endl;
          return false;
      }

      // Initialize the driver
      m_impl->driverInfo.asioVersion = 2;
      m_impl->driverInfo.sysRef = nullptr;  // HWND on Windows, can be null for console app

      if (ASIOInit(&m_impl->driverInfo) != ASE_OK) {
          std::cerr << "[ASIO] ASIOInit failed: " << m_impl->driverInfo.errorMessage << std::endl;
          return false;
      }

      std::cout << "[ASIO] Driver: " << m_impl->driverInfo.name
                << " v" << m_impl->driverInfo.driverVersion << std::endl;

      // Set sample rate
      if (ASIOSetSampleRate(config.sampleRate) != ASE_OK) {
          std::cerr << "[ASIO] Cannot set sample rate to " << config.sampleRate << std::endl;
          return false;
      }

      // Query buffer sizes
      long minSize, maxSize, preferredSize, granularity;
      ASIOGetBufferSize(&minSize, &maxSize, &preferredSize, &granularity);

      std::cout << "[ASIO] Buffer sizes: min=" << minSize
                << " max=" << maxSize
                << " preferred=" << preferredSize << std::endl;

      // Use requested buffer size, clamped to driver limits
      m_impl->actualBufferSize = std::max((long)config.bufferSize, minSize);
      m_impl->actualBufferSize = std::min((long)m_impl->actualBufferSize, maxSize);

      if (m_impl->actualBufferSize != config.bufferSize) {
          std::cout << "[ASIO] Requested " << config.bufferSize
                    << " samples, using " << m_impl->actualBufferSize << std::endl;
      }

      // Get channel info
      m_impl->inputChannelInfo.channel = config.inputChannel;
      m_impl->inputChannelInfo.isInput = ASIOTrue;
      ASIOGetChannelInfo(&m_impl->inputChannelInfo);

      m_impl->outputChannelInfo.channel = config.outputChannel;
      m_impl->outputChannelInfo.isInput = ASIOFalse;
      ASIOGetChannelInfo(&m_impl->outputChannelInfo);

      std::cout << "[ASIO] Input: " << m_impl->inputChannelInfo.name
                << " (" << m_impl->inputChannelInfo.type << ")" << std::endl;
      std::cout << "[ASIO] Output: " << m_impl->outputChannelInfo.name
                << " (" << m_impl->outputChannelInfo.type << ")" << std::endl;

      // Prepare buffer infos
      m_impl->bufferInfos[0].isInput = ASIOTrue;
      m_impl->bufferInfos[0].channelNum = config.inputChannel;
      m_impl->bufferInfos[0].buffers[0] = nullptr;
      m_impl->bufferInfos[0].buffers[1] = nullptr;

      m_impl->bufferInfos[1].isInput = ASIOFalse;
      m_impl->bufferInfos[1].channelNum = config.outputChannel;
      m_impl->bufferInfos[1].buffers[0] = nullptr;
      m_impl->bufferInfos[1].buffers[1] = nullptr;

      // Set up callbacks
      m_impl->asioCallbacks.bufferSwitch = bufferSwitch;
      m_impl->asioCallbacks.sampleRateDidChange = sampleRateChanged;
      m_impl->asioCallbacks.asioMessage = asioMessage;
      m_impl->asioCallbacks.bufferSwitchTimeInfo = bufferSwitchTimeInfo;

      // Create buffers
      if (ASIOCreateBuffers(m_impl->bufferInfos, 2, m_impl->actualBufferSize,
                            &m_impl->asioCallbacks) != ASE_OK) {
          std::cerr << "[ASIO] Failed to create buffers" << std::endl;
          return false;
      }

      // Report latency
      long inputLatency, outputLatency;
      ASIOGetLatencies(&inputLatency, &outputLatency);
      double inMs = (double)inputLatency / config.sampleRate * 1000.0;
      double outMs = (double)outputLatency / config.sampleRate * 1000.0;
      std::cout << "[ASIO] Latency — Input: " << inMs << "ms, Output: " << outMs << "ms" << std::endl;

      // Resize playout ring buffer (8x ASIO buffer size for safety)
      m_impl->playoutBuffer = RingBuffer<float>(m_impl->actualBufferSize * 8);

      return true;
  }

  bool AsioAudioHandler::start() {
      m_impl->running = true;
      if (ASIOStart() != ASE_OK) {
          std::cerr << "[ASIO] Failed to start" << std::endl;
          m_impl->running = false;
          return false;
      }
      std::cout << "[ASIO] Started — " << m_impl->actualBufferSize << " samples @ "
                << m_impl->config.sampleRate << "Hz ("
                << (double)m_impl->actualBufferSize / m_impl->config.sampleRate * 1000.0
                << "ms per callback)" << std::endl;
      return true;
  }

  void AsioAudioHandler::stop() {
      if (m_impl->running) {
          m_impl->running = false;
          ASIOStop();
          ASIODisposeBuffers();
          ASIOExit();
          std::cout << "[ASIO] Stopped" << std::endl;
      }
  }

  void AsioAudioHandler::setCaptureCallback(CaptureCallback cb) {
      m_impl->captureCallback = std::move(cb);
  }

  void AsioAudioHandler::writePlayoutSamples(const float* samples, int count) {
      m_impl->playoutBuffer.write(samples, count);
  }

  int AsioAudioHandler::getActualBufferSize() const {
      return m_impl->actualBufferSize;
  }

  AsioAudioHandler::Stats AsioAudioHandler::getStats() const {
      return {
          m_impl->actualBufferSize,
          m_impl->config.sampleRate,
          (double)m_impl->actualBufferSize / m_impl->config.sampleRate * 1000.0,
          (double)m_impl->actualBufferSize / m_impl->config.sampleRate * 1000.0,
          m_impl->capturedFrames,
          m_impl->playedFrames,
          m_impl->underruns,
          m_impl->overflows
      };
  }

  #else
  // Stub implementation when ASIO is not available
  AsioAudioHandler::AsioAudioHandler() : m_impl(nullptr) {}
  AsioAudioHandler::~AsioAudioHandler() {}
  std::vector<std::string> AsioAudioHandler::listDrivers() { return {}; }
  bool AsioAudioHandler::init(const AudioConfig&) { return false; }
  bool AsioAudioHandler::start() { return false; }
  void AsioAudioHandler::stop() {}
  void AsioAudioHandler::setCaptureCallback(CaptureCallback) {}
  void AsioAudioHandler::writePlayoutSamples(const float*, int) {}
  int AsioAudioHandler::getActualBufferSize() const { return 0; }
  AsioAudioHandler::Stats AsioAudioHandler::getStats() const { return {}; }
  #endif
