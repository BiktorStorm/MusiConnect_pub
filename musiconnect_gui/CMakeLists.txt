cmake_minimum_required(VERSION 3.16)
project(MusiConnectGUI VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Opus v1.4 still declares compatibility with older CMake versions.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# =============================================================================
# DEPENDENCIES via FetchContent
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

# --- GLFW (windowing + OpenGL context) ---
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.3.9
)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# --- Dear ImGui (no CMake project — we include sources directly) ---
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.90.4
)
FetchContent_MakeAvailable(imgui)

# Create an ImGui library target from source files
set(IMGUI_DIR ${imgui_SOURCE_DIR})
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)
target_link_libraries(imgui PUBLIC glfw)

# =============================================================================
# PLATFORM-SPECIFIC AUDIO SOURCES
# =============================================================================

# Shared sources (codec, network, ring buffer are the same on both platforms)
set(SHARED_AUDIO_SOURCES
    ${CMAKE_SOURCE_DIR}/../asio_app_test_mac/src/celt_codec.cpp
    ${CMAKE_SOURCE_DIR}/../asio_app_test_mac/src/celt_codec.h
    ${CMAKE_SOURCE_DIR}/../asio_app_test_mac/src/network.cpp
    ${CMAKE_SOURCE_DIR}/../asio_app_test_mac/src/network.h
    ${CMAKE_SOURCE_DIR}/../asio_app_test_mac/src/ring_buffer.h
)

if(APPLE)
    set(PLATFORM_AUDIO_SOURCES
        ${CMAKE_SOURCE_DIR}/../asio_app_test_mac/src/audio_coreaudio.cpp
        ${CMAKE_SOURCE_DIR}/../asio_app_test_mac/src/audio_coreaudio.h
    )
    set(PLATFORM_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/../asio_app_test_mac/src)
elseif(WIN32)
    set(PLATFORM_AUDIO_SOURCES
        ${CMAKE_SOURCE_DIR}/../asio_app_test/src/audio_asio.cpp
        ${CMAKE_SOURCE_DIR}/../asio_app_test/src/audio_asio.h
    )
    set(PLATFORM_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/../asio_app_test/src)
endif()

# =============================================================================
# MAIN APPLICATION
# =============================================================================
add_executable(musiconnect_gui
    src/main.cpp
    src/audio_engine.cpp
    src/audio_engine.h
    ${SHARED_AUDIO_SOURCES}
    ${PLATFORM_AUDIO_SOURCES}
)

target_include_directories(musiconnect_gui PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${PLATFORM_INCLUDE_DIR}
)

# Link ImGui + GLFW + OpenGL
target_link_libraries(musiconnect_gui PRIVATE imgui opus)

# =============================================================================
# PLATFORM-SPECIFIC LINKING
# =============================================================================
if(APPLE)
    # macOS: Core Audio frameworks + OpenGL
    find_library(COREAUDIO_FRAMEWORK CoreAudio REQUIRED)
    find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox REQUIRED)
    find_library(AUDIOUNIT_FRAMEWORK AudioUnit REQUIRED)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    find_library(OPENGL_FRAMEWORK OpenGL REQUIRED)

    target_link_libraries(musiconnect_gui PRIVATE
        ${COREAUDIO_FRAMEWORK}
        ${AUDIOTOOLBOX_FRAMEWORK}
        ${AUDIOUNIT_FRAMEWORK}
        ${COREFOUNDATION_FRAMEWORK}
        ${OPENGL_FRAMEWORK}
    )
elseif(WIN32)
    # Windows: OpenGL + Winsock
    find_package(OpenGL REQUIRED)
    target_link_libraries(musiconnect_gui PRIVATE
        OpenGL::GL
        ws2_32      # Winsock for UDP
    )

    # Hide console window for GUI app
    set_target_properties(musiconnect_gui PROPERTIES
        WIN32_EXECUTABLE TRUE
    )
endif()

# =============================================================================
# INSTALL
# =============================================================================
install(TARGETS musiconnect_gui RUNTIME DESTINATION bin)
