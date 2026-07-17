#pragma once

// Cross-platform socket compatibility layer.
// On Windows: uses Winsock2. On POSIX (macOS/Linux): uses BSD sockets.

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <winsock2.h>
    #include <ws2tcpip.h>

    using socket_t = SOCKET;
    constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
    constexpr int SOCK_ERROR = SOCKET_ERROR;

    inline int platform_socket_init() {
        WSADATA wsa_data;
        return WSAStartup(MAKEWORD(2, 2), &wsa_data);
    }

    inline void platform_socket_cleanup() {
        WSACleanup();
    }

    inline int platform_close_socket(socket_t s) {
        return closesocket(s);
    }

    inline int platform_get_error() {
        return WSAGetLastError();
    }

#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <cerrno>

    using socket_t = int;
    constexpr socket_t INVALID_SOCK = -1;
    constexpr int SOCK_ERROR = -1;

    inline int platform_socket_init() {
        // No initialization needed on POSIX
        return 0;
    }

    inline void platform_socket_cleanup() {
        // No cleanup needed on POSIX
    }

    inline int platform_close_socket(socket_t s) {
        return close(s);
    }

    inline int platform_get_error() {
        return errno;
    }

#endif
