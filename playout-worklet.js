  1   // =============================================================================
     2-  // PLAYOUT WORKLET — Minimal ring buffer between WebRTC decode and speakers.
     2+  // PLAYOUT WORKLET — Ring buffer with TWO input modes:
     3   //
     4-  // WHAT THIS DOES:
     5-  // Audio comes in from the WebRTC MediaStreamSource (via the AudioWorkletNode input).
     6-  // We buffer it in a ring buffer of configurable size, then output it.
     4+  // MODE 1 (original): Audio arrives via AudioWorkletNode inputs (WebRTC → MediaStream)
     5+  // MODE 2 (new):      Audio arrives via port.postMessage({ type: 'samples', data: Float32Array })
     7   //
     8-  // WHY:
     9-  // The browser's default playout path has hidden buffers (~50-100ms).
    10-  // By routing through this worklet, we control exactly how much audio is buffered.
    11-  // Less buffer = lower latency (but more risk of glitches if network hiccups).
     7+  // Mode 2 is used by the instrumented low-latency client. It bypasses all browser
     8+  // audio routing — samples go directly from WebCodecs AudioDecoder → this buffer.
    12   //
    13-  // BUFFER OVERFLOW (too much data):
    14-  //   → Drop oldest samples. This keeps latency constant.
    15-  //     (Alternative: skip the new samples — but that increases latency.)
    16-  //
    17-  // BUFFER UNDERRUN (no data available):
    18-  //   → Output silence. This causes a brief glitch but doesn't add delay.
    19-  //     (Alternative: stretch previous audio — but that adds complexity.)
    10+  // BUFFER STRATEGY (unchanged):
    11+  //   Overflow  → drop oldest (keeps latency bounded)
    12+  //   Underrun  → output silence (brief glitch, no added delay)
    20   // =============================================================================
    21
    22   class PlayoutProcessor extends AudioWorkletProcessor {
    23     constructor(options) {
    24       super();
    25
    26       // --- CONFIGURABLE BUFFER SIZE ---
    27-      // This is the ONLY latency you're adding in this stage.
    28       // At 48kHz:
    21+      //   64 samples   = 1.3ms  (extreme — will glitch on any jitter)
    29       //   128 samples  = 2.7ms  (almost nothing — will glitch constantly)
    23+      //   256 samples  = 5.3ms  (aggressive but usable on localhost)
    30       //   480 samples  = 10ms   (aggressive)
    31       //   960 samples  = 20ms   (matches one Opus frame — good default)
    32       //   2400 samples = 50ms   (comfortable)
    33       //   4800 samples = 100ms  (very safe, but defeats the purpose)
    34       this.bufferSize = options.processorOptions?.bufferSize || 960;
    35
    36       this.buffer = new Float32Array(this.bufferSize);
    37       this.writePos = 0;
    38       this.readPos = 0;
    39       this.buffered = 0;
    40
    41       this.totalPlayed = 0;
    42       this.totalUnderruns = 0;
    43       this.totalOverflows = 0;
    44
    45-      // Handle messages from main thread (resize, stats requests)
    39+      // Queue for samples arriving via port (mode 2)
    40+      this.sampleQueue = [];
    41+
    42+      // Handle messages from main thread
    46       this.port.onmessage = (event) => {
    47-        if (event.data.type === 'resize') {
    48-          // Resize buffer at runtime — allows you to experiment without reloading
    49-          this.bufferSize = event.data.bufferSize;
    44+        const data = event.data;
    45+
    46+        if (data.type === 'samples') {
    47+          // MODE 2: Direct sample injection from WebCodecs decoder
    48+          // Write samples directly into the ring buffer
    49+          const samples = data.data;
    50+          for (let i = 0; i < samples.length; i++) {
    51+            if (this.buffered >= this.bufferSize) {
    52+              // OVERFLOW: drop oldest to maintain latency bound
    53+              this.readPos = (this.readPos + 1) % this.bufferSize;
    54+              this.buffered--;
    55+              this.totalOverflows++;
    56+            }
    57+            this.buffer[this.writePos] = samples[i];
    58+            this.writePos = (this.writePos + 1) % this.bufferSize;
    59+            this.buffered++;
    60+          }
    61+        }
    62+
    63+        if (data.type === 'resize') {
    64+          this.bufferSize = data.bufferSize;
    50           this.buffer = new Float32Array(this.bufferSize);
    51           this.writePos = 0;
    52           this.readPos = 0;
    53           this.buffered = 0;
    54           this.port.postMessage({ type: 'resized', bufferSize: this.bufferSize });
    55         }
    56-        if (event.data.type === 'get-stats') {
    71+
    72+        if (data.type === 'get-stats') {
    57           this.port.postMessage({
    58             type: 'stats',
    59             buffered: this.buffered,
    60             bufferSize: this.bufferSize,
    61             totalPlayed: this.totalPlayed,
    62             totalUnderruns: this.totalUnderruns,
    63             totalOverflows: this.totalOverflows,
    64             fillPercent: Math.round((this.buffered / this.bufferSize) * 100)
    65           });
    66         }
    67       };
    68     }
    69
    70-    // Called every ~2.7ms by the browser audio thread.
    71-    // inputs[0][0] = samples from the WebRTC MediaStreamSource
    72-    // outputs[0][0] = samples to send to speakers
    86+    // Called every ~2.7ms (128 samples at 48kHz) by the browser audio thread.
    73     process(inputs, outputs) {
    74-      const input = inputs[0]?.[0];   // Incoming audio (from WebRTC decode)
    75-      const output = outputs[0][0];   // Outgoing audio (to speakers)
    88+      const input = inputs[0]?.[0];   // MODE 1: from MediaStreamSource (original path)
    89+      const output = outputs[0][0];   // To speakers
    76
    77-      // --- WRITE incoming samples into the ring buffer ---
    78-      if (input) {
    91+      // --- MODE 1: Write incoming input samples into ring buffer ---
    92+      // (Only active when connected via AudioWorkletNode inputs, i.e., original client)
    93+      if (input && input.length > 0) {
    79         for (let i = 0; i < input.length; i++) {
    80           if (this.buffered >= this.bufferSize) {
    81-            // OVERFLOW: buffer is full. Drop the OLDEST sample to make room.
    82-            // This keeps our latency from growing — we always stay at bufferSize.
    83             this.readPos = (this.readPos + 1) % this.bufferSize;
    84             this.buffered--;
    85             this.totalOverflows++;
    86           }
    87           this.buffer[this.writePos] = input[i];
    88           this.writePos = (this.writePos + 1) % this.bufferSize;
    89           this.buffered++;
    90         }
    91       }
    92
    93-      // --- READ from the ring buffer into the output ---
   106+      // --- READ from ring buffer into output ---
    94       for (let i = 0; i < output.length; i++) {
    95         if (this.buffered > 0) {
    96           output[i] = this.buffer[this.readPos];
    97           this.readPos = (this.readPos + 1) % this.bufferSize;
    98           this.buffered--;
    99           this.totalPlayed++;
   100         } else {
   101-          // UNDERRUN: nothing to play. Output silence.
   102-          // You'll hear this as a tiny click or gap.
   103           output[i] = 0;
   104           this.totalUnderruns++;
   105         }
   106       }
   107
   108       return true; // Keep processor alive
   109     }
   110   }
   111
   112   registerProcessor('playout-processor', PlayoutProcessor);
● Tasks updated

  Now let me verify the server already serves these files correctly:
● Write
● Read C:\Users\ealinor\Downloads\Webapp_p2p_test\server.js (L31-55)

  The server already has the correct routes:

  - /low → serves lowlatency.html
  - /playout-worklet.js → serves the worklet file

  Let me also quickly verify the files I created look correct:
● Read (2 files)
    C:\Users\ealinor\Downloads\Webapp_p2p_test\lowlatency.html, C:\Users\ealinor\Downloads\Webapp_p2p_test\lowlatency.html
● Tasks updated

  Done. Here's what I created in C:\Users\ealinor\Downloads\Webapp_p2p_test\:

  What changed

  lowlatency.html — completely rewritten with:

  1. Insertable Streams (receiver.createEncodedStreams()) to intercept encoded Opus frames before the browser's jitter buffer ever
  sees them
  2. WebCodecs AudioDecoder to manually decode Opus → PCM, measuring decode time per frame
  3. Direct port injection — decoded PCM goes to the worklet via postMessage instead of through the browser's audio routing graph
  4. Per-stage timing table showing latency for each step: Capture, Encode, Network, Jitter (bypassed), Decode, Buffer, Output, and
  Total
  5. Visual pipeline with color-coded stages (green = active, red = bypassed)
  6. SDP munging to set Opus ptime to 10ms/20ms/40ms via dropdown
  7. DataChannel RTT probing + WebRTC stats polling for network latency
  8. Buffer size slider starting at 256 samples (5.3ms) instead of 960

  playout-worklet.js — updated to support two modes:

  - Mode 1 (original): reads from AudioWorkletNode inputs (for the original client)
  - Mode 2 (new): accepts samples via port.postMessage({ type: 'samples', data }) — used by the instrumented client to bypass all
  browser audio routing

  How to use

  bun run server.js
  # Open http://localhost:3000/low in two Chrome tabs

  Important notes

  - Requires Chrome/Edge — Insertable Streams (encodedInsertableStreams: true) and WebCodecs AudioDecoder are Chromium-only as of
  now. Firefox/Safari won't work.
  - The jitter buffer bypass means any network jitter goes directly to your ears as glitches. On localhost this is fine; over the
  internet you'll hear pops on congestion.
  - The timing table updates every 200ms with real measurements. Watch the "Decode" row — that's the actual measured time WebCodecs
  takes per frame (typically <1ms).
