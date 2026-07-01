# =============================================================================
  # BUILD SCRIPT: Compile libopus (with Custom Modes) + wrapper to WASM
  #
  # PREREQUISITES:
  #   1. Emscripten SDK installed and activated:
  #      git clone https://github.com/emscripten-core/emsdk.git
  #      cd emsdk && ./emsdk install latest && ./emsdk activate latest
  #      source ./emsdk_env.sh
  #
  #   2. Standard build tools (make, autoconf, automake, libtool)
  #      On Ubuntu: sudo apt install autoconf automake libtool
  #      On macOS:  brew install autoconf automake libtool
  #
  # USAGE:
  #   chmod +x build_wasm.sh
  #   ./build_wasm.sh
  #
  # OUTPUT:
  #   opus_celt.js    — JavaScript glue code (loads the WASM)
  #   opus_celt.wasm  — The compiled WebAssembly binary
  # =============================================================================

  set -e

  SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
  OPUS_DIR="$SCRIPT_DIR/opus"
  BUILD_DIR="$SCRIPT_DIR/build"
  OUTPUT_DIR="$SCRIPT_DIR"

  echo "=== OPUS CELT WASM Builder ==="
  echo ""

  # --- Step 1: Clone opus if not present ---
  if [ ! -d "$OPUS_DIR" ]; then
      echo "[1/4] Cloning libopus..."
      git clone https://github.com/xiph/opus.git "$OPUS_DIR"
      cd "$OPUS_DIR"
      # Use a stable release tag
      git checkout v1.4
  else
      echo "[1/4] libopus already present, skipping clone"
      cd "$OPUS_DIR"
  fi

  # --- Step 2: Configure opus with custom modes enabled ---
  echo "[2/4] Configuring libopus with Emscripten (custom modes enabled)..."

  if [ ! -f "configure" ]; then
      ./autogen.sh
  fi

  mkdir -p "$BUILD_DIR"

  # Configure with Emscripten
  emconfigure ./configure \
      --prefix="$BUILD_DIR" \
      --host=wasm32-unknown-emscripten \
      --enable-custom-modes \
      --disable-shared \
      --enable-static \
      --disable-doc \
      --disable-extra-programs \
      --disable-stack-protector \
      CFLAGS="-O3 -fno-exceptions"

  # --- Step 3: Build static library ---
  echo "[3/4] Building libopus static library..."
  emmake make clean 2>/dev/null || true
  emmake make -j$(nproc 2>/dev/null || echo 4)
  emmake make install

  # --- Step 4: Compile wrapper + link to WASM ---
  echo "[4/4] Compiling WASM module..."

  cd "$SCRIPT_DIR"

  emcc -O3 \
      opus_celt_wrapper.c \
      -I"$BUILD_DIR/include" \
      -L"$BUILD_DIR/lib" \
      -lopus \
      -o opus_celt.js \
      -s MODULARIZE=1 \
      -s EXPORT_NAME="createOpusCelt" \
      -s EXPORTED_FUNCTIONS='[ \
          "_celt_init", \
          "_celt_encode_buffer", \
          "_celt_decode_buffer", \
          "_celt_encode", \
          "_celt_decode", \
          "_celt_destroy", \
          "_get_pcm_ptr", \
          "_get_encoded_ptr", \
          "_get_frame_size", \
          "_get_channels", \
          "_malloc", \
          "_free" \
      ]' \
      -s EXPORTED_RUNTIME_METHODS='["cwrap","getValue","setValue","HEAPF32","HEAPU8"]' \
      -s WASM=1 \
      -s ALLOW_MEMORY_GROWTH=1 \
      -s INITIAL_MEMORY=4194304 \
      -s STACK_SIZE=65536 \
      -s NO_EXIT_RUNTIME=1 \
      -s ENVIRONMENT="web,worker" \
      --no-entry

  echo ""
  echo "=== Build complete! ==="
  echo "Output files:"
  echo "  $OUTPUT_DIR/opus_celt.js   (JS glue — load this in your page)"
  echo "  $OUTPUT_DIR/opus_celt.wasm (WASM binary — served alongside JS)"
  echo ""
  echo "Usage in HTML:"
  echo '  <script src="opus_celt.js"></script>'
  echo '  <script>'
  echo '    const Module = await createOpusCelt();'
  echo '    const init = Module.cwrap("celt_init", "number", ["number","number","number","number","number"]);'
  echo '    init(48000, 64, 1, 64000, 1);'
  echo '  </script>'
