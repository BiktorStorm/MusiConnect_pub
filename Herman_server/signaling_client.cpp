// =============================================================================
// MusiConnect Signaling Client + UDP hole punch — implementation
// See signaling_client.h and PROTOCOL.md.
// =============================================================================

#include "signaling_client.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using namespace sig;

static double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

static std::string ip_to_string(uint32_t net_ip) {
    struct in_addr a;
    a.s_addr = net_ip;
    char buf[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &a, buf, sizeof(buf));
    return std::string(buf);
}

// Best-effort primary LAN IPv4 (net order). Returns 0 if it can't be determined.
// Trick: "connect" a UDP socket to a public address (no packets sent) and read
// back the local address the OS picked.
static uint32_t guess_local_ip() {
    socket_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCK) return 0;
    sockaddr_in probe{};
    probe.sin_family = AF_INET;
    probe.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &probe.sin_addr);
    uint32_t result = 0;
    if (connect(s, reinterpret_cast<sockaddr*>(&probe), sizeof(probe)) == 0) {
        sockaddr_in local{};
        socklen_t len = sizeof(local);
        if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0)
            result = local.sin_addr.s_addr;
    }
    CLOSE_SOCKET(s);
    return result;
}

SignalingClient::SignalingClient(uint16_t local_port)
    : local_port_(local_port) {
    platform_socket_init();
}

SignalingClient::~SignalingClient() {
    close_socket();
    platform_socket_cleanup();
}

bool SignalingClient::open_socket() {
    sock_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_ == INVALID_SOCK) return false;

    int yes = 1;
    setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(local_port_);
    if (bind(sock_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        CLOSE_SOCKET(sock_);
        sock_ = INVALID_SOCK;
        return false;
    }
    set_nonblocking(sock_);
    owns_socket_ = true;
    return true;
}

void SignalingClient::close_socket() {
    if (sock_ != INVALID_SOCK && owns_socket_) {
        CLOSE_SOCKET(sock_);
    }
    sock_ = INVALID_SOCK;
    owns_socket_ = false;
}

socket_t SignalingClient::take_socket() {
    if (sock_ == INVALID_SOCK || !owns_socket_) return INVALID_SOCK;
    socket_t s = sock_;
    owns_socket_ = false; // caller owns it now; destructor won't close it
    return s;
}

bool SignalingClient::send_register(const sockaddr_in& server, const std::string& room) {
    uint8_t buf[MAX_MSG];
    size_t n = write_header(buf, sizeof(buf), MSG_REGISTER);
    if (!n) return false;
    Writer w(buf + n, sizeof(buf) - n);
    uint8_t room_len = static_cast<uint8_t>(room.size());
    if (!w.put_u8(room_len)) return false;
    if (!w.put_bytes(room.data(), room_len)) return false;
    w.put_u32(guess_local_ip());           // network order
    w.put_u16(htons(local_port_));          // network order
    size_t total = n + w.size(buf + n);
    return sendto(sock_, reinterpret_cast<const char*>(buf),
                  static_cast<int>(total), 0,
                  reinterpret_cast<const sockaddr*>(&server), sizeof(server)) > 0;
}

void SignalingClient::send_ping(const sockaddr_in& server) {
    uint8_t buf[MAX_MSG];
    size_t n = write_header(buf, sizeof(buf), MSG_PING);
    if (!n) return;
    sendto(sock_, reinterpret_cast<const char*>(buf), static_cast<int>(n), 0,
           reinterpret_cast<const sockaddr*>(&server), sizeof(server));
}

void SignalingClient::send_bye(const sockaddr_in& server) {
    uint8_t buf[MAX_MSG];
    size_t n = write_header(buf, sizeof(buf), MSG_BYE);
    if (!n) return;
    sendto(sock_, reinterpret_cast<const char*>(buf), static_cast<int>(n), 0,
           reinterpret_cast<const sockaddr*>(&server), sizeof(server));
}

// Small helper: send a header-only message to an arbitrary endpoint.
static void send_bare(socket_t sock, const sockaddr_in& to, uint8_t type) {
    uint8_t buf[MAX_MSG];
    size_t n = write_header(buf, sizeof(buf), type);
    if (!n) return;
    sendto(sock, reinterpret_cast<const char*>(buf), static_cast<int>(n), 0,
           reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

ResolveResult SignalingClient::resolve(const std::string& server_ip,
                                       uint16_t server_port,
                                       const std::string& room,
                                       ResolvedPeer& out,
                                       int wait_peer_ms,
                                       int punch_ms) {
    if (room.empty() || room.size() > ROOM_CODE_MAX) return ResolveResult::SocketError;
    if (!open_socket()) return ResolveResult::SocketError;

    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(server_port);
    if (inet_pton(AF_INET, server_ip.c_str(), &server.sin_addr) != 1) {
        close_socket();
        return ResolveResult::SocketError;
    }

    // --- Phase 1: register and wait for PEER ---
    if (!send_register(server, room)) {
        close_socket();
        return ResolveResult::ServerUnreachable;
    }

    // Candidate endpoints from PEER (public + local)
    sockaddr_in cand_public{}, cand_local{};
    bool have_public = false, have_local = false;
    bool got_peer = false;
    bool got_registered = false;

    double start = now_ms();
    double last_reg = start;   // periodic re-register (acts as keepalive too)
    double last_ping = start;

    while (now_ms() - start < wait_peer_ms) {
        uint8_t buf[MAX_MSG];
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock_, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                         reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n >= HEADER_SIZE) {
            Reader r(buf, static_cast<size_t>(n));
            uint8_t type;
            if (read_header(r, type)) {
                if (type == MSG_REGISTERED) {
                    uint32_t pub_ip; uint16_t pub_port; uint8_t status;
                    if (r.get_u32(pub_ip) && r.get_u16(pub_port) && r.get_u8(status)) {
                        got_registered = true;
                        if (status == REG_ROOM_FULL) {
                            send_bye(server);
                            close_socket();
                            return ResolveResult::RoomFull;
                        }
                        printf("[client] public endpoint seen by server: %s:%u\n",
                               ip_to_string(pub_ip).c_str(), ntohs(pub_port));
                    }
                } else if (type == MSG_ERROR) {
                    uint8_t code;
                    if (r.get_u8(code)) {
                        if (code == ERR_ROOM_FULL) { close_socket(); return ResolveResult::RoomFull; }
                        if (code == ERR_VERSION)   { close_socket(); return ResolveResult::VersionMismatch; }
                        // rate limited / bad request: keep trying within budget
                    }
                } else if (type == MSG_PEER) {
                    uint32_t p_ip, l_ip; uint16_t p_port, l_port;
                    if (r.get_u32(p_ip) && r.get_u16(p_port) &&
                        r.get_u32(l_ip) && r.get_u16(l_port)) {
                        cand_public.sin_family = AF_INET;
                        cand_public.sin_addr.s_addr = p_ip;
                        cand_public.sin_port = p_port;
                        have_public = (p_ip != 0 && p_port != 0);
                        if (l_ip != 0 && l_port != 0) {
                            cand_local.sin_family = AF_INET;
                            cand_local.sin_addr.s_addr = l_ip;
                            cand_local.sin_port = l_port;
                            have_local = true;
                        }
                        got_peer = true;
                        printf("[client] peer: public %s:%u  lan %s:%u\n",
                               ip_to_string(p_ip).c_str(), ntohs(p_port),
                               ip_to_string(l_ip).c_str(), ntohs(l_port));
                        break; // move to hole punch
                    }
                }
            }
        }

        double t = now_ms();
        // Re-send REGISTER every 2s until acknowledged (handles server-side loss).
        if (!got_registered && t - last_reg > 2000) {
            send_register(server, room);
            last_reg = t;
        }
        // Keepalive ping every 5s once registered, to hold the NAT mapping open.
        if (got_registered && t - last_ping > 5000) {
            send_ping(server);
            last_ping = t;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (!got_peer) {
        send_bye(server);
        close_socket();
        return got_registered ? ResolveResult::Timeout : ResolveResult::ServerUnreachable;
    }

    // --- Phase 2: hole punch directly to the peer ---
    // We're done with the server for setup; tell it we're leaving the room so
    // the slot frees up (audio no longer needs signaling).
    send_bye(server);

    std::vector<sockaddr_in> cands;
    if (have_local)  cands.push_back(cand_local);   // try LAN first (same network)
    if (have_public) cands.push_back(cand_public);

    sockaddr_in confirmed{};
    bool is_confirmed = false;

    double p_start = now_ms();
    double last_send = 0;
    while (now_ms() - p_start < punch_ms && !is_confirmed) {
        double t = now_ms();
        if (t - last_send > 50) {           // fire PUNCH at all candidates every 50ms
            for (auto& c : cands) send_bare(sock_, c, MSG_PUNCH);
            last_send = t;
        }

        uint8_t buf[MAX_MSG];
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock_, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                         reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n >= HEADER_SIZE) {
            Reader r(buf, static_cast<size_t>(n));
            uint8_t type;
            if (read_header(r, type)) {
                if (type == MSG_PUNCH) {
                    // Peer is probing us — ACK back to the source we actually saw.
                    send_bare(sock_, from, MSG_PUNCH_ACK);
                } else if (type == MSG_PUNCH_ACK) {
                    // The source of an ACK is a confirmed working path.
                    confirmed = from;
                    is_confirmed = true;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // --- Result ---
    if (is_confirmed) {
        out.ip = ip_to_string(confirmed.sin_addr.s_addr);
        out.port = ntohs(confirmed.sin_port);
        out.confirmed = true;
    } else if (have_public) {
        // Best effort: many cone NATs still work without seeing an explicit ACK.
        out.ip = ip_to_string(cand_public.sin_addr.s_addr);
        out.port = ntohs(cand_public.sin_port);
        out.confirmed = false;
    } else if (have_local) {
        out.ip = ip_to_string(cand_local.sin_addr.s_addr);
        out.port = ntohs(cand_local.sin_port);
        out.confirmed = false;
    } else {
        close_socket();
        return ResolveResult::Timeout;
    }

    // By default we close the signaling socket so the audio transport can bind
    // the same local port (with SO_REUSEADDR). If you prefer zero-gap handoff,
    // call take_socket() BEFORE resolve() returns is not possible; instead do
    // not close here and expose the socket. For the stub we close.
    close_socket();
    return ResolveResult::Success;
}
