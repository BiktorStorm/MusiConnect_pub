#!/usr/bin/env python3
# =============================================================================
# Multi-peer signaling verification client (protocol v2)
#
# Registers several fake peers in the same room against a running
# signaling_server_multipeer.py and prints the mesh (peer endpoints + sender
# IDs) each one is told about. Purely a signaling test -- it sends no audio.
#
# Usage:
#   python multipeer_test_client.py [server_ip=127.0.0.1] [port=5000] \
#          [room=jam-test] [num_peers=3]
# =============================================================================

import socket
import struct
import sys
import time

PROTO_MAGIC = 0x4D4A414D
PROTO_VERSION = 2
HEADER_SIZE = 8

MSG_REGISTER = 1
MSG_REGISTERED = 2
MSG_PEER = 3
MSG_BYE = 5
MSG_ERROR = 6

REG_OK_WAITING = 0
REG_ROOM_FULL = 1


def header(msg_type):
    return struct.pack("<IBBH", PROTO_MAGIC, PROTO_VERSION, msg_type, 0)


def build_register(room, lan_ip, lan_port, sender_id):
    rb = room.encode("utf-8")
    return (header(MSG_REGISTER)
            + struct.pack("<B", len(rb)) + rb
            + socket.inet_aton(lan_ip) + struct.pack("!H", lan_port)
            + struct.pack("<B", sender_id))


def parse(data):
    if len(data) < HEADER_SIZE:
        return None, None
    magic, ver, mtype, _ = struct.unpack_from("<IBBH", data, 0)
    if magic != PROTO_MAGIC or ver != PROTO_VERSION:
        return None, None
    return mtype, data[HEADER_SIZE:]


def main(argv):
    server_ip = argv[1] if len(argv) > 1 else "127.0.0.1"
    port = int(argv[2]) if len(argv) > 2 else 5000
    room = argv[3] if len(argv) > 3 else "jam-test"
    num_peers = int(argv[4]) if len(argv) > 4 else 3
    server = (server_ip, port)

    peers = []
    for i in range(num_peers):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("0.0.0.0", 0))
        s.settimeout(0.3)
        peers.append({"sock": s, "id": i, "mesh": {}})

    # Register each peer; stagger slightly so introductions are incremental.
    for p in peers:
        lan_port = p["sock"].getsockname()[1]
        p["sock"].sendto(
            build_register(room, "192.168.1.%d" % (10 + p["id"]), lan_port, p["id"]),
            server)
        time.sleep(0.15)

    # Drain messages for a short window so late PEER messages arrive.
    deadline = time.time() + 2.0
    while time.time() < deadline:
        progressed = False
        for p in peers:
            try:
                data, _ = p["sock"].recvfrom(256)
            except socket.timeout:
                continue
            progressed = True
            mtype, payload = parse(data)
            if mtype == MSG_REGISTERED:
                ip = socket.inet_ntoa(payload[0:4])
                pport = struct.unpack("!H", payload[4:6])[0]
                status = payload[6]
                tag = "OK" if status == REG_OK_WAITING else "ROOM_FULL"
                print("id=%d  REGISTERED public=%s:%d status=%s"
                      % (p["id"], ip, pport, tag))
            elif mtype == MSG_PEER:
                pip = socket.inet_ntoa(payload[0:4])
                pport = struct.unpack("!H", payload[4:6])[0]
                lip = socket.inet_ntoa(payload[6:10])
                lport = struct.unpack("!H", payload[10:12])[0]
                pid = payload[12]
                p["mesh"][pid] = (pip, pport, lip, lport)
            elif mtype == MSG_ERROR:
                print("id=%d  ERROR code=%d" % (p["id"], payload[0]))
        if not progressed:
            time.sleep(0.05)

    print("\n--- Mesh view per peer ---")
    ok = True
    for p in peers:
        ids = sorted(p["mesh"].keys())
        print("id=%d knows peers: %s" % (p["id"], ids))
        for pid, (pip, pport, lip, lport) in sorted(p["mesh"].items()):
            print("    -> id=%d public=%s:%d lan=%s:%d" % (pid, pip, pport, lip, lport))
        expected = [i for i in range(num_peers) if i != p["id"]]
        if ids != expected:
            ok = False
            print("    !! expected %s" % expected)

    # Clean up: say BYE so the server drops us immediately.
    for p in peers:
        try:
            p["sock"].sendto(header(MSG_BYE), server)
        except OSError:
            pass
        p["sock"].close()

    print("\nRESULT: %s" % ("full mesh formed for all peers"
                            if ok else "MESH INCOMPLETE (see !! above)"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
