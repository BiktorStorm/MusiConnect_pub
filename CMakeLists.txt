cmake_minimum_required(VERSION 3.14)
project(audio_transceiver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Find PortAudio ---
# Option 1: system-installed PortAudio (pkg-config or CMake find module)
# Option 2: specify manually with -DPORTAUDIO_ROOT=<path>
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PORTAUDIO portaudio-2.0)
endif()

if(NOT PORTAUDIO_FOUND)
    # Manual fallback: user can set PORTAUDIO_ROOT
    if(DEFINED PORTAUDIO_ROOT)
        set(PORTAUDIO_INCLUDE_DIRS "${PORTAUDIO_ROOT}/include")
        if(WIN32)
            set(PORTAUDIO_LIBRARY_DIRS "${PORTAUDIO_ROOT}/lib")
            set(PORTAUDIO_LIBRARIES "portaudio_x64")
        else()
            set(PORTAUDIO_LIBRARY_DIRS "${PORTAUDIO_ROOT}/lib")
            set(PORTAUDIO_LIBRARIES "portaudio")
        endif()
    else()
        # Try CMake's find_library as last resort
        find_path(PORTAUDIO_INCLUDE_DIRS portaudio.h)
        find_library(PORTAUDIO_LIBRARIES NAMES portaudio portaudio_x64)
        if(NOT PORTAUDIO_INCLUDE_DIRS OR NOT PORTAUDIO_LIBRARIES)
            message(FATAL_ERROR
                "PortAudio not found. Install it or set -DPORTAUDIO_ROOT=<path>\n"
                "  macOS:   brew install portaudio\n"
                "  Linux:   sudo apt install libportaudio2 portaudio19-dev\n"
                "  Windows: download from https://www.portaudio.com or use vcpkg")
        endif()
    endif()
endif()

# --- Build the transceiver ---
add_executable(audio_transceiver audio_transceiver.cpp)

target_include_directories(audio_transceiver PRIVATE ${PORTAUDIO_INCLUDE_DIRS})
if(DEFINED PORTAUDIO_LIBRARY_DIRS)
    target_link_directories(audio_transceiver PRIVATE ${PORTAUDIO_LIBRARY_DIRS})
endif()
target_link_libraries(audio_transceiver PRIVATE ${PORTAUDIO_LIBRARIES})

# --- Platform-specific link libraries ---
if(WIN32)
    target_link_libraries(audio_transceiver PRIVATE ws2_32)
elseif(APPLE)
    target_link_libraries(audio_transceiver PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework CoreFoundation")
else()
    # Linux: PortAudio typically links against ALSA/pthread already,
    # but we ensure pthread is linked.
    find_package(Threads REQUIRED)
    target_link_libraries(audio_transceiver PRIVATE Threads::Threads)
endif()

# --- iOS audio session (optional .mm file) ---
if(APPLE AND CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # If you have ios_audio_session.mm, add it here:
    # target_sources(audio_transceiver PRIVATE ios_audio_session.mm)
    target_link_libraries(audio_transceiver PRIVATE "-framework AVFoundation")
endif()
