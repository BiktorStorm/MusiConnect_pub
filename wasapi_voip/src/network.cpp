// =============================================================================
// UDP NETWORK TRANSPORT IMPLEMENTATION (Windows)
//
// Uses Winsock2 on Windows.
// Optimized for minimum latency:
//   - Minimal socket buffers (no queuing)
//   - Non-blocking receive with WSAPoll
//   - Minimal packet header (4 bytes sequence number)
// =============================================================================

#include "network.h"
#include <iostream>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

// Packet header: just a sequence number (4 bytes)
static constexpr int HEADER_SIZE = 4;
static constexpr int MAX_PACKET_SIZE = 512;  // More than enough for one CELT frame

UdpTransport::UdpTransport() = default;

UdpTransport::~UdpTransport() {
    stop();
}

bool UdpTransport::init(const NetworkConfig& config) {
    m_config = config;

    // Create UDP socket
    m_socket = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCKET) {
        std::cerr << "[NET] Failed to create socket: " << WSAGetLastError() << std::endl;
        return false;
    }

    // Bind to local port
    sockaddr_in localAddr{};
    localAddr.sin_family = AF_INET;
    localAddr.sin_port = htons(config.localPort);
    localAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind((SOCKET)m_socket, reinterpret_cast<sockaddr*>(&localAddr), sizeof(localAddr)) == SOCKET_ERROR) {
        std::cerr << "[NET] Failed to bind to port " << config.localPort
                  << ": " << WSAGetLastError() << std::endl;
        closesocket((SOCKET)m_socket);
        m_socket = -1;
        return false;
    }

    // Set socket to non-blocking
    u_long nonBlocking = 1;
    ioctlsocket((SOCKET)m_socket, FIONBIO, &nonBlocking);

    // Reduce socket send buffer for lower latency
    int bufSize = 4096;
    setsockopt((SOCKET)m_socket, SOL_SOCKET, SO_SNDBUF, (const char*)&bufSize, sizeof(bufSize));
    setsockopt((SOCKET)m_socket, SOL_SOCKET, SO_RCVBUF, (const char*)&bufSize, sizeof(bufSize));

    std::cout << "[NET] Bound to port " << config.localPort
              << ", remote: " << config.remoteHost << ":" << config.remotePort << std::endl;

    return true;
}

bool UdpTransport::start() {
    m_running = true;
    m_receiveThread = std::thread(&UdpTransport::receiveLoop, this);
    std::cout << "[NET] Receive thread started" << std::endl;
    return true;
}

void UdpTransport::stop() {
    m_running = false;
    if (m_receiveThread.joinable()) {
        m_receiveThread.join();
    }
    if (m_socket != -1) {
        closesocket((SOCKET)m_socket);
        m_socket = -1;
    }
    std::cout << "[NET] Stopped" << std::endl;
}

bool UdpTransport::send(const uint8_t* data, int length) {
    if (m_socket == -1 || length <= 0) return false;

    // Build packet: [4-byte sequence][payload]
    uint8_t packet[MAX_PACKET_SIZE];
    if (length + HEADER_SIZE > MAX_PACKET_SIZE) return false;

    uint32_t seq = m_seqSend.fetch_add(1, std::memory_order_relaxed);
    memcpy(packet, &seq, 4);
    memcpy(packet + HEADER_SIZE, data, length);

    // Send to remote peer
    sockaddr_in remoteAddr{};
    remoteAddr.sin_family = AF_INET;
    remoteAddr.sin_port = htons(m_config.remotePort);
    inet_pton(AF_INET, m_config.remoteHost.c_str(), &remoteAddr.sin_addr);

    int sent = sendto((SOCKET)m_socket, (const char*)packet, length + HEADER_SIZE, 0,
                      reinterpret_cast<sockaddr*>(&remoteAddr), sizeof(remoteAddr));

    if (sent > 0) {
        m_packetsSent.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

void UdpTransport::setReceiveCallback(PacketCallback cb) {
    m_callback = std::move(cb);
}

void UdpTransport::receiveLoop() {
    char buffer[MAX_PACKET_SIZE];
    sockaddr_in senderAddr{};
    int senderLen = sizeof(senderAddr);

    while (m_running) {
        // Poll with short timeout to allow clean shutdown
        WSAPOLLFD pfd{};
        pfd.fd = (SOCKET)m_socket;
        pfd.events = POLLIN;
        int ready = WSAPoll(&pfd, 1, 1);  // 1ms timeout

        if (ready <= 0) continue;

        int received = recvfrom((SOCKET)m_socket, buffer, MAX_PACKET_SIZE, 0,
                                reinterpret_cast<sockaddr*>(&senderAddr), &senderLen);

        if (received > HEADER_SIZE) {
            // Parse header
            uint32_t seq;
            memcpy(&seq, buffer, 4);

            // Detect packet loss (sequence gaps)
            uint32_t lastSeq = m_lastSeqReceived.load(std::memory_order_relaxed);
            if (seq > lastSeq + 1 && lastSeq > 0) {
                uint32_t lost = seq - lastSeq - 1;
                m_packetsLost.fetch_add(lost, std::memory_order_relaxed);
            }
            m_lastSeqReceived.store(seq, std::memory_order_relaxed);
            m_packetsReceived.fetch_add(1, std::memory_order_relaxed);

            // Deliver payload to callback
            if (m_callback) {
                m_callback(reinterpret_cast<const uint8_t*>(buffer + HEADER_SIZE),
                           received - HEADER_SIZE, seq);
            }
        }
    }
}

UdpTransport::Stats UdpTransport::getStats() const {
    return {
        m_packetsSent.load(),
        m_packetsReceived.load(),
        m_packetsLost.load(),
        m_seqSend.load(),
        m_lastSeqReceived.load()
    };
}
