
  // =============================================================================
  // UDP NETWORK TRANSPORT IMPLEMENTATION
  //
  // Cross-platform (Windows Winsock / POSIX sockets).
  // Optimized for minimum latency:
  //   - No Nagle algorithm (irrelevant for UDP but SO_SNDBUF minimized)
  //   - Non-blocking receive with tight poll loop
  //   - Minimal packet header (4 bytes sequence number)
  // =============================================================================

  #include "network.h"
  #include <iostream>
  #include <cstring>

  // Platform-specific socket includes
  #ifdef _WIN32
      #define WIN32_LEAN_AND_MEAN
      #include <winsock2.h>
      #include <ws2tcpip.h>
      #pragma comment(lib, "ws2_32.lib")
      using socklen_t = int;
      #define SOCKET_TYPE SOCKET
      #define INVALID_SOCK INVALID_SOCKET
      #define CLOSE_SOCKET closesocket
      #define SOCK_ERROR SOCKET_ERROR
  #else
      #include <sys/socket.h>
      #include <netinet/in.h>
      #include <arpa/inet.h>
      #include <unistd.h>
      #include <fcntl.h>
      #include <poll.h>
      #define SOCKET_TYPE int
      #define INVALID_SOCK -1
      #define CLOSE_SOCKET close
      #define SOCK_ERROR -1
  #endif

  // Packet header: just a sequence number (4 bytes)
  static constexpr int HEADER_SIZE = 4;
  static constexpr int MAX_PACKET_SIZE = 512;  // More than enough for one CELT frame

  UdpTransport::UdpTransport() = default;

  UdpTransport::~UdpTransport() {
      stop();
  }

  bool UdpTransport::init(const NetworkConfig& config) {
      m_config = config;

  #ifdef _WIN32
      // Initialize Winsock
      WSADATA wsaData;
      if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
          std::cerr << "[NET] WSAStartup failed" << std::endl;
          return false;
      }
  #endif

      // Create UDP socket
      m_socket = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
      if (m_socket == (int)INVALID_SOCK) {
          std::cerr << "[NET] Failed to create socket" << std::endl;
          return false;
      }

      // Bind to local port
      sockaddr_in localAddr{};
      localAddr.sin_family = AF_INET;
      localAddr.sin_port = htons(config.localPort);
      localAddr.sin_addr.s_addr = INADDR_ANY;

      if (bind(m_socket, reinterpret_cast<sockaddr*>(&localAddr), sizeof(localAddr)) == SOCK_ERROR) {
          std::cerr << "[NET] Failed to bind to port " << config.localPort << std::endl;
          CLOSE_SOCKET(m_socket);
          m_socket = -1;
          return false;
      }

      // Set socket to non-blocking
  #ifdef _WIN32
      u_long nonBlocking = 1;
      ioctlsocket(m_socket, FIONBIO, &nonBlocking);
  #else
      int flags = fcntl(m_socket, F_GETFL, 0);
      fcntl(m_socket, F_SETFL, flags | O_NONBLOCK);
  #endif

      // Reduce socket send buffer for lower latency
      int bufSize = 4096;  // Small send buffer — we don't want queuing
      setsockopt(m_socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char*>(&bufSize), sizeof(bufSize));
      setsockopt(m_socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&bufSize), sizeof(bufSize));

      // DSCP EF (Expedited Forwarding) — marks packets as real-time traffic
      // 5G networks with QoS support will prioritize these packets
      int dscp = 46 << 2;  // EF = 46, shift left 2 for TOS field
      setsockopt(m_socket, IPPROTO_IP, IP_TOS, reinterpret_cast<char*>(&dscp), sizeof(dscp));

      std::cout << "[NET] Bound to port " << config.localPort
                << ", remote: " << config.remoteHost << ":" << config.remotePort << std::endl;

      return true;
  }

  bool UdpTransport::start() {
      m_running = true;
      m_receiveThread = std::thread(&UdpTransport::receiveLoop, this);

      // Pin receive thread to CPU core 1 to avoid context-switch latency
  #ifdef _WIN32
      SetThreadAffinityMask(m_receiveThread.native_handle(), 1ULL << 1);
      SetThreadPriority(m_receiveThread.native_handle(), THREAD_PRIORITY_TIME_CRITICAL);
  #else
      cpu_set_t cpuset;
      CPU_ZERO(&cpuset);
      CPU_SET(1, &cpuset);
      pthread_setaffinity_np(m_receiveThread.native_handle(), sizeof(cpuset), &cpuset);
  #endif

      std::cout << "[NET] Receive thread started (pinned to core 1, realtime priority)" << std::endl;
      return true;
  }

  void UdpTransport::stop() {
      m_running = false;
      if (m_receiveThread.joinable()) {
          m_receiveThread.join();
      }
      if (m_socket != -1) {
          CLOSE_SOCKET(m_socket);
          m_socket = -1;
      }
  #ifdef _WIN32
      WSACleanup();
  #endif
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

      int sent = sendto(m_socket, reinterpret_cast<const char*>(packet), length + HEADER_SIZE, 0,
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
      uint8_t buffer[MAX_PACKET_SIZE];
      sockaddr_in senderAddr{};
      socklen_t senderLen = sizeof(senderAddr);

      while (m_running) {
          // Poll with short timeout to allow clean shutdown
  #ifdef _WIN32
          fd_set readSet;
          FD_ZERO(&readSet);
          FD_SET(m_socket, &readSet);
          timeval timeout{0, 500};  // 0.5ms timeout — tight loop for low latency
          int ready = select(0, &readSet, nullptr, nullptr, &timeout);
  #else
          struct pollfd pfd{};
          pfd.fd = m_socket;
          pfd.events = POLLIN;
          int ready = poll(&pfd, 1, 1);  // 1ms timeout
  #endif

          if (ready <= 0) continue;

          int received = recvfrom(m_socket, reinterpret_cast<char*>(buffer), MAX_PACKET_SIZE, 0,
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
                  m_callback(buffer + HEADER_SIZE, received - HEADER_SIZE, seq);
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
