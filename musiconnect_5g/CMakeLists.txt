cmake_minimum_required(VERSION 3.16...4.3)
project(musiconnect_5g VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Auto-detect platform
if(WIN32)
    option(USE_ASIO "Use ASIO audio (Windows)" ON)
elseif(APPLE)
    option(USE_COREAUDIO "Use CoreAudio (macOS)" ON)
endif()

# --- Opus with custom modes ---
include(FetchContent)
FetchContent_Declare(opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG        v1.4
)
set(OPUS_CUSTOM_MODES ON CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(opus)

# --- Platform audio sources ---
set(AUDIO_SOURCES "")

if(USE_ASIO)
    set(ASIO_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/asio")
    if(NOT EXISTS "${ASIO_SDK_DIR}/common/asio.h")
        message(FATAL_ERROR
            "ASIO SDK not found at ${ASIO_SDK_DIR}/common/asio.h\n"
            "Download from https://www.steinberg.net/asiosdk and extract to external/asio/")
    endif()

    add_library(asio_sdk STATIC
        ${ASIO_SDK_DIR}/common/asio.cpp
        ${ASIO_SDK_DIR}/host/asiodrivers.cpp
        ${ASIO_SDK_DIR}/host/pc/asiolist.cpp
    )
    target_include_directories(asio_sdk PUBLIC
        ${ASIO_SDK_DIR}/common
        ${ASIO_SDK_DIR}/host
        ${ASIO_SDK_DIR}/host/pc
    )
    if(MSVC)
        target_link_libraries(asio_sdk PUBLIC ole32 uuid)
    endif()

    list(APPEND AUDIO_SOURCES src/audio_asio.cpp)
endif()

if(USE_COREAUDIO)
    list(APPEND AUDIO_SOURCES src/audio_coreaudio.cpp)
endif()

# --- Main executable ---
add_executable(musiconnect_5g
    src/main.cpp
    src/celt_codec.cpp
    src/network.cpp
    ${AUDIO_SOURCES}
)

target_include_directories(musiconnect_5g PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(musiconnect_5g PRIVATE opus)

if(USE_ASIO)
    target_link_libraries(musiconnect_5g PRIVATE asio_sdk)
    target_compile_definitions(musiconnect_5g PRIVATE USE_ASIO=1)
endif()

if(USE_COREAUDIO)
    target_link_libraries(musiconnect_5g PRIVATE
        "-framework AudioToolbox"
        "-framework CoreAudio"
        "-framework CoreFoundation"
    )
    target_compile_definitions(musiconnect_5g PRIVATE USE_COREAUDIO=1)
endif()

if(WIN32)
    target_link_libraries(musiconnect_5g PRIVATE ws2_32)
endif()

install(TARGETS musiconnect_5g RUNTIME DESTINATION bin)
