#include "network.h"
#include <iostream>
#include <cstring>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    using socklen_t = int;
    #define CLOSE_SOCKET closesocket
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/ip.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <poll.h>
    #include <pthread.h>
    #define CLOSE_SOCKET close
#endif

static constexpr int HEADER_SIZE = 4;
static constexpr int MAX_PACKET = 512;

UdpTransport::UdpTransport() = default;

UdpTransport::~UdpTransport() { stop(); }

bool UdpTransport::init(const NetworkConfig& config) {
    m_config = config;

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#endif

    m_socket = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket < 0) return false;

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(config.localPort);
    local.sin_addr.s_addr = INADDR_ANY;

    if (bind(m_socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
        CLOSE_SOCKET(m_socket);
        m_socket = -1;
        return false;
    }

    // Non-blocking
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(m_socket, FIONBIO, &nb);
#else
    fcntl(m_socket, F_SETFL, fcntl(m_socket, F_GETFL, 0) | O_NONBLOCK);
#endif

    // Minimize socket buffers to prevent OS queuing
    int bufSz = 4096;
    setsockopt(m_socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char*>(&bufSz), sizeof(bufSz));
    setsockopt(m_socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&bufSz), sizeof(bufSz));

    // DSCP EF (46) — real-time priority for 5G QoS
    int dscp = 46 << 2;
    setsockopt(m_socket, IPPROTO_IP, IP_TOS, reinterpret_cast<char*>(&dscp), sizeof(dscp));

    std::cout << "[NET] Bound :" << config.localPort
              << " -> " << config.remoteHost << ":" << config.remotePort << "\n";
    return true;
}

bool UdpTransport::start() {
    m_running = true;
    m_recvThread = std::thread(&UdpTransport::receiveLoop, this);

#ifdef _WIN32
    SetThreadAffinityMask(m_recvThread.native_handle(), 1ULL << 1);
    SetThreadPriority(m_recvThread.native_handle(), THREAD_PRIORITY_TIME_CRITICAL);
#else
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(1, &cpuset);
    pthread_setaffinity_np(m_recvThread.native_handle(), sizeof(cpuset), &cpuset);
    sched_param sp{};
    sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
    pthread_setschedparam(m_recvThread.native_handle(), SCHED_FIFO, &sp);
#endif

    return true;
}

void UdpTransport::stop() {
    m_running = false;
    if (m_recvThread.joinable()) m_recvThread.join();
    if (m_socket >= 0) {
        CLOSE_SOCKET(m_socket);
        m_socket = -1;
    }
#ifdef _WIN32
    WSACleanup();
#endif
}

bool UdpTransport::send(const uint8_t* data, int length) {
    if (m_socket < 0 || length <= 0 || length + HEADER_SIZE > MAX_PACKET) return false;

    uint8_t pkt[MAX_PACKET];
    uint32_t seq = m_seqSend.fetch_add(1, std::memory_order_relaxed);
    std::memcpy(pkt, &seq, 4);
    std::memcpy(pkt + HEADER_SIZE, data, length);

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(m_config.remotePort);
    inet_pton(AF_INET, m_config.remoteHost.c_str(), &dest.sin_addr);

    int sent = sendto(m_socket, reinterpret_cast<const char*>(pkt), length + HEADER_SIZE, 0,
                      reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
    if (sent > 0) { m_sent.fetch_add(1, std::memory_order_relaxed); return true; }
    return false;
}

void UdpTransport::setReceiveCallback(PacketCallback cb) { m_callback = std::move(cb); }

void UdpTransport::receiveLoop() {
    uint8_t buf[MAX_PACKET];
    sockaddr_in sender{};
    socklen_t slen = sizeof(sender);

    while (m_running) {
#ifdef _WIN32
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(m_socket, &rset);
        timeval tv{0, 200};  // 0.2ms poll — tighter than original
        if (select(0, &rset, nullptr, nullptr, &tv) <= 0) continue;
#else
        pollfd pfd{m_socket, POLLIN, 0};
        if (poll(&pfd, 1, 0) <= 0) continue;  // non-blocking poll
#endif

        int n = recvfrom(m_socket, reinterpret_cast<char*>(buf), MAX_PACKET, 0,
                         reinterpret_cast<sockaddr*>(&sender), &slen);
        if (n <= HEADER_SIZE) continue;

        uint32_t seq;
        std::memcpy(&seq, buf, 4);

        uint32_t last = m_lastSeqRecv.load(std::memory_order_relaxed);
        if (seq > last + 1 && last > 0) {
            m_lost.fetch_add(seq - last - 1, std::memory_order_relaxed);
        }
        m_lastSeqRecv.store(seq, std::memory_order_relaxed);
        m_received.fetch_add(1, std::memory_order_relaxed);

        if (m_callback) m_callback(buf + HEADER_SIZE, n - HEADER_SIZE, seq);
    }
}

UdpTransport::Stats UdpTransport::getStats() const {
    return { m_sent.load(), m_received.load(), m_lost.load() };
}
