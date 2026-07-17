cmake_minimum_required(VERSION 3.21)

project(viktor_voicechat
    VERSION 0.1.0
    LANGUAGES CXX
    DESCRIPTION "Peer-to-peer low latency voice chat"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Main application
add_executable(${PROJECT_NAME}
    src/main.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

# UDP Sender (test tool)
add_executable(sender
    src/sender.cpp
)
target_include_directories(sender PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

# UDP Receiver (test tool)
add_executable(receiver
    src/receiver.cpp
)
target_include_directories(receiver PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

# Link Winsock only on Windows
if(WIN32)
    target_link_libraries(sender PRIVATE ws2_32)
    target_link_libraries(receiver PRIVATE ws2_32)
endif()
