// =============================================================================
// UDP NETWORK TRANSPORT IMPLEMENTATION
//
// Uses POSIX sockets on macOS.
// Optimized for minimum latency:
//   - No Nagle algorithm (irrelevant for UDP but SO_SNDBUF minimized)
//   - Non-blocking receive with tight poll loop
//   - Minimal packet header (4 bytes sequence number)
// =============================================================================

#include "network.h"
#include <iostream>
#include <cstring>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

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
    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket < 0) {
        std::cerr << "[NET] Failed to create socket" << std::endl;
        return false;
    }

    // Bind to local port
    sockaddr_in localAddr{};
    localAddr.sin_family = AF_INET;
    localAddr.sin_port = htons(config.localPort);
    localAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(m_socket, reinterpret_cast<sockaddr*>(&localAddr), sizeof(localAddr)) < 0) {
        std::cerr << "[NET] Failed to bind to port " << config.localPort << std::endl;
        close(m_socket);
        m_socket = -1;
        return false;
    }

    // Set socket to non-blocking
    int flags = fcntl(m_socket, F_GETFL, 0);
    fcntl(m_socket, F_SETFL, flags | O_NONBLOCK);

    // Reduce socket send buffer for lower latency
    int bufSize = 4096;  // Small send buffer — we don't want queuing
    setsockopt(m_socket, SOL_SOCKET, SO_SNDBUF, &bufSize, sizeof(bufSize));
    setsockopt(m_socket, SOL_SOCKET, SO_RCVBUF, &bufSize, sizeof(bufSize));

    std::cout << "[NET] Bound to port " << config.localPort
              << ", remote: " << config.remoteHost << ":" << config.remotePort << std::endl;

    // FIX 4: Resolve and cache the remote address once here so send() never
    // needs to call inet_pton or rebuild the sockaddr_in on the hot path.
    m_remoteAddr = {};
    m_remoteAddr.sin_family = AF_INET;
    m_remoteAddr.sin_port = htons(config.remotePort);
    inet_pton(AF_INET, config.remoteHost.c_str(), &m_remoteAddr.sin_addr);

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
        close(m_socket);
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

    // FIX 4: Use the cached remote address instead of rebuilding it each call.
    ssize_t sent = sendto(m_socket, packet, length + HEADER_SIZE, 0,
                          reinterpret_cast<const sockaddr*>(&m_remoteAddr), sizeof(m_remoteAddr));

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
    uint8_t buffer[MAX_PACKET_SIZE];
    sockaddr_in senderAddr{};
    socklen_t senderLen = sizeof(senderAddr);

    while (m_running) {
        // Poll with short timeout to allow clean shutdown
        struct pollfd pfd{};
        pfd.fd = m_socket;
        pfd.events = POLLIN;
        int ready = poll(&pfd, 1, 1);  // 1ms timeout

        if (ready <= 0) continue;

        ssize_t received = recvfrom(m_socket, buffer, MAX_PACKET_SIZE, 0,
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
                m_callback(buffer + HEADER_SIZE, (int)(received - HEADER_SIZE), seq);
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
