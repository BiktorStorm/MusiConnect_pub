 // =============================================================================
  // CAPTURE WORKLET — Collects microphone audio into 64-sample chunks
  //
  // WHY:
  // The browser's AudioWorklet process() is called with 128 samples at a time.
  // But our CELT encoder needs exactly 64 samples per frame.
  // This worklet splits the 128-sample blocks into two 64-sample chunks
  // and sends each to the main thread for encoding.
  //
  // Also handles PLAYBACK: receives decoded 64-sample chunks from main thread
  // and outputs them to speakers.
  //
  // DATA FLOW:
  //   Mic → process(inputs) → split to 64-sample chunks → port.postMessage → main thread
  //   Main thread → port.postMessage(decoded) → ring buffer → process(outputs) → speakers
  // =============================================================================

  class CapturePlayoutProcessor extends AudioWorkletProcessor {
    constructor(options) {
      super();

      this.frameSize = options.processorOptions?.frameSize || 64;
      this.channels = options.processorOptions?.channels || 1;

      // --- CAPTURE: accumulate input samples until we have frameSize ---
      this.captureBuffer = new Float32Array(this.frameSize);
      this.capturePos = 0;

      // --- PLAYOUT: ring buffer for decoded audio ---
      // Keep it small: 2-4 frames worth (128-256 samples at 64-sample frames)
      this.playBufferSize = options.processorOptions?.playBufferSize || 256;
      this.playBuffer = new Float32Array(this.playBufferSize);
      this.playWritePos = 0;
      this.playReadPos = 0;
      this.playBuffered = 0;

      // Stats
      this.totalCaptured = 0;
      this.totalPlayed = 0;
      this.totalUnderruns = 0;
      this.totalOverflows = 0;

      // Handle messages from main thread
      this.port.onmessage = (event) => {
        const data = event.data;

        if (data.type === 'decoded') {
          // Received decoded PCM from main thread — write into play buffer
          const samples = data.samples;
          for (let i = 0; i < samples.length; i++) {
            if (this.playBuffered >= this.playBufferSize) {
              // Overflow: drop oldest to bound latency
              this.playReadPos = (this.playReadPos + 1) % this.playBufferSize;
              this.playBuffered--;
              this.totalOverflows++;
            }
            this.playBuffer[this.playWritePos] = samples[i];
            this.playWritePos = (this.playWritePos + 1) % this.playBufferSize;
            this.playBuffered++;
          }
        }

        if (data.type === 'get-stats') {
          this.port.postMessage({
            type: 'stats',
            playBuffered: this.playBuffered,
            playBufferSize: this.playBufferSize,
            totalCaptured: this.totalCaptured,
            totalPlayed: this.totalPlayed,
            totalUnderruns: this.totalUnderruns,
            totalOverflows: this.totalOverflows,
            playFillPercent: Math.round((this.playBuffered / this.playBufferSize) * 100),
          });
        }

        if (data.type === 'resize-play-buffer') {
          this.playBufferSize = data.size;
          this.playBuffer = new Float32Array(this.playBufferSize);
          this.playWritePos = 0;
          this.playReadPos = 0;
          this.playBuffered = 0;
        }
      };
    }

    process(inputs, outputs) {
      const input = inputs[0]?.[0];   // Microphone input (mono)
      const output = outputs[0]?.[0]; // Speaker output (mono)

      // --- CAPTURE: split input into 64-sample frames ---
      if (input && input.length > 0) {
        for (let i = 0; i < input.length; i++) {
          this.captureBuffer[this.capturePos] = input[i];
          this.capturePos++;

          if (this.capturePos >= this.frameSize) {
            // We have a complete frame — send to main thread for encoding
            // Transfer ownership for zero-copy (the buffer becomes unusable here)
            const frame = this.captureBuffer.slice(); // Copy since we reuse the buffer
            this.port.postMessage({ type: 'pcm-frame', samples: frame }, [frame.buffer]);
            this.capturePos = 0;
            this.totalCaptured += this.frameSize;
          }
        }
      }

      // --- PLAYOUT: read from ring buffer into output ---
      if (output) {
        for (let i = 0; i < output.length; i++) {
          if (this.playBuffered > 0) {
            output[i] = this.playBuffer[this.playReadPos];
            this.playReadPos = (this.playReadPos + 1) % this.playBufferSize;
            this.playBuffered--;
            this.totalPlayed++;
          } else {
            output[i] = 0; // Underrun — silence
            this.totalUnderruns++;
          }
        }
      }

      return true;
    }
  }

  registerProcessor('capture-playout-processor', CapturePlayoutProcessor);
