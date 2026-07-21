cmake_minimum_required(VERSION 3.16)
project(MusiConnect_WASAPI_GUI VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Allow Opus v1.4's old cmake_minimum_required to work with newer CMake
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

# =============================================================================
# DEPENDENCIES
# =============================================================================

include(FetchContent)

# --- Opus (with custom modes for CELT) ---
FetchContent_Declare(
    opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG        v1.4
)
set(OPUS_CUSTOM_MODES ON CACHE BOOL "Enable Opus Custom Modes (CELT)" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(opus)

# --- nlohmann/json (header-only, for JSON protocol with GUI) ---
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(json)

# =============================================================================
# MAIN APPLICATION
# =============================================================================
add_executable(musiconnect
    src/main.cpp
    src/audio_wasapi.cpp
    src/audio_wasapi.h
    src/celt_codec.cpp
    src/celt_codec.h
    src/network.cpp
    src/network.h
    src/ring_buffer.h
)

target_include_directories(musiconnect PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

# Link dependencies
target_link_libraries(musiconnect PRIVATE
    opus
    nlohmann_json::nlohmann_json
)

# Link Windows system libraries for WASAPI + networking
target_link_libraries(musiconnect PRIVATE
    ole32           # COM (CoInitialize, CoCreateInstance)
    avrt            # MMCSS (AvSetMmThreadCharacteristics)
    ws2_32          # Winsock2 (UDP networking)
)

# Windows defines
target_compile_definitions(musiconnect PRIVATE
    _WIN32_WINNT=0x0A00   # Target Windows 10+
    NOMINMAX              # Prevent min/max macros
    WIN32_LEAN_AND_MEAN   # Exclude rarely-used Windows headers
)

# =============================================================================
# INSTALL
# =============================================================================
install(TARGETS musiconnect RUNTIME DESTINATION bin)
