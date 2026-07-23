#pragma once
// =============================================================================
// MusiConnect Signaling Client + UDP hole punch
//
// Usage model:
//   1. Construct SignalingClient with the audio LOCAL port (the same port the
//      audio transport will use). The client binds a UDP socket to that port
//      with SO_REUSEADDR so the exact NAT mapping used for audio is the one the
//      server observes and the one hole punching opens.
//   2. Call resolve(server, room) -> blocks until a peer is found and hole
//      punching completes (or times out).
//   3. On success you get a ResolvedPeer{ip, port}. Feed that into your
//      NetworkConfig.remoteHost / remotePort (or audio_transceiver's remote
//      address) and start the audio stream on the SAME local port.
//
// The signaling socket is closed by resolve() before returning so the audio
// transport can bind the local port. Because both bind with SO_REUSEADDR and
// the handoff is immediate, the NAT mapping persists (mappings typically live
// 30s+). For the tightest guarantee you can instead hand off the live socket
// via take_socket() and skip re-binding.
// =============================================================================

#include "signaling_protocol.h"
#include <string>
#include <cstdint>

struct ResolvedPeer {
    std::string ip;          // dotted-quad of the confirmed remote endpoint
    uint16_t    port = 0;    // host order
    bool        confirmed = false; // true = hole punch ACKed; false = best-effort
};

enum class ResolveResult {
    Success,          // peer resolved (see ResolvedPeer.confirmed for quality)
    ServerUnreachable,
    RoomFull,
    Timeout,          // no peer joined / no punch within budget
    SocketError,
    VersionMismatch,
};

class SignalingClient {
public:
    // local_port: the UDP port the audio transport will use (e.g. 12345 / 4464).
    explicit SignalingClient(uint16_t local_port);
    ~SignalingClient();

    // Blocking. Registers in `room` on the signaling server at server_ip:server_port,
    // waits for a peer, performs hole punch, and fills `out` on success.
    //   wait_peer_ms:  how long to wait for the other peer to appear.
    //   punch_ms:      hole-punch attempt budget once the peer is known.
    ResolveResult resolve(const std::string& server_ip,
                          uint16_t server_port,
                          const std::string& room,
                          ResolvedPeer& out,
                          int wait_peer_ms = 60000,
                          int punch_ms     = 3000);

    // Optional: hand the live, hole-punched socket to the caller instead of
    // closing it. After this returns a valid handle, the caller owns the socket
    // and this object will not close it. Returns INVALID_SOCK if unavailable.
    socket_t take_socket();

    uint16_t local_port() const { return local_port_; }

private:
    bool open_socket();
    void close_socket();

    // send helpers on the bound socket
    bool send_register(const sockaddr_in& server, const std::string& room);
    void send_ping(const sockaddr_in& server);
    void send_bye(const sockaddr_in& server);

    uint16_t  local_port_;
    socket_t  sock_       = INVALID_SOCK;
    bool      owns_socket_ = false;
};
