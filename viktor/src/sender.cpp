#include <iostream>
#include <string>
#include "platform_socket.h"

constexpr int PORT = 9000;
constexpr const char* DEST_IP = "127.0.0.1"; // localhost for testing

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

    // Set up the destination address
    sockaddr_in dest_addr{};
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, DEST_IP, &dest_addr.sin_addr);

    std::cout << "UDP Sender — type messages and press Enter to send (Ctrl+C to quit)\n";
    std::cout << "Sending to " << DEST_IP << ":" << PORT << "\n\n";

    std::string message;
    while (std::getline(std::cin, message)) {
        int bytes_sent = sendto(
            sock,
            message.c_str(),
            static_cast<int>(message.size()),
            0,
            reinterpret_cast<sockaddr*>(&dest_addr),
            sizeof(dest_addr)
        );

        if (bytes_sent == SOCK_ERROR) {
            std::cerr << "Send failed: " << platform_get_error() << "\n";
        } else {
            std::cout << "Sent " << bytes_sent << " bytes\n";
        }
    }

    platform_close_socket(sock);
    platform_socket_cleanup();
    return 0;
}
