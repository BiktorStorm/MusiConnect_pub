cmake_minimum_required(VERSION 3.16)
project(MusiConnect VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

# =============================================================================
# MAIN APPLICATION
# =============================================================================
add_executable(musiconnect
    src/main.cpp
    src/audio_coreaudio.cpp
    src/audio_coreaudio.h
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

# Link macOS Core Audio frameworks
find_library(COREAUDIO_FRAMEWORK CoreAudio REQUIRED)
find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox REQUIRED)
find_library(AUDIOUNIT_FRAMEWORK AudioUnit REQUIRED)
find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)

target_link_libraries(musiconnect PRIVATE
    ${COREAUDIO_FRAMEWORK}
    ${AUDIOTOOLBOX_FRAMEWORK}
    ${AUDIOUNIT_FRAMEWORK}
    ${COREFOUNDATION_FRAMEWORK}
)

# =============================================================================
# INSTALL
# =============================================================================
install(TARGETS musiconnect RUNTIME DESTINATION bin)
