
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

     cd /mnt/c/Users/ealinor/Downloads/Webapp_p2p_test/celt
     chmod +x build_wasm.sh
     ./build_wasm.sh

  3. Run:

     cd /mnt/c/Users/ealinor/Downloads/Webapp_p2p_test
     bun run server.js
     # Open http://localhost:3000/celt in two Chrome tabs

  The theoretical latency budget with CELT on localhost:

  ┌───────────────────────────┬─────────────────────────────┐
  │ Stage                     │ Time                        │
  ├───────────────────────────┼─────────────────────────────┤
  │ Capture (64 samples)      │ 1.33ms                      │
  ├───────────────────────────┼─────────────────────────────┤
  │ CELT encode (WASM)        │ ~0.1ms                      │
  ├───────────────────────────┼─────────────────────────────┤
  │ DataChannel (localhost)   │ ~0.1ms                      │
  ├───────────────────────────┼─────────────────────────────┤
  │ CELT decode (WASM)        │ ~0.1ms                      │
  ├───────────────────────────┼─────────────────────────────┤
  │ Play buffer (192 samples) │ 4.0ms                       │
  ├───────────────────────────┼─────────────────────────────┤
  │ Output DAC                │ 5-25ms (hardware dependent) │
  ├───────────────────────────┼─────────────────────────────┤
  │ Total                     │ ~10-30ms                    │
  └───────────────────────────┴─────────────────────────────┘
