#include <iostream>
#include <string>
#include <winsock2.h>
#include <ws2tcpip.h>

// Link with Winsock library
#pragma comment(lib, "ws2_32.lib")

constexpr int PORT = 9000;
constexpr const char* DEST_IP = "127.0.0.1"; // localhost for testing

int main() {
    // Initialize Winsock (Windows-specific requirement)
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }

    // Create a UDP socket
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        std::cerr << "Socket creation failed: " << WSAGetLastError() << "\n";
        WSACleanup();
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

        if (bytes_sent == SOCKET_ERROR) {
            std::cerr << "Send failed: " << WSAGetLastError() << "\n";
        } else {
            std::cout << "Sent " << bytes_sent << " bytes\n";
        }
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}
