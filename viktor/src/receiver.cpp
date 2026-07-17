#include <iostream>
#include <array>
#include <string>
#include "platform_socket.h"

constexpr int PORT = 9000;
constexpr int BUFFER_SIZE = 1024;

int main() {
    if (platform_socket_init() != 0) {
        std::cerr << "Socket initialization failed\n";
        return 1;
    }

    // Create a UDP socket
    socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCK) {
        std::cerr << "Socket creation failed: " << platform_get_error() << "\n";
        platform_socket_cleanup();
        return 1;
    }

    // Bind the socket to listen on all interfaces at PORT
    sockaddr_in listen_addr{};
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(PORT);
    listen_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, reinterpret_cast<sockaddr*>(&listen_addr), sizeof(listen_addr)) == SOCK_ERROR) {
        std::cerr << "Bind failed: " << platform_get_error() << "\n";
        platform_close_socket(sock);
        platform_socket_cleanup();
        return 1;
    }

    std::cout << "UDP Receiver — listening on port " << PORT << " (Ctrl+C to quit)\n\n";

    std::array<char, BUFFER_SIZE> buffer{};
    sockaddr_in sender_addr{};

    // socklen_t on POSIX, int on Windows — both work with this:
#ifdef _WIN32
    int sender_addr_size = sizeof(sender_addr);
#else
    socklen_t sender_addr_size = sizeof(sender_addr);
#endif

    while (true) {
        // Wait for incoming data (blocks until a packet arrives)
        int bytes_received = recvfrom(
            sock,
            buffer.data(),
            BUFFER_SIZE,
            0,
            reinterpret_cast<sockaddr*>(&sender_addr),
            &sender_addr_size
        );

        if (bytes_received == SOCK_ERROR) {
            std::cerr << "Receive failed: " << platform_get_error() << "\n";
            continue;
        }

        // Get the sender's IP address as a string
        char sender_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sender_addr.sin_addr, sender_ip, INET_ADDRSTRLEN);

        std::cout << "[" << sender_ip << ":" << ntohs(sender_addr.sin_port) << "] "
                  << std::string(buffer.data(), bytes_received) << "\n";
    }

    platform_close_socket(sock);
    platform_socket_cleanup();
    return 0;
}
