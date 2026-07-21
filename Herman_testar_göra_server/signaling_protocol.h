#pragma once
// =============================================================================
// MusiConnect Signaling Protocol — shared definitions
//
// Wire format is documented in PROTOCOL.md. This header provides:
//   - cross-platform socket glue (matches audio_transceiver.cpp conventions)
//   - message type constants
//   - tiny serialization helpers
//
// The signaling server NEVER carries audio. It only introduces two peers so
// they can hole-punch and stream directly.
// =============================================================================

#include <cstdint>
#include <cstring>
#include <string>

// -----------------------------------------------------------------------------
// Platform: sockets (same pattern as audio_transceiver.cpp)
// -----------------------------------------------------------------------------
#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef int socklen_t;
  typedef SOCKET socket_t;
  #define INVALID_SOCK INVALID_SOCKET
  #define CLOSE_SOCKET closesocket
  inline void platform_socket_init() {
      WSADATA wsa;
      WSAStartup(MAKEWORD(2, 2), &wsa);
  }
  inline void platform_socket_cleanup() { WSACleanup(); }
  inline void set_nonblocking(socket_t s) {
      u_long mode = 1;
      ioctlsocket(s, FIONBIO, &mode);
  }
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  typedef int socket_t;
  #define INVALID_SOCK (-1)
  #define CLOSE_SOCKET close
  inline void platform_socket_init() {}
  inline void platform_socket_cleanup() {}
  inline void set_nonblocking(socket_t s) {
      int flags = fcntl(s, F_GETFL, 0);
      fcntl(s, F_SETFL, flags | O_NONBLOCK);
  }
#endif

// -----------------------------------------------------------------------------
// Protocol constants
// -----------------------------------------------------------------------------
namespace sig {

constexpr uint32_t PROTO_MAGIC   = 0x4D4A414D; // 'M','J','A','M'
constexpr uint8_t  PROTO_VERSION = 1;
constexpr int      HEADER_SIZE   = 8;          // magic(4) ver(1) type(1) reserved(2)
constexpr int      ROOM_CODE_MAX = 32;
constexpr int      MAX_MSG       = 256;

// Message types
enum MsgType : uint8_t {
    MSG_REGISTER   = 1,   // client -> server
    MSG_REGISTERED = 2,   // server -> client
    MSG_PEER       = 3,   // server -> client
    MSG_PING       = 4,   // client -> server (keepalive)
    MSG_BYE        = 5,   // client -> server
    MSG_ERROR      = 6,   // server -> client
    MSG_PUNCH      = 10,  // client -> client
    MSG_PUNCH_ACK  = 11,  // client -> client
};

// Error codes (in MSG_ERROR payload)
enum ErrCode : uint8_t {
    ERR_BAD_REQUEST   = 1,
    ERR_ROOM_FULL     = 2,
    ERR_RATE_LIMITED  = 3,
    ERR_VERSION       = 4,
};

// Registration status (in MSG_REGISTERED payload)
enum RegStatus : uint8_t {
    REG_OK_WAITING = 0,
    REG_ROOM_FULL  = 1,
};

// -----------------------------------------------------------------------------
// Serialization helpers. Cursor-based; caller is responsible for buffer size.
// Endpoint fields (ip/port) are written in network byte order by the caller
// (i.e. copy the raw sockaddr_in fields). Other scalars are raw host-order,
// matching audio_transceiver.cpp's memcpy convention.
// -----------------------------------------------------------------------------
struct Writer {
    uint8_t* p;
    uint8_t* end;
    Writer(uint8_t* buf, size_t cap) : p(buf), end(buf + cap) {}

    bool put_bytes(const void* src, size_t n) {
        if (p + n > end) return false;
        std::memcpy(p, src, n);
        p += n;
        return true;
    }
    bool put_u8(uint8_t v)   { return put_bytes(&v, 1); }
    bool put_u16(uint16_t v) { return put_bytes(&v, 2); } // caller supplies net order for ports
    bool put_u32(uint32_t v) { return put_bytes(&v, 4); } // caller supplies net order for ips
    size_t size(uint8_t* start) const { return static_cast<size_t>(p - start); }
};

struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    Reader(const uint8_t* buf, size_t len) : p(buf), end(buf + len) {}

    bool get_bytes(void* dst, size_t n) {
        if (p + n > end) return false;
        std::memcpy(dst, p, n);
        p += n;
        return true;
    }
    bool get_u8(uint8_t& v)   { return get_bytes(&v, 1); }
    bool get_u16(uint16_t& v) { return get_bytes(&v, 2); }
    bool get_u32(uint32_t& v) { return get_bytes(&v, 4); }
    size_t remaining() const  { return static_cast<size_t>(end - p); }
};

// Write the common 8-byte header. Returns bytes written (== HEADER_SIZE) or 0.
inline size_t write_header(uint8_t* buf, size_t cap, uint8_t type) {
    Writer w(buf, cap);
    uint32_t magic = PROTO_MAGIC;
    uint16_t reserved = 0;
    if (!w.put_u32(magic))          return 0;
    if (!w.put_u8(PROTO_VERSION))   return 0;
    if (!w.put_u8(type))            return 0;
    if (!w.put_u16(reserved))       return 0;
    return HEADER_SIZE;
}

// Validate + parse the header. On success advances reader past header and sets
// out_type. Returns false if magic/version wrong or too short.
inline bool read_header(Reader& r, uint8_t& out_type) {
    uint32_t magic;
    uint8_t  ver, type;
    uint16_t reserved;
    if (!r.get_u32(magic))    return false;
    if (!r.get_u8(ver))       return false;
    if (!r.get_u8(type))      return false;
    if (!r.get_u16(reserved)) return false;
    if (magic != PROTO_MAGIC) return false;
    if (ver != PROTO_VERSION) return false;
    out_type = type;
    return true;
}

} // namespace sig
