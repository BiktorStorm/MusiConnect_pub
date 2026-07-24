#!/usr/bin/env python3
# =============================================================================
# MusiConnect Signaling / Rendezvous Server  --  MULTI-PEER (v2)
#
# A single-threaded UDP server that introduces peers in a room so they can
# hole-punch and stream audio directly to each other in a full mesh. It NEVER
# carries audio.
#
# Differences vs. the 1:1 signaling_server_python.py:
#   - A room holds up to MAX_MEMBERS peers (not just 2).
#   - Members are keyed by a user-provided SENDER ID (uint8, 0-255) instead of
#     by public endpoint. This gives each peer a STABLE identity: if a peer's
#     NAT remaps its port mid-session, a re-REGISTER with the same ID just
#     updates its endpoint instead of creating a ghost member.
#   - Introduction is INCREMENTAL: when a peer joins (or its endpoint changes),
#     the server sends it a PEER for every existing member and sends every
#     existing member a PEER for it. A full mesh forms as peers arrive.
#   - PEER messages now carry the peer's sender ID, so a client can associate
#     an endpoint with an ID before any audio arrives.
#
# Wire format (PROTOCOL v2). The 8-byte header is unchanged except VERSION=2.
# Header is a raw little-endian layout; ip/port fields are network byte order.
#
#   REGISTER   (client->server) payload:
#       room_len : u8
#       room     : room_len bytes
#       local_ip : 4 bytes (network order, raw; client-reported LAN ip)
#       local_prt: 2 bytes (network order, raw)
#       sender_id: u8                                   <-- NEW in v2
#
#   REGISTERED (server->client) payload  (unchanged from v1):
#       pub_ip   : 4 bytes (network order)
#       pub_port : 2 bytes (network order)
#       status   : u8   (REG_OK_WAITING / REG_ROOM_FULL)
#
#   PEER       (server->client) payload:
#       pub_ip   : 4 bytes (network order)
#       pub_port : 2 bytes (network order)
#       local_ip : 4 bytes (network order, raw)
#       local_prt: 2 bytes (network order, raw)
#       sender_id: u8                                   <-- NEW in v2
#
#   PING / BYE (client->server): header only; matched by source endpoint.
#   ERROR      (server->client): header + code u8.
#
# Run:
#   python signaling_server_multipeer.py            # listens on UDP 5000
#   python signaling_server_multipeer.py 5000       # explicit port
# =============================================================================

import signal
import socket
import struct
import sys
import time

# -----------------------------------------------------------------------------
# Protocol constants
# -----------------------------------------------------------------------------
PROTO_MAGIC = 0x4D4A414D  # 'M','J','A','M'
PROTO_VERSION = 2         # bumped from 1: REGISTER/PEER now carry a sender ID
HEADER_SIZE = 8           # magic(4) ver(1) type(1) reserved(2)
ROOM_CODE_MAX = 32
MAX_MSG = 256

# Message types
MSG_REGISTER = 1    # client -> server
MSG_REGISTERED = 2  # server -> client
MSG_PEER = 3        # server -> client
MSG_PING = 4        # client -> server (keepalive)
MSG_BYE = 5         # client -> server
MSG_ERROR = 6       # server -> client
MSG_PUNCH = 10      # client -> client
MSG_PUNCH_ACK = 11  # client -> client

# Error codes (in MSG_ERROR payload)
ERR_BAD_REQUEST = 1
ERR_ROOM_FULL = 2
ERR_RATE_LIMITED = 3
ERR_VERSION = 4

# Registration status (in MSG_REGISTERED payload)
REG_OK_WAITING = 0
REG_ROOM_FULL = 1

# -----------------------------------------------------------------------------
# Tunables
# -----------------------------------------------------------------------------
DEFAULT_PORT = 5000
MAX_ROOMS = 1000          # total simultaneous rooms
MAX_MEMBERS = 8           # peers per room (full-mesh jam)
MEMBER_TIMEOUT_S = 30     # drop stale members
REG_RATE_WINDOW_S = 1     # rate-limit window
REG_RATE_MAX = 20         # max REGISTERs / window / IP

# -----------------------------------------------------------------------------
# Clean shutdown
# -----------------------------------------------------------------------------
g_running = True


def signal_handler(signum, frame):
    global g_running
    g_running = False


def now_seconds():
    # Monotonic clock, matching the C++ steady_clock (not wall time).
    return time.monotonic()


# -----------------------------------------------------------------------------
# Server state
# -----------------------------------------------------------------------------
class Member:
    __slots__ = ("sender_id", "addr", "pub_ip", "pub_port",
                 "local_ip", "local_port", "last_seen")

    def __init__(self):
        self.sender_id = 0        # uint8 (0-255), user-provided stable identity
        self.addr = None          # (ip_str, port) for sendto / endpoint matching
        self.pub_ip = b"\x00\x00\x00\x00"   # 4 bytes, network order (wire)
        self.pub_port = b"\x00\x00"         # 2 bytes, network order (wire)
        self.local_ip = b"\x00\x00\x00\x00"  # 4 bytes, raw (echoed as-is)
        self.local_port = b"\x00\x00"        # 2 bytes, raw (echoed as-is)
        self.last_seen = 0.0


class Room:
    __slots__ = ("members",)

    def __init__(self):
        self.members = {}         # sender_id (int) -> Member


g_rooms = {}    # room_code (str) -> Room

# crude per-source-IP rate limiter for REGISTER; key = source ip string
g_rate = {}     # ip_str -> [window_start, count]


# -----------------------------------------------------------------------------
# Serialization helpers (byte-for-byte compatible with the C++ Writer/Reader)
# -----------------------------------------------------------------------------
def write_header(msg_type):
    # magic: little-endian uint32; version: u8; type: u8; reserved: u16 = 0
    return struct.pack("<IBBH", PROTO_MAGIC, PROTO_VERSION, msg_type, 0)


def read_header(data):
    # Returns msg_type on success, or None if too short / bad magic / bad version.
    if len(data) < HEADER_SIZE:
        return None
    magic, ver, msg_type, _reserved = struct.unpack_from("<IBBH", data, 0)
    if magic != PROTO_MAGIC:
        return None
    if ver != PROTO_VERSION:
        return None
    return msg_type


def addr_to_wire(addr):
    """(ip_str, port) -> (ip_bytes[4] net order, port_bytes[2] net order)."""
    ip_bytes = socket.inet_aton(addr[0])        # already network byte order
    port_bytes = struct.pack("!H", addr[1])     # network (big-endian) order
    return ip_bytes, port_bytes


# -----------------------------------------------------------------------------
# Message senders
# -----------------------------------------------------------------------------
def send_error(sock, to_addr, code):
    buf = write_header(MSG_ERROR) + struct.pack("<B", code)
    sock.sendto(buf, to_addr)


def send_registered(sock, to_addr, pub_ip, pub_port, status):
    buf = write_header(MSG_REGISTERED) + pub_ip + pub_port + struct.pack("<B", status)
    sock.sendto(buf, to_addr)


def send_peer(sock, to_addr, peer):
    # public ip/port, local ip/port (raw net-order bytes), then the peer's id.
    buf = (write_header(MSG_PEER)
           + peer.pub_ip + peer.pub_port
           + peer.local_ip + peer.local_port
           + struct.pack("<B", peer.sender_id))
    sock.sendto(buf, to_addr)


# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------
def rate_ok(src_ip_str, now):
    """True if this source IP is allowed another REGISTER right now."""
    e = g_rate.get(src_ip_str)
    if e is None:
        e = [0.0, 0]
        g_rate[src_ip_str] = e
    if now - e[0] >= REG_RATE_WINDOW_S:
        e[0] = now
        e[1] = 0
    if e[1] >= REG_RATE_MAX:
        return False
    e[1] += 1
    return True


def find_member_by_addr(room, addr):
    """Return the Member in a room whose endpoint matches addr, or None."""
    for m in room.members.values():
        if m.addr == addr:
            return m
    return None


def introduce_member(sock, room, sender_id):
    """Mesh a (new or moved) member with every other member in the room."""
    m = room.members.get(sender_id)
    if m is None:
        return
    for other_id, other in room.members.items():
        if other_id == sender_id:
            continue
        send_peer(sock, m.addr, other)      # tell m about the other peer
        send_peer(sock, other.addr, m)      # tell the other peer about m
        print("[room] introduced id=%d %s:%u <-> id=%d %s:%u" % (
            m.sender_id, m.addr[0], m.addr[1],
            other.sender_id, other.addr[0], other.addr[1]))


def reap_stale(now):
    """Drop members not seen recently; erase empty rooms."""
    dead_rooms = []
    for code, room in g_rooms.items():
        stale_ids = [sid for sid, m in room.members.items()
                     if now - m.last_seen > MEMBER_TIMEOUT_S]
        for sid in stale_ids:
            print("[reap] room=\"%s\" id=%d timed out" % (code, sid))
            del room.members[sid]
        if not room.members:
            dead_rooms.append(code)
    for code in dead_rooms:
        del g_rooms[code]


# -----------------------------------------------------------------------------
# Message handling
# -----------------------------------------------------------------------------
def handle_register(sock, from_addr, payload, now):
    # payload is the bytes AFTER the 8-byte header.
    src_ip_str = from_addr[0]

    if not rate_ok(src_ip_str, now):
        send_error(sock, from_addr, ERR_RATE_LIMITED)
        return

    # room_len (u8)
    if len(payload) < 1:
        send_error(sock, from_addr, ERR_BAD_REQUEST)
        return
    room_len = payload[0]
    off = 1
    if room_len == 0 or room_len > ROOM_CODE_MAX:
        send_error(sock, from_addr, ERR_BAD_REQUEST)
        return

    # room bytes
    if len(payload) < off + room_len:
        send_error(sock, from_addr, ERR_BAD_REQUEST)
        return
    room_bytes = payload[off:off + room_len]
    off += room_len

    # local_ip (4) + local_port (2) + sender_id (1) = 7 bytes
    if len(payload) < off + 7:
        send_error(sock, from_addr, ERR_BAD_REQUEST)
        return
    local_ip = bytes(payload[off:off + 4])
    local_port = bytes(payload[off + 4:off + 6])
    sender_id = payload[off + 6]

    try:
        room_code = room_bytes.decode("utf-8")
    except UnicodeDecodeError:
        room_code = room_bytes.decode("latin-1")

    room = g_rooms.get(room_code)
    if room is None:
        if len(g_rooms) >= MAX_ROOMS:
            send_error(sock, from_addr, ERR_RATE_LIMITED)
            return
        room = Room()
        g_rooms[room_code] = room

    pub_ip, pub_port = addr_to_wire(from_addr)

    existing = room.members.get(sender_id)
    if existing is None:
        # New identity. Enforce room capacity.
        if len(room.members) >= MAX_MEMBERS:
            send_registered(sock, from_addr, pub_ip, pub_port, REG_ROOM_FULL)
            send_error(sock, from_addr, ERR_ROOM_FULL)
            return
        m = Member()
        m.sender_id = sender_id
        m.addr = from_addr
        m.pub_ip = pub_ip
        m.pub_port = pub_port
        m.local_ip = local_ip
        m.local_port = local_port
        m.last_seen = now
        room.members[sender_id] = m
        endpoint_changed = True   # brand new -> introduce
    else:
        # Same identity re-registering. Trust the ID: update its endpoint.
        # (This is what makes NAT-rebind/rejoin robust.)
        endpoint_changed = (existing.addr != from_addr)
        existing.addr = from_addr
        existing.pub_ip = pub_ip
        existing.pub_port = pub_port
        existing.local_ip = local_ip
        existing.local_port = local_port
        existing.last_seen = now

    lan_ip = socket.inet_ntoa(local_ip)
    lan_port = struct.unpack("!H", local_port)[0]
    print("[reg]  room=\"%s\" id=%d  %s:%u (lan %s:%u)  members=%d" % (
        room_code, sender_id, from_addr[0], from_addr[1],
        lan_ip, lan_port, len(room.members)))

    send_registered(sock, from_addr, pub_ip, pub_port, REG_OK_WAITING)

    # Introduce only when the member is new or has moved, so periodic
    # re-registers (keepalive) don't spam PEER messages.
    if endpoint_changed:
        introduce_member(sock, room, sender_id)


def handle_ping(from_addr, now):
    for room in g_rooms.values():
        m = find_member_by_addr(room, from_addr)
        if m is not None:
            m.last_seen = now
            return


def handle_bye(from_addr):
    for code, room in g_rooms.items():
        m = find_member_by_addr(room, from_addr)
        if m is not None:
            print("[bye]  room=\"%s\" id=%d" % (code, m.sender_id))
            del room.members[m.sender_id]
            return


# -----------------------------------------------------------------------------
# main
# -----------------------------------------------------------------------------
def main(argv):
    port = DEFAULT_PORT
    if len(argv) >= 2:
        try:
            port = int(argv[1])
        except ValueError:
            port = DEFAULT_PORT

    signal.signal(signal.SIGINT, signal_handler)
    if hasattr(signal, "SIGTERM") and sys.platform != "win32":
        signal.signal(signal.SIGTERM, signal_handler)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.bind(("0.0.0.0", port))
    except OSError as e:
        sys.stderr.write("Failed to bind on UDP port %u: %s\n" % (port, e))
        win = getattr(e, "winerror", None)
        if win == 10048 or e.errno == 98:          # WSAEADDRINUSE / EADDRINUSE
            sys.stderr.write(
                "  Port %u is already in use. Another instance may still be\n"
                "  running, or another app owns it. Pick a different port:\n"
                "    python signaling_server_multipeer.py 5001\n" % port)
        elif win == 10013 or e.errno == 13:        # WSAEACCES / EACCES
            sys.stderr.write(
                "  Access denied. On Windows the port may fall in a reserved\n"
                "  range (Hyper-V/WSL/Docker). Check with:\n"
                "    netsh int ipv4 show excludedportrange protocol=udp\n"
                "  then pick a port outside those ranges.\n")
        sock.close()
        return 1

    # Receive timeout so we can reap stale members and notice shutdown.
    sock.settimeout(0.5)

    print("============================================================")
    print("  MusiConnect Signaling Server  (multi-peer, protocol v2)")
    print("============================================================")
    print("  Listening   : UDP 0.0.0.0:%u" % port)
    print("  Max rooms   : %d" % MAX_ROOMS)
    print("  Max members : %d per room   member timeout: %ds" % (
        MAX_MEMBERS, MEMBER_TIMEOUT_S))
    print("  Ctrl+C to stop")
    print("============================================================")

    last_reap = now_seconds()

    while g_running:
        try:
            data, from_addr = sock.recvfrom(MAX_MSG)
        except socket.timeout:
            data, from_addr = None, None
        except OSError:
            data, from_addr = None, None

        now = now_seconds()
        if now - last_reap >= 1.0:
            reap_stale(now)
            last_reap = now

        if data is None or len(data) < HEADER_SIZE:
            continue  # timeout or runt

        msg_type = read_header(data)
        if msg_type is None:
            # wrong magic/version (e.g. a v1 client talking to the v2 server)
            continue

        payload = data[HEADER_SIZE:]

        if msg_type == MSG_REGISTER:
            handle_register(sock, from_addr, payload, now)
        elif msg_type == MSG_PING:
            handle_ping(from_addr, now)
        elif msg_type == MSG_BYE:
            handle_bye(from_addr)
        else:
            # servers ignore PUNCH/PUNCH_ACK/etc. addressed to them
            pass

    total_members = sum(len(r.members) for r in g_rooms.values())
    print("\n[server] shutting down (%u rooms, %u members)" % (
        len(g_rooms), total_members))
    sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
