
  // audio_jack.cpp — JACK audio driver implementation
  // JACK replaces ASIO: same low-latency callback model, cross-platform,
  // runs your callback in a realtime-priority thread.

  #include "audio_jack.h"
  #include <cstdio>
  #include <cstring>

  AudioJack::AudioJack() = default;

  AudioJack::~AudioJack() {
      stop();
  }

  bool AudioJack::init(const JackConfig& config) {
      config_ = config;

      // Open JACK client
      jack_status_t status;
      client_ = jack_client_open(
          config_.client_name.c_str(),
          JackNoStartServer,  // Don't auto-start JACK server
          &status
      );

      if (!client_) {
          fprintf(stderr, "[JACK] Failed to open client (status=0x%x)\n", status);
          if (status & JackServerFailed) {
              fprintf(stderr, "[JACK] Unable to connect to JACK server. Is jackd running?\n");
          }
          return false;
      }

      if (status & JackNameNotUnique) {
          const char* actual = jack_get_client_name(client_);
          fprintf(stderr, "[JACK] Client name not unique, using: %s\n", actual);
      }

      // Set callbacks
      jack_set_process_callback(client_, process_callback, this);
      jack_on_shutdown(client_, shutdown_callback, this);
      jack_set_sample_rate_callback(client_, sample_rate_callback, this);
      jack_set_buffer_size_callback(client_, buffer_size_callback, this);

      // Get server parameters
      actual_sample_rate_ = jack_get_sample_rate(client_);
      actual_buffer_size_ = jack_get_buffer_size(client_);

      printf("[JACK] Connected: rate=%u Hz, buffer=%u frames (%.2f ms)\n",
             actual_sample_rate_, actual_buffer_size_,
             (double)actual_buffer_size_ / actual_sample_rate_ * 1000.0);

      if (actual_sample_rate_ != config_.sample_rate) {
          fprintf(stderr, "[JACK] Warning: server rate %u != requested %u. Using server rate.\n",
                  actual_sample_rate_, config_.sample_rate);
      }

      // Register ports
      input_port_ = jack_port_register(
          client_, "capture",
          JACK_DEFAULT_AUDIO_TYPE,
          JackPortIsInput, 0
      );

      output_port_ = jack_port_register(
          client_, "playout",
          JACK_DEFAULT_AUDIO_TYPE,
          JackPortIsOutput, 0
      );

      if (!input_port_ || !output_port_) {
          fprintf(stderr, "[JACK] Failed to register ports\n");
          jack_client_close(client_);
          client_ = nullptr;
          return false;
      }

      printf("[JACK] Ports registered: capture (in), playout (out)\n");
      return true;
  }

  void AudioJack::set_callback(AudioCallback cb) {
      callback_ = std::move(cb);
  }

  bool AudioJack::start() {
      if (!client_) {
          fprintf(stderr, "[JACK] Client not initialized\n");
          return false;
      }

      // Activate client — starts the realtime thread
      if (jack_activate(client_) != 0) {
          fprintf(stderr, "[JACK] Failed to activate client\n");
          return false;
      }

      running_.store(true, std::memory_order_release);

      // Auto-connect to system ports (physical hardware)
      const char** capture_ports = jack_get_ports(
          client_, nullptr, JACK_DEFAULT_AUDIO_TYPE,
          JackPortIsPhysical | JackPortIsOutput  // system capture = output from HW
      );

      if (capture_ports) {
          if (capture_ports[0]) {
              int err = jack_connect(client_, capture_ports[0], jack_port_name(input_port_));
              if (err == 0) {
                  printf("[JACK] Connected system capture → our input\n");
              } else if (err != EEXIST) {
                  fprintf(stderr, "[JACK] Warning: could not connect capture port\n");
              }
          }
          jack_free(capture_ports);
      }

      const char** playback_ports = jack_get_ports(
          client_, nullptr, JACK_DEFAULT_AUDIO_TYPE,
          JackPortIsPhysical | JackPortIsInput  // system playout = input to HW
      );

      if (playback_ports) {
          if (playback_ports[0]) {
              int err = jack_connect(client_, jack_port_name(output_port_), playback_ports[0]);
              if (err == 0) {
                  printf("[JACK] Connected our output → system playout\n");
              } else if (err != EEXIST) {
                  fprintf(stderr, "[JACK] Warning: could not connect playout port\n");
              }
          }
          jack_free(playback_ports);
      }

      printf("[JACK] Audio engine started (latency: %.2f ms one-way)\n", get_latency_ms());
      return true;
  }

  void AudioJack::stop() {
      if (client_ && running_.load(std::memory_order_acquire)) {
          running_.store(false, std::memory_order_release);
          jack_deactivate(client_);
          jack_client_close(client_);
          client_ = nullptr;
          printf("[JACK] Audio engine stopped\n");
      }
  }

  uint32_t AudioJack::get_latency_samples() const {
      return actual_buffer_size_;
  }

  double AudioJack::get_latency_ms() const {
      if (actual_sample_rate_ == 0) return 0.0;
      return (double)actual_buffer_size_ / actual_sample_rate_ * 1000.0;
  }

  // ─── JACK Callbacks (realtime thread — must be lock-free!) ─────────────────────

  int AudioJack::process_callback(jack_nframes_t nframes, void* arg) {
      auto* self = static_cast<AudioJack*>(arg);

      if (!self->running_.load(std::memory_order_relaxed)) {
          return 0;
      }

      // Get port buffers (zero-copy from JACK)
      auto* in  = static_cast<const float*>(jack_port_get_buffer(self->input_port_, nframes));
      auto* out = static_cast<float*>(jack_port_get_buffer(self->output_port_, nframes));

      if (self->callback_) {
          self->callback_(in, out, nframes);
      } else {
          // Silence if no callback set
          memset(out, 0, sizeof(float) * nframes);
      }

      return 0; // 0 = success in JACK
  }

  void AudioJack::shutdown_callback(void* arg) {
      auto* self = static_cast<AudioJack*>(arg);
      self->running_.store(false, std::memory_order_release);
      fprintf(stderr, "[JACK] Server shut down unexpectedly!\n");
  }

  int AudioJack::sample_rate_callback(jack_nframes_t nframes, void* arg) {
      auto* self = static_cast<AudioJack*>(arg);
      self->actual_sample_rate_ = nframes;
      printf("[JACK] Sample rate changed: %u Hz\n", nframes);
      return 0;
  }

  int AudioJack::buffer_size_callback(jack_nframes_t nframes, void* arg) {
      auto* self = static_cast<AudioJack*>(arg);
      self->actual_buffer_size_ = nframes;
      printf("[JACK] Buffer size changed: %u frames (%.2f ms)\n",
             nframes, (double)nframes / self->actual_sample_rate_ * 1000.0);
      return 0;
  }

