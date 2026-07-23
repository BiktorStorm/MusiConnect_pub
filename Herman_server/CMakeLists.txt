cmake_minimum_required(VERSION 3.15)
project(musiconnect_signaling LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- signaling server (standalone, no audio deps) ----
add_executable(signaling_server
    signaling_server.cpp
)

# ---- demo client (signaling + hole punch, no audio deps) ----
add_executable(demo_client
    demo_client.cpp
    signaling_client.cpp
)

# Winsock on Windows
if (WIN32)
    target_link_libraries(signaling_server PRIVATE ws2_32)
    target_link_libraries(demo_client      PRIVATE ws2_32)
endif()

# The signaling code is self-contained. To integrate the client into the audio
# app, add signaling_client.cpp to that target and #include "signaling_client.h".
