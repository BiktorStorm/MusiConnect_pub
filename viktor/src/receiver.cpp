#include <iostream>
#include <array>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

constexpr int PORT = 9000;
constexpr int BUFFER_SIZE = 1024;

int main() {
    // Initialize Winsock
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

    // Bind the socket to listen on all interfaces at PORT
    sockaddr_in listen_addr{};
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(PORT);
    listen_addr.sin_addr.s_addr = INADDR_ANY; // Listen on all network interfaces

    if (bind(sock, reinterpret_cast<sockaddr*>(&listen_addr), sizeof(listen_addr)) == SOCKET_ERROR) {
        std::cerr << "Bind failed: " << WSAGetLastError() << "\n";
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    std::cout << "UDP Receiver — listening on port " << PORT << " (Ctrl+C to quit)\n\n";

    std::array<char, BUFFER_SIZE> buffer{};
    sockaddr_in sender_addr{};
    int sender_addr_size = sizeof(sender_addr);

    while (true) {
        // Wait for incoming data (this blocks until a packet arrives)
        int bytes_received = recvfrom(
            sock,
            buffer.data(),
            BUFFER_SIZE,
            0,
            reinterpret_cast<sockaddr*>(&sender_addr),
            &sender_addr_size
        );

        if (bytes_received == SOCKET_ERROR) {
            std::cerr << "Receive failed: " << WSAGetLastError() << "\n";
            continue;
        }

        // Get the sender's IP address as a string
        char sender_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sender_addr.sin_addr, sender_ip, INET_ADDRSTRLEN);

        std::cout << "[" << sender_ip << ":" << ntohs(sender_addr.sin_port) << "] "
                  << std::string(buffer.data(), bytes_received) << "\n";
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}
