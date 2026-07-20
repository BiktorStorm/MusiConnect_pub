#pragma once
// =============================================================================
// UDP NETWORK TRANSPORT
//
// Simple peer-to-peer UDP transport for audio frames.
//
// Design choices for low latency:
//   - Raw UDP (no TCP, no retransmission, no ordering guarantees)
//   - Minimal packet header (just a sequence number for loss detection)
//   - Non-blocking I/O with a receive thread
//   - UDP hole punching for NAT traversal
//
// Packet format:
//   [0..3]  uint32_t sequence number (for loss detection, not reordering)
//   [4..N]  encoded audio frame (CELT payload)
//
// NAT Traversal (hole punching):
//   Both peers send packets to each other's public IP:port.
//   The first few packets may be lost (NAT hasn't opened yet), but once
//   one gets through, the NAT "remembers" the mapping and allows traffic.
// =============================================================================

#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <atomic>
#include <netinet/in.h>  // FIX 4: sockaddr_in is now a member, needs this header

// Callback: called when an audio packet arrives
// Parameters: encoded data pointer, data length, sequence number
using PacketCallback = std::function<void(const uint8_t*, int, uint32_t)>;

struct NetworkConfig {
    uint16_t localPort = 4464;           // Port to bind to
    std::string remoteHost = "127.0.0.1"; // Peer's IP address
    uint16_t remotePort = 4465;           // Peer's port
};

class UdpTransport {
public:
    UdpTransport();
    ~UdpTransport();

    // Initialize socket and bind to local port
    bool init(const NetworkConfig& config);

    // Start the receive thread
    bool start();

    // Stop and clean up
    void stop();

    // Send an encoded audio frame to the remote peer
    bool send(const uint8_t* data, int length);

    // Set callback for received packets
    void setReceiveCallback(PacketCallback cb);

    // Get stats
    struct Stats {
        uint64_t packetsSent = 0;
        uint64_t packetsReceived = 0;
        uint64_t packetsLost = 0;       // Detected via sequence gaps
        uint32_t lastSeqSent = 0;
        uint32_t lastSeqReceived = 0;
    };
    Stats getStats() const;

private:
    void receiveLoop();

    NetworkConfig m_config;
    PacketCallback m_callback;

    int m_socket = -1;  // Socket descriptor
    std::thread m_receiveThread;
    std::atomic<bool> m_running{false};

    // FIX 4: Cached remote address — resolved once in init() instead of
    // rebuilding the sockaddr_in and calling inet_pton on every send().
    sockaddr_in m_remoteAddr{};

    // Stats
    std::atomic<uint64_t> m_packetsSent{0};
    std::atomic<uint64_t> m_packetsReceived{0};
    std::atomic<uint64_t> m_packetsLost{0};
    std::atomic<uint32_t> m_seqSend{0};
    std::atomic<uint32_t> m_lastSeqReceived{0};
};
