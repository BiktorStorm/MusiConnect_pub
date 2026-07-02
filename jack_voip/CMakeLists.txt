
  cmake_minimum_required(VERSION 3.20)
  project(jack_voip LANGUAGES CXX)

  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)

  # ─── Find JACK Audio ────────────────────────────────────────────────────────────
  # JACK is expected to be installed as a system library.
  # On Linux:   sudo apt install libjack-jackd2-dev
  # On macOS:   brew install jack
  # On Windows: Install JACK2 from https://jackaudio.org/downloads/
  find_path(JACK_INCLUDE_DIR jack/jack.h
      HINTS
          $ENV{JACK_DIR}/include
          "C:/Program Files/JACK2/include"
          /usr/include
          /usr/local/include
  )

  find_library(JACK_LIBRARY
      NAMES jack jack64 libjack libjack64
      HINTS
          $ENV{JACK_DIR}/lib
          "C:/Program Files/JACK2/lib"
          /usr/lib
          /usr/local/lib
          /usr/lib/x86_64-linux-gnu
  )

  if(NOT JACK_INCLUDE_DIR OR NOT JACK_LIBRARY)
      message(FATAL_ERROR
          "JACK Audio not found!\n"
          "Install JACK2:\n"
          "  Linux:   sudo apt install libjack-jackd2-dev\n"
          "  macOS:   brew install jack\n"
          "  Windows: https://jackaudio.org/downloads/\n"
          "Or set JACK_DIR environment variable to your JACK installation."
      )
  endif()

  message(STATUS "JACK include: ${JACK_INCLUDE_DIR}")
  message(STATUS "JACK library: ${JACK_LIBRARY}")

  # ─── Fetch libopus ──────────────────────────────────────────────────────────────
  include(FetchContent)
  FetchContent_Declare(
      opus
      GIT_REPOSITORY https://gitlab.xiph.org/xiph/opus.git
      GIT_TAG        v1.4
  )
  set(OPUS_CUSTOM_MODES ON CACHE BOOL "Enable Opus Custom Modes (CELT)" FORCE)
  set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(opus)

  # ─── Application ────────────────────────────────────────────────────────────────
  add_executable(jack_voip
      src/main.cpp
      src/audio_jack.cpp
      src/celt_codec.cpp
      src/network.cpp
  )

  target_include_directories(jack_voip PRIVATE
      ${JACK_INCLUDE_DIR}
      ${opus_SOURCE_DIR}/include
      src
  )

  target_link_libraries(jack_voip PRIVATE
      ${JACK_LIBRARY}
      opus
  )

  # Platform-specific networking libraries
  if(WIN32)
      target_link_libraries(jack_voip PRIVATE ws2_32)
      target_compile_definitions(jack_voip PRIVATE _WIN32_WINNT=0x0A00 NOMINMAX)
  elseif(APPLE)
      # No extra libs needed on macOS
  else()
      target_link_libraries(jack_voip PRIVATE pthread)
  endif()

  # Realtime priority hint
  if(UNIX AND NOT APPLE)
      target_link_libraries(jack_voip PRIVATE rt)
  endif()

