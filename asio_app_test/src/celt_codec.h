  #pragma once
  // =============================================================================
  // CELT CODEC WRAPPER
  //
  // Wraps libopus Custom Mode (CELT) for encoding/decoding audio frames.
  // Configured identically to Jamulus:
  //   - Custom frame sizes (64, 128, 256 samples)
  //   - OPUS_APPLICATION_RESTRICTED_LOWDELAY (CELT only, no SILK)
  //   - Constant bitrate
  //   - Low complexity for fast encoding
  // =============================================================================

  #include <cstdint>
  #include <vector>

  struct CeltConfig {
      int sampleRate = 48000;
      int frameSize = 64;        // Samples per frame (64 = 1.33ms)
      int channels = 1;          // 1 = mono, 2 = stereo
      int bitrate = 64000;       // Bits per second
      int complexity = 1;        // 0-10, lower = faster
      int packetLossPercent = 35; // PLC hint for small frames
  };

  class CeltCodec {
  public:
      CeltCodec();
      ~CeltCodec();

      // Initialize encoder and decoder
      bool init(const CeltConfig& config);

      // Encode one frame of PCM audio.
      // Input: frameSize float samples
      // Output: encoded bytes written to output buffer
      // Returns: number of encoded bytes, or -1 on error
      int encode(const float* pcm, uint8_t* output, int maxOutputBytes);

      // Decode one frame of encoded audio.
      // Input: encoded bytes
      // Output: frameSize float samples written to pcm buffer
      // Returns: number of decoded samples per channel, or -1 on error
      int decode(const uint8_t* encoded, int encodedLen, float* pcm);

      // Decode with packet loss concealment (no data received)
      // Generates a "best guess" frame to minimize audible glitch
      int decodePLC(float* pcm);

      // Get frame size in samples
      int getFrameSize() const { return m_config.frameSize; }

      // Get frame duration in milliseconds
      double getFrameDurationMs() const {
          return (double)m_config.frameSize / m_config.sampleRate * 1000.0;
      }

      // Get expected encoded frame size in bytes (CBR = constant)
      int getEncodedFrameSize() const { return m_encodedFrameSize; }

  private:
      CeltConfig m_config;
      void* m_mode = nullptr;
      void* m_encoder = nullptr;
      void* m_decoder = nullptr;
      int m_encodedFrameSize = 0;
  };
