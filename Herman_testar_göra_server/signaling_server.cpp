// =============================================================================
// MusiConnect Signaling / Rendezvous Server
//
// A single-threaded UDP server that introduces two peers so they can hole-punch
// and stream audio directly. It NEVER carries audio.
//
// Responsibilities (see PROTOCOL.md):
//   - REGISTER:  add sender to a room (max 2 members); reflect its public
//                endpoint back (STUN-style) in REGISTERED.
//   - When a room has 2 members: send each the other's endpoints (PEER).
//   - PING:      keepalive; refresh last-seen.
//   - BYE:       remove sender from its room.
//   - Timeouts:  drop members not seen for MEMBER_TIMEOUT.
//   - Basic abuse limits: max rooms, per-source REGISTER rate limit.
//
// Build (see CMakeLists.txt / README.md), e.g.:
//   g++ -std=c++17 signaling_server.cpp -o signaling_server         (POSIX)
//   cl /std:c++17 signaling_server.cpp ws2_32.lib                   (MSVC)
// =============================================================================

#include "signaling_protocol.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using namespace sig;

// -----------------------------------------------------------------------------
// Tunables
// -----------------------------------------------------------------------------
static const uint16_t DEFAULT_PORT      = 5000;
static const int      MAX_ROOMS         = 1000;   // total simultaneous rooms
static const int      MEMBER_TIMEOUT_S  = 30;     // drop stale members
static const int      REG_RATE_WINDOW_S = 1;      // rate-limit window
static const int      REG_RATE_MAX      = 20;     // max REGISTERs / window / IP

// -----------------------------------------------------------------------------
// Clean shutdown
// -----------------------------------------------------------------------------
static std::atomic<bool> g_running{true};
static void signal_handler(int /*sig*/) { g_running.store(false); }

static double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// -----------------------------------------------------------------------------
// Server state
// -----------------------------------------------------------------------------
struct Member {
    uint32_t public_ip   = 0;   // network order (as observed by server)
    uint16_t public_port = 0;   // network order
    uint32_t local_ip    = 0;   // network order (client-reported LAN ip), 0 = unknown
    uint16_t local_port  = 0;   // network order
    double   last_seen   = 0.0;
    bool     used        = false;
};

struct Room {
    Member members[2];
    bool   peer_sent = false;   // whether PEER has been dispatched to both
};

static std::unordered_map<std::string, Room> g_rooms;

// crude per-source-IP rate limiter for REGISTER
struct RateEntry { double window_start = 0.0; int count = 0; };
static std::unordered_map<uint32_t, RateEntry> g_rate; // key = public_ip (net order)

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
static std::string ip_to_string(uint32_t net_ip) {
    struct in_addr a;
    a.s_addr = net_ip;
    char buf[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &a, buf, sizeof(buf));
    return std::string(buf);
}

static void make_addr(sockaddr_in& out, uint32_t net_ip, uint16_t net_port) {
    std::memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    out.sin_addr.s_addr = net_ip;
    out.sin_port = net_port;
}

static void send_error(socket_t sock, const sockaddr_in& to, uint8_t code) {
    uint8_t buf[MAX_MSG];
    size_t n = write_header(buf, sizeof(buf), MSG_ERROR);
    if (!n) return;
    Writer w(buf + n, sizeof(buf) - n);
    w.put_u8(code);
    sendto(sock, reinterpret_cast<const char*>(buf), static_cast<int>(n + 1), 0,
           reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

static void send_registered(socket_t sock, const sockaddr_in& to,
                            uint32_t pub_ip, uint16_t pub_port, uint8_t status) {
    uint8_t buf[MAX_MSG];
    size_t n = write_header(buf, sizeof(buf), MSG_REGISTERED);
    if (!n) return;
    Writer w(buf + n, sizeof(buf) - n);
    w.put_u32(pub_ip);      // network order
    w.put_u16(pub_port);    // network order
    w.put_u8(status);
    sendto(sock, reinterpret_cast<const char*>(buf),
           static_cast<int>(n + 7), 0,
           reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

static void send_peer(socket_t sock, const sockaddr_in& to, const Member& peer) {
    uint8_t buf[MAX_MSG];
    size_t n = write_header(buf, sizeof(buf), MSG_PEER);
    if (!n) return;
    Writer w(buf + n, sizeof(buf) - n);
    w.put_u32(peer.public_ip);
    w.put_u16(peer.public_port);
    w.put_u32(peer.local_ip);
    w.put_u16(peer.local_port);
    sendto(sock, reinterpret_cast<const char*>(buf),
           static_cast<int>(n + 12), 0,
           reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

// Returns true if this source IP is allowed another REGISTER right now.
static bool rate_ok(uint32_t src_ip, double now) {
    RateEntry& e = g_rate[src_ip];
    if (now - e.window_start >= REG_RATE_WINDOW_S) {
        e.window_start = now;
        e.count = 0;
    }
    if (e.count >= REG_RATE_MAX) return false;
    e.count++;
    return true;
}

// Find the member slot for a given endpoint in a room, or -1.
static int find_member(const Room& room, uint32_t ip, uint16_t port) {
    for (int i = 0; i < 2; i++)
        if (room.members[i].used &&
            room.members[i].public_ip == ip &&
            room.members[i].public_port == port)
            return i;
    return -1;
}

static int free_slot(const Room& room) {
    for (int i = 0; i < 2; i++)
        if (!room.members[i].used) return i;
    return -1;
}

// If both members present and not yet introduced, send PEER to both.
static void maybe_introduce(socket_t sock, Room& room) {
    if (room.peer_sent) return;
    if (!room.members[0].used || !room.members[1].used) return;

    sockaddr_in a0, a1;
    make_addr(a0, room.members[0].public_ip, room.members[0].public_port);
    make_addr(a1, room.members[1].public_ip, room.members[1].public_port);

    send_peer(sock, a0, room.members[1]);
    send_peer(sock, a1, room.members[0]);
    room.peer_sent = true;

    printf("[room] introduced %s:%u <-> %s:%u\n",
           ip_to_string(room.members[0].public_ip).c_str(),
           ntohs(room.members[0].public_port),
           ip_to_string(room.members[1].public_ip).c_str(),
           ntohs(room.members[1].public_port));
}

// Drop members not seen recently; erase empty rooms.
static void reap_stale(double now) {
    for (auto it = g_rooms.begin(); it != g_rooms.end(); ) {
        Room& room = it->second;
        int alive = 0;
        for (int i = 0; i < 2; i++) {
            if (room.members[i].used) {
                if (now - room.members[i].last_seen > MEMBER_TIMEOUT_S) {
                    room.members[i] = Member{}; // reset
                    room.peer_sent = false;     // allow re-introduce if refilled
                } else {
                    alive++;
                }
            }
        }
        if (alive == 0) it = g_rooms.erase(it);
        else ++it;
    }
}

// -----------------------------------------------------------------------------
// Message handling
// -----------------------------------------------------------------------------
static void handle_register(socket_t sock, const sockaddr_in& from,
                            Reader& r, double now) {
    uint32_t src_ip   = from.sin_addr.s_addr;
    uint16_t src_port = from.sin_port;

    if (!rate_ok(src_ip, now)) {
        send_error(sock, from, ERR_RATE_LIMITED);
        return;
    }

    uint8_t room_len = 0;
    if (!r.get_u8(room_len) || room_len == 0 || room_len > ROOM_CODE_MAX) {
        send_error(sock, from, ERR_BAD_REQUEST);
        return;
    }
    char room_buf[ROOM_CODE_MAX + 1] = {0};
    if (!r.get_bytes(room_buf, room_len)) {
        send_error(sock, from, ERR_BAD_REQUEST);
        return;
    }
    uint32_t local_ip = 0;
    uint16_t local_port = 0;
    if (!r.get_u32(local_ip) || !r.get_u16(local_port)) {
        send_error(sock, from, ERR_BAD_REQUEST);
        return;
    }
    std::string room_code(room_buf, room_len);

    auto it = g_rooms.find(room_code);
    if (it == g_rooms.end()) {
        if (static_cast<int>(g_rooms.size()) >= MAX_ROOMS) {
            send_error(sock, from, ERR_RATE_LIMITED);
            return;
        }
        it = g_rooms.emplace(room_code, Room{}).first;
    }
    Room& room = it->second;

    // Already registered? refresh (re-send REGISTERED, idempotent).
    int slot = find_member(room, src_ip, src_port);
    if (slot < 0) {
        slot = free_slot(room);
        if (slot < 0) {
            // room full with two *other* peers
            send_registered(sock, from, src_ip, src_port, REG_ROOM_FULL);
            send_error(sock, from, ERR_ROOM_FULL);
            return;
        }
    }

    Member& m = room.members[slot];
    m.used        = true;
    m.public_ip   = src_ip;
    m.public_port = src_port;
    m.local_ip    = local_ip;
    m.local_port  = local_port;
    m.last_seen   = now;

    printf("[reg]  room=\"%s\" slot=%d  %s:%u (lan %s:%u)\n",
           room_code.c_str(), slot,
           ip_to_string(src_ip).c_str(), ntohs(src_port),
           ip_to_string(local_ip).c_str(), ntohs(local_port));

    send_registered(sock, from, src_ip, src_port, REG_OK_WAITING);
    maybe_introduce(sock, room);
}

static void handle_ping(const sockaddr_in& from, double now) {
    uint32_t ip = from.sin_addr.s_addr;
    uint16_t port = from.sin_port;
    for (auto& kv : g_rooms) {
        int slot = find_member(kv.second, ip, port);
        if (slot >= 0) {
            kv.second.members[slot].last_seen = now;
            return;
        }
    }
}

static void handle_bye(const sockaddr_in& from) {
    uint32_t ip = from.sin_addr.s_addr;
    uint16_t port = from.sin_port;
    for (auto& kv : g_rooms) {
        int slot = find_member(kv.second, ip, port);
        if (slot >= 0) {
            printf("[bye]  room=\"%s\" slot=%d\n", kv.first.c_str(), slot);
            kv.second.members[slot] = Member{};
            kv.second.peer_sent = false;
            return;
        }
    }
}

// -----------------------------------------------------------------------------
// main
// -----------------------------------------------------------------------------
int main(int argc, char** argv) {
    uint16_t port = DEFAULT_PORT;
    if (argc >= 2) port = static_cast<uint16_t>(atoi(argv[1]));

    platform_socket_init();
    std::signal(SIGINT, signal_handler);
#ifndef _WIN32
    std::signal(SIGTERM, signal_handler);
#endif

    socket_t sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCK) {
        fprintf(stderr, "Failed to create socket\n");
        platform_socket_cleanup();
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        fprintf(stderr, "Failed to bind on UDP port %u\n", port);
        CLOSE_SOCKET(sock);
        platform_socket_cleanup();
        return 1;
    }

    // Receive timeout so we can reap stale members and notice shutdown.
#ifdef _WIN32
    DWORD tv = 500;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 500000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif

    printf("============================================================\n");
    printf("  MusiConnect Signaling Server\n");
    printf("============================================================\n");
    printf("  Listening : UDP 0.0.0.0:%u\n", port);
    printf("  Max rooms : %d   member timeout: %ds\n", MAX_ROOMS, MEMBER_TIMEOUT_S);
    printf("  Ctrl+C to stop\n");
    printf("============================================================\n");

    double last_reap = now_seconds();

    while (g_running.load()) {
        uint8_t buf[MAX_MSG];
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                         reinterpret_cast<sockaddr*>(&from), &from_len);

        double now = now_seconds();
        if (now - last_reap >= 1.0) {
            reap_stale(now);
            last_reap = now;
        }

        if (n < HEADER_SIZE) continue; // timeout or runt

        Reader r(buf, static_cast<size_t>(n));
        uint8_t type;
        if (!read_header(r, type)) {
            // wrong magic/version
            continue;
        }

        switch (type) {
            case MSG_REGISTER: handle_register(sock, from, r, now); break;
            case MSG_PING:     handle_ping(from, now);              break;
            case MSG_BYE:      handle_bye(from);                    break;
            default:
                // servers ignore PUNCH/PUNCH_ACK/etc. addressed to them
                break;
        }
    }

    printf("\n[server] shutting down (%zu active rooms)\n", g_rooms.size());
    CLOSE_SOCKET(sock);
    platform_socket_cleanup();
    return 0;
}
