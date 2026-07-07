#pragma once
// UDP transport for P2P audio over 5G.
// No jitter buffer, no retransmission. Immediate delivery.
// Packet: [4-byte seq][CELT payload]

#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <atomic>

using PacketCallback = std::function<void(const uint8_t* data, int len, uint32_t seq)>;

struct NetworkConfig {
    uint16_t localPort = 4464;
    std::string remoteHost = "127.0.0.1";
    uint16_t remotePort = 4465;
};

class UdpTransport {
public:
    UdpTransport();
    ~UdpTransport();

    bool init(const NetworkConfig& config);
    bool start();
    void stop();

    bool send(const uint8_t* data, int length);
    void setReceiveCallback(PacketCallback cb);

    struct Stats {
        uint64_t sent = 0;
        uint64_t received = 0;
        uint64_t lost = 0;
    };
    Stats getStats() const;

private:
    void receiveLoop();

    NetworkConfig m_config;
    PacketCallback m_callback;
    int m_socket = -1;
    std::thread m_recvThread;
    std::atomic<bool> m_running{false};

    std::atomic<uint64_t> m_sent{0};
    std::atomic<uint64_t> m_received{0};
    std::atomic<uint64_t> m_lost{0};
    std::atomic<uint32_t> m_seqSend{0};
    std::atomic<uint32_t> m_lastSeqRecv{0};
};
