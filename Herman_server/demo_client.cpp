// =============================================================================
// MusiConnect signaling demo client
//
// Shows the intended flow:
//   1. Ask the signaling server to find our peer in a shared room ("call sign").
//   2. Get back the peer's reachable UDP endpoint (after hole punch).
//   3. Hand that endpoint off to the audio transport.
//
// This demo does NOT start real audio (no PortAudio dependency here). It prints
// the resolved endpoint and shows exactly where you would launch
// audio_transceiver / UdpTransport on the SAME local port.
//
// Run two instances (on two machines, or two ports on one machine) with the
// same room code:
//   demo_client <server_ip> <room_code> [local_port] [server_port]
//
// Example:
//   ./demo_client 203.0.113.10 jam-4f9a2c 12345 5000
// =============================================================================

#include "signaling_client.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static const char* result_str(ResolveResult r) {
    switch (r) {
        case ResolveResult::Success:           return "Success";
        case ResolveResult::ServerUnreachable: return "ServerUnreachable";
        case ResolveResult::RoomFull:          return "RoomFull";
        case ResolveResult::Timeout:           return "Timeout";
        case ResolveResult::SocketError:       return "SocketError";
        case ResolveResult::VersionMismatch:   return "VersionMismatch";
    }
    return "Unknown";
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr,
            "Usage: %s <server_ip> <room_code> [local_port=12345] [server_port=5000]\n",
            argv[0]);
        return 2;
    }

    std::string server_ip   = argv[1];
    std::string room_code   = argv[2];
    uint16_t    local_port  = (argc >= 4) ? static_cast<uint16_t>(atoi(argv[3])) : 12345;
    uint16_t    server_port = (argc >= 5) ? static_cast<uint16_t>(atoi(argv[4])) : 5000;

    printf("============================================================\n");
    printf("  MusiConnect signaling demo client\n");
    printf("============================================================\n");
    printf("  Server : %s:%u\n", server_ip.c_str(), server_port);
    printf("  Room   : \"%s\"\n", room_code.c_str());
    printf("  Local  : UDP port %u (will also be the audio port)\n", local_port);
    printf("  Waiting for peer... (Ctrl+C to abort)\n");
    printf("============================================================\n");

    SignalingClient client(local_port);
    ResolvedPeer peer;
    ResolveResult res = client.resolve(server_ip, server_port, room_code, peer);

    if (res != ResolveResult::Success) {
        fprintf(stderr, "\n[demo] resolve failed: %s\n", result_str(res));
        return 1;
    }

    printf("\n============================================================\n");
    printf("  PEER RESOLVED\n");
    printf("------------------------------------------------------------\n");
    printf("  Remote endpoint : %s:%u\n", peer.ip.c_str(), peer.port);
    printf("  Hole punch      : %s\n",
           peer.confirmed ? "CONFIRMED (bidirectional)"
                          : "best-effort (no ACK; may be unreliable)");
    printf("------------------------------------------------------------\n");
    printf("  HANDOFF: now start the audio transport, e.g.\n");
    printf("    NetworkConfig cfg;\n");
    printf("    cfg.localPort  = %u;   // same port used for signaling\n", local_port);
    printf("    cfg.remoteHost = \"%s\";\n", peer.ip.c_str());
    printf("    cfg.remotePort = %u;\n", peer.port);
    printf("    UdpTransport t; t.init(cfg); t.start();\n");
    printf("  (or feed %s:%u as the remote IP/port into audio_transceiver)\n",
           peer.ip.c_str(), peer.port);
    printf("============================================================\n");

    return 0;
}
