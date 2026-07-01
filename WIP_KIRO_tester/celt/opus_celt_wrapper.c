  // =============================================================================
  // OPUS CUSTOM MODE (CELT) WRAPPER FOR WASM
  //
  // This wraps libopus's Custom Mode API to expose a minimal encode/decode
  // interface that can be called from JavaScript via Emscripten.
  //
  // Custom Mode allows frame sizes as small as 64 samples (1.33ms at 48kHz),
  // which is impossible with standard Opus (minimum 10ms).
  //
  // The CELT codec inside Opus has:
  //   - Zero algorithmic delay (unlike SILK which adds 6.5ms)
  //   - Support for arbitrary frame sizes (64, 128, 256, 480, etc.)
  //   - High quality at low latency
  //
  // IMPORTANT: Custom Mode is NOT wire-compatible with standard Opus.
  // Both encoder and decoder must use the same custom mode settings.
  // =============================================================================

  #include "opus/opus_custom.h"
  #include <stdlib.h>
  #include <string.h>

  // --- Global state (single encoder/decoder instance for simplicity) ---
  static OpusCustomMode    *g_mode = NULL;
  static OpusCustomEncoder *g_encoder = NULL;
  static OpusCustomDecoder *g_decoder = NULL;
  static int                g_frame_size = 64;
  static int                g_channels = 1;
  static int                g_sample_rate = 48000;

  // Buffers for encode/decode (allocated once)
  static float         *g_pcm_buffer = NULL;     // For decoded output
  static unsigned char *g_encoded_buffer = NULL;  // For encoded output
  #define MAX_ENCODED_BYTES 512

  // =============================================================================
  // INITIALIZATION
  // =============================================================================

  /**
   * Initialize the CELT encoder and decoder.
   *
   * @param sample_rate  Sample rate (48000 recommended)
   * @param frame_size   Samples per frame (64 = 1.33ms, 128 = 2.67ms, 256 = 5.33ms)
   * @param channels     Number of channels (1 = mono, 2 = stereo)
   * @param bitrate      Target bitrate in bits/sec (e.g., 64000 for 64kbps)
   * @param complexity   Encoder complexity 0-10 (lower = faster, 1 recommended for low latency)
   * @return 0 on success, negative on error
   */
  int celt_init(int sample_rate, int frame_size, int channels, int bitrate, int complexity) {
      int err;

      g_sample_rate = sample_rate;
      g_frame_size = frame_size;
      g_channels = channels;

      // Create custom mode with our exact frame size
      g_mode = opus_custom_mode_create(sample_rate, frame_size, &err);
      if (err != OPUS_OK || !g_mode) return -1;

      // Create encoder
      g_encoder = opus_custom_encoder_create(g_mode, channels, &err);
      if (err != OPUS_OK || !g_encoder) return -2;

      // Create decoder
      g_decoder = opus_custom_decoder_create(g_mode, channels, &err);
      if (err != OPUS_OK || !g_decoder) return -3;

      // --- Configure encoder for minimum latency (same as Jamulus) ---

      // Constant bitrate — predictable packet sizes
      opus_custom_encoder_ctl(g_encoder, OPUS_SET_VBR(0));

      // Set bitrate
      opus_custom_encoder_ctl(g_encoder, OPUS_SET_BITRATE(bitrate));

      // Restricted low delay — uses only CELT, no SILK lookahead
      opus_custom_encoder_ctl(g_encoder, OPUS_SET_APPLICATION(OPUS_APPLICATION_RESTRICTED_LOWDELAY));

      // Low complexity — faster encoding
      opus_custom_encoder_ctl(g_encoder, OPUS_SET_COMPLEXITY(complexity));

      // For very small frames, help with packet loss concealment
      if (frame_size <= 128) {
          opus_custom_encoder_ctl(g_encoder, OPUS_SET_PACKET_LOSS_PERC(35));
      }

      // Allocate buffers
      g_pcm_buffer = (float *)malloc(frame_size * channels * sizeof(float));
      g_encoded_buffer = (unsigned char *)malloc(MAX_ENCODED_BYTES);

      if (!g_pcm_buffer || !g_encoded_buffer) return -4;

      return 0;
  }

  // =============================================================================
  // ENCODING
  // =============================================================================

  /**
   * Encode a frame of PCM audio.
   *
   * @param pcm_input    Pointer to float PCM samples (frame_size * channels)
   * @param output       Pointer to output buffer for encoded bytes
   * @param max_bytes    Maximum output buffer size
   * @return Number of encoded bytes on success, negative on error
   */
  int celt_encode(const float *pcm_input, unsigned char *output, int max_bytes) {
      if (!g_encoder || !pcm_input || !output) return -1;

      int encoded_bytes = opus_custom_encode_float(
          g_encoder,
          pcm_input,
          g_frame_size,
          output,
          max_bytes
      );

      return encoded_bytes;
  }

  /**
   * Encode using the internal buffer (for easy JS interop).
   * Call get_pcm_ptr() to get the buffer to write PCM into,
   * then call this to encode it.
   *
   * @return Number of encoded bytes, or negative on error.
   *         Result is in the buffer returned by get_encoded_ptr().
   */
  int celt_encode_buffer(void) {
      if (!g_encoder || !g_pcm_buffer || !g_encoded_buffer) return -1;

      int encoded_bytes = opus_custom_encode_float(
          g_encoder,
          g_pcm_buffer,
          g_frame_size,
          g_encoded_buffer,
          MAX_ENCODED_BYTES
      );

      return encoded_bytes;
  }

  // =============================================================================
  // DECODING
  // =============================================================================

  /**
   * Decode an encoded frame back to PCM.
   *
   * @param encoded_input  Pointer to encoded bytes
   * @param encoded_len    Length of encoded data in bytes
   * @param pcm_output     Pointer to output buffer for decoded float PCM
   * @param frame_size     Expected frame size in samples
   * @return Number of decoded samples per channel, or negative on error
   */
  int celt_decode(const unsigned char *encoded_input, int encoded_len, float *pcm_output, int frame_size) {
      if (!g_decoder || !encoded_input || !pcm_output) return -1;

      int decoded_samples = opus_custom_decode_float(
          g_decoder,
          encoded_input,
          encoded_len,
          pcm_output,
          frame_size
      );

      return decoded_samples;
  }

  /**
   * Decode using the internal buffer (for easy JS interop).
   * Write encoded bytes into get_encoded_ptr(), then call this.
   * Result is in the buffer at get_pcm_ptr().
   *
   * @param encoded_len  Number of encoded bytes to decode
   * @return Number of decoded samples per channel, or negative on error
   */
  int celt_decode_buffer(int encoded_len) {
      if (!g_decoder || !g_encoded_buffer || !g_pcm_buffer) return -1;

      int decoded_samples = opus_custom_decode_float(
          g_decoder,
          g_encoded_buffer,
          encoded_len,
          g_pcm_buffer,
          g_frame_size
      );

      return decoded_samples;
  }

  // =============================================================================
  // BUFFER ACCESSORS (for zero-copy JS interop)
  // =============================================================================

  /** Get pointer to PCM buffer (write PCM here before encoding, read PCM here after decoding) */
  float *get_pcm_ptr(void) {
      return g_pcm_buffer;
  }

  /** Get pointer to encoded buffer (read encoded bytes after encoding, write before decoding) */
  unsigned char *get_encoded_ptr(void) {
      return g_encoded_buffer;
  }

  /** Get the configured frame size */
  int get_frame_size(void) {
      return g_frame_size;
  }

  /** Get the configured number of channels */
  int get_channels(void) {
      return g_channels;
  }

  // =============================================================================
  // CLEANUP
  // =============================================================================

  void celt_destroy(void) {
      if (g_encoder) { opus_custom_encoder_destroy(g_encoder); g_encoder = NULL; }
      if (g_decoder) { opus_custom_decoder_destroy(g_decoder); g_decoder = NULL; }
      if (g_mode)    { opus_custom_mode_destroy(g_mode);       g_mode = NULL; }
      if (g_pcm_buffer)     { free(g_pcm_buffer);     g_pcm_buffer = NULL; }
      if (g_encoded_buffer) { free(g_encoded_buffer); g_encoded_buffer = NULL; }
  }
