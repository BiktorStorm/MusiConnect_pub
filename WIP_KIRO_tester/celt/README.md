// Latency is still aroud 60 ms 
baseLatency: 10 ms, outputLatency: 40 ms?

  C source code (libopus)
      ↓ Emscripten compiler (emcc)
  WebAssembly binary (.wasm)
      ↓ Browser loads it
  JavaScript calls WASM functions like native functions

  The C code runs at near-native speed inside the browser's WASM sandbox. You don't need to know C to use it — the build_wasm.sh
   script handles compilation, and from JavaScript you just call:

  const Module = await createOpusCelt();
  const init = Module.cwrap('celt_init', 'number', ['number','number','number','number','number']);
  init(48000, 64, 1, 64000, 1);  // 48kHz, 64 samples/frame, mono, 64kbps, complexity 1

  To get this running

  1. Install Emscripten (in WSL2 on your Windows machine — easiest):

     git clone https://github.com/emscripten-core/emsdk.git
     cd emsdk && ./emsdk install latest && ./emsdk activate latest && source ./emsdk_env.sh

  2. Build:

     cd /mnt/c/Users/.../celt
     in particular: cd '/c/Users/ealinor/OneDrive - Ericsson/Desktop/WIP_KIRO_tester/celt'

     chmod +x build_wasm.sh
     ./build_wasm.sh
     in particular: bash build_wasm.sh

  4. Run:

     cd /mnt/c/Users/(PATH innan celt)
     bun run server.js
     # Open http://localhost:3000/celt in two Chrome tabs

________________________________
Check of latency speed in browser console: (ctrl + shift + J on windows)
const temp = new AudioContext(); 
console.log('baseLatency:', temp.baseLatency * 1000, 'ms');
console.log('outputLatency:', temp.outputLatency * 1000, 'ms');

