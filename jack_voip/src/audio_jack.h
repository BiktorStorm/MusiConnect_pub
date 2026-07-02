
  // audio_jack.h — JACK audio driver for low-latency capture and playout
  // Replaces ASIO: JACK provides the same callback-based, zero-copy model
  // with realtime-priority threads and direct hardware access.

  #pragma once
  #include <jack/jack.h>
  #include <functional>
  #include <string>
  #include <atomic>
  #include <cstdint>

  struct JackConfig {
      std::string client_name = "jack_voip";
      uint32_t    sample_rate = 48000;   // JACK server dictates this; we verify
      uint32_t    buffer_size = 64;      // frames per period (64 @ 48kHz = 1.33ms)
      int         channels    = 1;       // mono for voice
  };

  // Callback signature: (input_buffer, output_buffer, frame_count)
  // Called from JACK's realtime thread — must be lock-free!
  using AudioCallback = std::function<void(const float*, float*, uint32_t)>;

  class AudioJack {
  public:
      AudioJack();
      ~AudioJack();

      // Initialize JACK client with given config
      bool init(const JackConfig& config);

      // Set the audio processing callback
      void set_callback(AudioCallback cb);

      // Start audio processing (activates JACK client)
      bool start();

      // Stop audio processing
      void stop();

      // Get actual sample rate (set by JACK server)
      uint32_t get_sample_rate() const { return actual_sample_rate_; }

      // Get actual buffer size (set by JACK server)
      uint32_t get_buffer_size() const { return actual_buffer_size_; }

      // Check if running
      bool is_running() const { return running_.load(std::memory_order_acquire); }

      // Get latency in samples (one-way)
      uint32_t get_latency_samples() const;

      // Get latency in milliseconds (one-way)
      double get_latency_ms() const;

  private:
      // JACK callbacks (static because JACK uses C API)
      static int  process_callback(jack_nframes_t nframes, void* arg);
      static void shutdown_callback(void* arg);
      static int  sample_rate_callback(jack_nframes_t nframes, void* arg);
      static int  buffer_size_callback(jack_nframes_t nframes, void* arg);

      jack_client_t*  client_   = nullptr;
      jack_port_t*    input_port_  = nullptr;
      jack_port_t*    output_port_ = nullptr;

      AudioCallback   callback_;
      std::atomic<bool> running_{false};

      uint32_t actual_sample_rate_ = 0;
      uint32_t actual_buffer_size_ = 0;
      JackConfig config_;
  };
