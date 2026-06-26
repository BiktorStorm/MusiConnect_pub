// =============================================================================
// PLAYOUT WORKLET — Runs in the audio thread, outputs samples to speakers.
//
// This replaces the browser's default playout buffer with one YOU control.
// The browser calls process() every ~2.7ms asking for 128 samples.
// We pull from a ring buffer that the main thread fills with decoded PCM.
//
// KEY CONCEPT:
// - The ring buffer size IS your playout latency.
//   Smaller buffer = lower latency but more risk of underrun (glitches).
//   Larger buffer = smoother but adds delay.
// =============================================================================

class PlayoutProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();

    // --- CONFIGURABLE BUFFER ---
    // Buffer size in samples. At 48kHz:
    //   480 samples  = 10ms  (aggressive, may glitch)
    //   960 samples  = 20ms  (good for LAN)
    //   2400 samples = 50ms  (safe for internet)
    //   4800 samples = 100ms (very safe, high latency)
    this.bufferSize = options.processorOptions?.bufferSize || 960;  // default 20ms

    // Ring buffer: stores PCM samples waiting to be played
    this.buffer = new Float32Array(this.bufferSize);
    this.writePos = 0;   // Where new samples are written
    this.readPos = 0;    // Where we read from for playback
    this.buffered = 0;   // How many samples are currently waiting

    // Stats for visualization
    this.totalPlayed = 0;
    this.totalUnderruns = 0;  // Times we had nothing to play (glitches)

    // Listen for PCM data from the main thread
    this.port.onmessage = (event) => {
      if (event.data.type === 'samples') {
        this.enqueueSamples(event.data.samples);
      }
      if (event.data.type === 'resize') {
        // Allow runtime buffer size changes
        this.bufferSize = event.data.bufferSize;
        this.buffer = new Float32Array(this.bufferSize);
        this.writePos = 0;
        this.readPos = 0;
        this.buffered = 0;
        this.port.postMessage({ type: 'resized', bufferSize: this.bufferSize });
      }
      if (event.data.type === 'get-stats') {
        this.port.postMessage({
          type: 'stats',
          buffered: this.buffered,
          bufferSize: this.bufferSize,
          totalPlayed: this.totalPlayed,
          totalUnderruns: this.totalUnderruns,
          fillPercent: Math.round((this.buffered / this.bufferSize) * 100)
        });
      }
    };
  }

  // Write incoming PCM samples into the ring buffer
  enqueueSamples(samples) {
    for (let i = 0; i < samples.length; i++) {
      if (this.buffered >= this.bufferSize) {
        // Buffer full — DROP oldest samples to stay low-latency.
        // This is the "drop over buffer" philosophy:
        // we'd rather lose old audio than accumulate delay.
        this.readPos = (this.readPos + 1) % this.bufferSize;
        this.buffered--;
      }
      this.buffer[this.writePos] = samples[i];
      this.writePos = (this.writePos + 1) % this.bufferSize;
      this.buffered++;
    }
  }

  // Called by the browser every ~2.7ms — fill the output with 128 samples
  process(inputs, outputs, parameters) {
    const output = outputs[0][0]; // mono output channel

    for (let i = 0; i < output.length; i++) {
      if (this.buffered > 0) {
        // We have data — play it
        output[i] = this.buffer[this.readPos];
        this.readPos = (this.readPos + 1) % this.bufferSize;
        this.buffered--;
        this.totalPlayed++;
      } else {
        // UNDERRUN: no data available. Output silence.
        // This is a glitch — it means packets aren't arriving fast enough.
        output[i] = 0;
        this.totalUnderruns++;
      }
    }

    return true; // Keep the processor alive
  }
}

registerProcessor('playout-processor', PlayoutProcessor);
