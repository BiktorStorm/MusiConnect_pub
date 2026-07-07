
  #pragma once
  // =============================================================================
  // ASIO AUDIO HANDLER
  //
  // Manages the ASIO driver lifecycle:
  //   - Enumerate available ASIO drivers
  //   - Open a driver and configure buffer size
  //   - Provide capture (input) and playout (output) callbacks
  //   - Feed captured audio to a callback for encoding
  //   - Accept decoded audio for playout via ring buffer
  //
  // ASIO runs its callback on a high-priority thread. The callback must:
  //   - Never block (no locks, no allocation, no I/O)
  //   - Never take longer than the buffer duration
  //   - Only read/write to lock-free data structures
  // =============================================================================

  #include "ring_buffer.h"
  #include <functional>
  #include <string>
  #include <vector>
  #include <cstdint>

  // Callback type: called from ASIO thread with captured audio
  // Parameters: pointer to float samples, number of samples
  using CaptureCallback = std::function<void(const float*, int)>;

  struct AudioConfig {
      int sampleRate = 48000;
      int bufferSize = 64;      // ASIO buffer size in samples (64 = 1.33ms)
      int inputChannel = 0;     // Which input channel to capture
      int outputChannel = 0;    // Which output channel to play on
      std::string driverName;   // ASIO driver name (empty = first available)
  };

  class AsioAudioHandler {
  public:
      AsioAudioHandler();
      ~AsioAudioHandler();

      // List available ASIO drivers
      static std::vector<std::string> listDrivers();

      // Channel names for a driver (input + output)
      struct ChannelList {
          std::vector<std::string> inputs;
          std::vector<std::string> outputs;
          bool success = false;
      };

      // Query the input/output channel names for a specific driver.
      // Loads + initializes the driver, enumerates channels, then releases it.
      // Safe to call before init() — it fully cleans up after itself.
      static ChannelList queryChannels(const std::string& driverName);

      // Initialize with config
      bool init(const AudioConfig& config);

      // Start audio streaming
      bool start();

      // Stop audio streaming
      void stop();

      // Set callback for captured audio (called from ASIO thread!)
      void setCaptureCallback(CaptureCallback cb);

      // Write decoded audio for playout (thread-safe, called from network thread)
      void writePlayoutSamples(const float* samples, int count);

      // Get current stats
      struct Stats {
          int bufferSize;
          int sampleRate;
          double inputLatencyMs;
          double outputLatencyMs;
          uint64_t capturedFrames;
          uint64_t playedFrames;
          uint64_t underruns;
          uint64_t overflows;
      };
      Stats getStats() const;

      // Get the actual buffer size the driver chose (may differ from requested)
      int getActualBufferSize() const;

  private:
      struct Impl;
      Impl* m_impl;
  };
