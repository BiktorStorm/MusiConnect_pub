
  cmake_minimum_required(VERSION 3.16)
  project(MusiConnect VERSION 0.1.0 LANGUAGES C CXX)

  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)

  # =============================================================================
  # OPTIONS
  # =============================================================================
  option(USE_ASIO "Use ASIO for audio I/O (Windows)" ON)
  option(USE_JACK "Use JACK for audio I/O (Linux/macOS)" OFF)

  # =============================================================================
  # DEPENDENCIES
  # =============================================================================

  # --- Opus (with custom modes) ---
  # We build Opus from source with OPUS_CUSTOM_MODES enabled
  include(FetchContent)

  FetchContent_Declare(
      opus
      GIT_REPOSITORY https://github.com/xiph/opus.git
      GIT_TAG        v1.4
  )

  # Enable custom modes before fetching
  set(OPUS_CUSTOM_MODES ON CACHE BOOL "Enable Opus Custom Modes (CELT)" FORCE)
  set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)

  FetchContent_MakeAvailable(opus)

  # --- ASIO SDK (Windows) ---
  if(USE_ASIO)
      # You must download the ASIO SDK from Steinberg and place it here:
      # external/asio/common/asio.h, asio.cpp, etc.
      set(ASIO_SDK_DIR "${CMAKE_SOURCE_DIR}/external/asio")

      if(NOT EXISTS "${ASIO_SDK_DIR}/common/asio.h")
          message(FATAL_ERROR
              "ASIO SDK not found at ${ASIO_SDK_DIR}/common/asio.h\n"
              "Download from: https://www.steinberg.net/asiosdk\n"
              "Extract so that external/asio/common/asio.h exists.")
      endif()

      # ASIO source files
      set(ASIO_SOURCES
          ${ASIO_SDK_DIR}/common/asio.cpp
          ${ASIO_SDK_DIR}/host/asiodrivers.cpp
          ${ASIO_SDK_DIR}/host/pc/asiolist.cpp
      )

      add_library(asio_sdk STATIC ${ASIO_SOURCES})
      target_include_directories(asio_sdk PUBLIC
          ${ASIO_SDK_DIR}/common
          ${ASIO_SDK_DIR}/host
          ${ASIO_SDK_DIR}/host/pc
      )
      target_compile_definitions(asio_sdk PUBLIC ASIO_SDK)

      if(MSVC)
          target_link_libraries(asio_sdk PUBLIC ole32 uuid)
      endif()
  endif()

  # --- JACK (Linux/macOS) ---
  if(USE_JACK)
      find_package(PkgConfig REQUIRED)
      pkg_check_modules(JACK REQUIRED jack)
  endif()

  # =============================================================================
  # MAIN APPLICATION
  # =============================================================================
  add_executable(musiconnect
      src/main.cpp
      src/audio_asio.cpp
      src/audio_asio.h
      src/celt_codec.cpp
      src/celt_codec.h
      src/network.cpp
      src/network.h
      src/ring_buffer.h
  )

  target_include_directories(musiconnect PRIVATE
      ${CMAKE_SOURCE_DIR}/src
  )

  # Link Opus
  target_link_libraries(musiconnect PRIVATE opus)

  # Link audio backend
  if(USE_ASIO)
      target_link_libraries(musiconnect PRIVATE asio_sdk)
      target_compile_definitions(musiconnect PRIVATE USE_ASIO=1)
  endif()

  if(USE_JACK)
      target_include_directories(musiconnect PRIVATE ${JACK_INCLUDE_DIRS})
      target_link_libraries(musiconnect PRIVATE ${JACK_LIBRARIES})
      target_compile_definitions(musiconnect PRIVATE USE_JACK=1)
  endif()

  # Platform-specific
  if(WIN32)
      target_link_libraries(musiconnect PRIVATE ws2_32)  # Winsock for UDP
  endif()

  # =============================================================================
  # INSTALL
  # =============================================================================
  install(TARGETS musiconnect RUNTIME DESTINATION bin)
