// =============================================================================
  // PLAYOUT WORKLET — Ring buffer with TWO input modes:
  //
  // MODE 1 (original): Audio arrives via AudioWorkletNode inputs (WebRTC → MediaStream)
  // MODE 2 (new):      Audio arrives via port.postMessage({ type: 'samples', data: Float32Array })
  //
  // Mode 2 is used by the instrumented low-latency client. It bypasses all browser
  // audio routing — samples go directly from WebCodecs AudioDecoder → this buffer.
  //
  // BUFFER STRATEGY (unchanged):
  //   Overflow  → drop oldest (keeps latency bounded)
  //   Underrun  → output silence (brief glitch, no added delay)
  // =============================================================================

  class PlayoutProcessor extends AudioWorkletProcessor {
    constructor(options) {
      super();

      // --- CONFIGURABLE BUFFER SIZE ---
      // At 48kHz:
      //   64 samples   = 1.3ms  (extreme — will glitch on any jitter)
      //   128 samples  = 2.7ms  (almost nothing — will glitch constantly)
      //   256 samples  = 5.3ms  (aggressive but usable on localhost)
      //   480 samples  = 10ms   (aggressive)
      //   960 samples  = 20ms   (matches one Opus frame — good default)
      //   2400 samples = 50ms   (comfortable)
      //   4800 samples = 100ms  (very safe, but defeats the purpose)
      this.bufferSize = options.processorOptions?.bufferSize || 960;

      this.buffer = new Float32Array(this.bufferSize);
      this.writePos = 0;
      this.readPos = 0;
      this.buffered = 0;

      this.totalPlayed = 0;
      this.totalUnderruns = 0;
      this.totalOverflows = 0;

      // Queue for samples arriving via port (mode 2)
      this.sampleQueue = [];

      // Handle messages from main thread
      this.port.onmessage = (event) => {
        const data = event.data;

        if (data.type === 'samples') {
          // MODE 2: Direct sample injection from WebCodecs decoder
          // Write samples directly into the ring buffer
          const samples = data.data;
          for (let i = 0; i < samples.length; i++) {
            if (this.buffered >= this.bufferSize) {
              // OVERFLOW: drop oldest to maintain latency bound
              this.readPos = (this.readPos + 1) % this.bufferSize;
              this.buffered--;
              this.totalOverflows++;
            }
            this.buffer[this.writePos] = samples[i];
            this.writePos = (this.writePos + 1) % this.bufferSize;
            this.buffered++;
          }
        }

        if (data.type === 'resize') {
          this.bufferSize = data.bufferSize;
          this.buffer = new Float32Array(this.bufferSize);
          this.writePos = 0;
          this.readPos = 0;
          this.buffered = 0;
          this.port.postMessage({ type: 'resized', bufferSize: this.bufferSize });
        }

        if (data.type === 'get-stats') {
          this.port.postMessage({
            type: 'stats',
            buffered: this.buffered,
            bufferSize: this.bufferSize,
            totalPlayed: this.totalPlayed,
            totalUnderruns: this.totalUnderruns,
            totalOverflows: this.totalOverflows,
            fillPercent: Math.round((this.buffered / this.bufferSize) * 100)
          });
        }
      };
    }

    // Called every ~2.7ms (128 samples at 48kHz) by the browser audio thread.
    process(inputs, outputs) {
      const input = inputs[0]?.[0];   // MODE 1: from MediaStreamSource (original path)
      const output = outputs[0][0];   // To speakers

      // --- MODE 1: Write incoming input samples into ring buffer ---
      // (Only active when connected via AudioWorkletNode inputs, i.e., original client)
      if (input && input.length > 0) {
        for (let i = 0; i < input.length; i++) {
          if (this.buffered >= this.bufferSize) {
            this.readPos = (this.readPos + 1) % this.bufferSize;
            this.buffered--;
            this.totalOverflows++;
          }
          this.buffer[this.writePos] = input[i];
          this.writePos = (this.writePos + 1) % this.bufferSize;
          this.buffered++;
        }
      }

      // --- READ from ring buffer into output ---
      for (let i = 0; i < output.length; i++) {
        if (this.buffered > 0) {
          output[i] = this.buffer[this.readPos];
          this.readPos = (this.readPos + 1) % this.bufferSize;
          this.buffered--;
          this.totalPlayed++;
        } else {
          output[i] = 0;
          this.totalUnderruns++;
        }
      }

      return true; // Keep processor alive
    }
  }

  registerProcessor('playout-processor', PlayoutProcessor);

