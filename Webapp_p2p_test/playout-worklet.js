// =============================================================================
// PLAYOUT WORKLET — Minimal ring buffer between WebRTC decode and speakers.
//
// WHAT THIS DOES:
// Audio comes in from the WebRTC MediaStreamSource (via the AudioWorkletNode input).
// We buffer it in a ring buffer of configurable size, then output it.
//
// WHY:
// The browser's default playout path has hidden buffers (~50-100ms).
// By routing through this worklet, we control exactly how much audio is buffered.
// Less buffer = lower latency (but more risk of glitches if network hiccups).
//
// BUFFER OVERFLOW (too much data):
//   → Drop oldest samples. This keeps latency constant.
//     (Alternative: skip the new samples — but that increases latency.)
//
// BUFFER UNDERRUN (no data available):
//   → Output silence. This causes a brief glitch but doesn't add delay.
//     (Alternative: stretch previous audio — but that adds complexity.)
// =============================================================================

class PlayoutProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();

    // --- CONFIGURABLE BUFFER SIZE ---
    // This is the ONLY latency you're adding in this stage.
    // At 48kHz:
    //   128 samples  = 2.7ms  (almost nothing — will glitch constantly)
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

    // Handle messages from main thread (resize, stats requests)
    this.port.onmessage = (event) => {
      if (event.data.type === 'resize') {
        // Resize buffer at runtime — allows you to experiment without reloading
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
          totalOverflows: this.totalOverflows,
          fillPercent: Math.round((this.buffered / this.bufferSize) * 100)
        });
      }
    };
  }

  // Called every ~2.7ms by the browser audio thread.
  // inputs[0][0] = samples from the WebRTC MediaStreamSource
  // outputs[0][0] = samples to send to speakers
  process(inputs, outputs) {
    const input = inputs[0]?.[0];   // Incoming audio (from WebRTC decode)
    const output = outputs[0][0];   // Outgoing audio (to speakers)

    // --- WRITE incoming samples into the ring buffer ---
    if (input) {
      for (let i = 0; i < input.length; i++) {
        if (this.buffered >= this.bufferSize) {
          // OVERFLOW: buffer is full. Drop the OLDEST sample to make room.
          // This keeps our latency from growing — we always stay at bufferSize.
          this.readPos = (this.readPos + 1) % this.bufferSize;
          this.buffered--;
          this.totalOverflows++;
        }
        this.buffer[this.writePos] = input[i];
        this.writePos = (this.writePos + 1) % this.bufferSize;
        this.buffered++;
      }
    }

    // --- READ from the ring buffer into the output ---
    for (let i = 0; i < output.length; i++) {
      if (this.buffered > 0) {
        output[i] = this.buffer[this.readPos];
        this.readPos = (this.readPos + 1) % this.bufferSize;
        this.buffered--;
        this.totalPlayed++;
      } else {
        // UNDERRUN: nothing to play. Output silence.
        // You'll hear this as a tiny click or gap.
        output[i] = 0;
        this.totalUnderruns++;
      }
    }

    return true; // Keep processor alive
  }
}

registerProcessor('playout-processor', PlayoutProcessor);
