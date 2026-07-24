#!/usr/bin/env python3
# =============================================================================
# MusiConnect Signaling / Rendezvous Server  (Python port)
#
# A single-threaded UDP server that introduces two peers so they can hole-punch
# and stream audio directly. It NEVER carries audio.
#
# This is a faithful port of signaling_server.cpp. The wire format is byte-for-
# byte compatible with the C++ clients (signaling_protocol.h / PROTOCOL.md):
#   - The 8-byte header is a raw little-endian memcpy layout, matching the C++
#     Writer/Reader (magic as a little-endian uint32, then version, type, and a
#     reserved uint16).
#   - Endpoint fields (ip/port) travel in network byte order, exactly as the
#     C++ server copies the raw sockaddr_in fields.
#
# Responsibilities (see PROTOCOL.md):
#   - REGISTER:  add sender to a room (max 2 members); reflect its public
#                endpoint back (STUN-style) in REGISTERED.
#   - When a room has 2 members: send each the other's endpoints (PEER).
#   - PING:      keepalive; refresh last-seen.
#   - BYE:       remove sender from its room.
#   - Timeouts:  drop members not seen for MEMBER_TIMEOUT.
#   - Basic abuse limits: max rooms, per-source REGISTER rate limit.
#
# Run:
#   python signaling_server_python.py            # listens on UDP 5000
#   python signaling_server_python.py 5000       # explicit port
# =============================================================================

import signal
import socket
import struct
import sys
import time

# -----------------------------------------------------------------------------
# Protocol constants (mirror signaling_protocol.h, namespace sig)
# -----------------------------------------------------------------------------
PROTO_MAGIC = 0x4D4A414D  # 'M','J','A','M'
PROTO_VERSION = 1
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
# Tunables (mirror the C++ static const values)
# -----------------------------------------------------------------------------
DEFAULT_PORT = 5000
MAX_ROOMS = 1000          # total simultaneous rooms
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
    # Matches the C++ steady_clock: a monotonic clock, not wall time.
    return time.monotonic()


# -----------------------------------------------------------------------------
# Server state
# -----------------------------------------------------------------------------
class Member:
    __slots__ = ("used", "addr", "pub_ip", "pub_port",
                 "local_ip", "local_port", "last_seen")

    def __init__(self):
        self.used = False
        self.addr = None          # (ip_str, port) for sendto / matching
        self.pub_ip = b"\x00\x00\x00\x00"   # 4 bytes, network order (wire)
        self.pub_port = b"\x00\x00"         # 2 bytes, network order (wire)
        self.local_ip = b"\x00\x00\x00\x00"  # 4 bytes, raw (echoed as-is)
        self.local_port = b"\x00\x00"        # 2 bytes, raw (echoed as-is)
        self.last_seen = 0.0


class Room:
    __slots__ = ("members", "peer_sent")

    def __init__(self):
        self.members = [Member(), Member()]
        self.peer_sent = False    # whether PEER has been dispatched to both


g_rooms = {}    # room_code (str) -> Room

# crude per-source-IP rate limiter for REGISTER; key = source ip string
g_rate = {}     # ip_str -> [window_start, count]


# -----------------------------------------------------------------------------
# Serialization helpers (byte-for-byte compatible with the C++ Writer/Reader)
# -----------------------------------------------------------------------------
def write_header(msg_type):
    # magic: little-endian uint32 (raw memcpy of a native int on x86/ARM64)
    # version: u8, type: u8, reserved: u16 = 0
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
    # pub_ip: 4 bytes net order, pub_port: 2 bytes net order, status: u8
    buf = write_header(MSG_REGISTERED) + pub_ip + pub_port + struct.pack("<B", status)
    sock.sendto(buf, to_addr)


def send_peer(sock, to_addr, peer):
    # public ip/port then local ip/port, all raw network-order bytes.
    buf = (write_header(MSG_PEER)
           + peer.pub_ip + peer.pub_port
           + peer.local_ip + peer.local_port)
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


def find_member(room, addr):
    """Return slot index for a given endpoint in a room, or -1."""
    for i in range(2):
        m = room.members[i]
        if m.used and m.addr == addr:
            return i
    return -1


def free_slot(room):
    for i in range(2):
        if not room.members[i].used:
            return i
    return -1


def maybe_introduce(sock, room):
    """If both members present and not yet introduced, send PEER to both."""
    if room.peer_sent:
        return
    if not room.members[0].used or not room.members[1].used:
        return

    send_peer(sock, room.members[0].addr, room.members[1])
    send_peer(sock, room.members[1].addr, room.members[0])
    room.peer_sent = True

    print("[room] introduced %s:%u <-> %s:%u" % (
        room.members[0].addr[0], room.members[0].addr[1],
        room.members[1].addr[0], room.members[1].addr[1]))


def reap_stale(now):
    """Drop members not seen recently; erase empty rooms."""
    dead_rooms = []
    for code, room in g_rooms.items():
        alive = 0
        for i in range(2):
            m = room.members[i]
            if m.used:
                if now - m.last_seen > MEMBER_TIMEOUT_S:
                    room.members[i] = Member()   # reset
                    room.peer_sent = False       # allow re-introduce if refilled
                else:
                    alive += 1
        if alive == 0:
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

    # local_ip (u32) + local_port (u16), stored/echoed as raw bytes
    if len(payload) < off + 6:
        send_error(sock, from_addr, ERR_BAD_REQUEST)
        return
    local_ip = bytes(payload[off:off + 4])
    local_port = bytes(payload[off + 4:off + 6])

    try:
        room_code = room_bytes.decode("utf-8")
    except UnicodeDecodeError:
        # C++ treats the room code as raw bytes; fall back to latin-1 so any
        # byte sequence remains a stable, distinct key.
        room_code = room_bytes.decode("latin-1")

    room = g_rooms.get(room_code)
    if room is None:
        if len(g_rooms) >= MAX_ROOMS:
            send_error(sock, from_addr, ERR_RATE_LIMITED)
            return
        room = Room()
        g_rooms[room_code] = room

    # Already registered? refresh (re-send REGISTERED, idempotent).
    slot = find_member(room, from_addr)
    if slot < 0:
        slot = free_slot(room)
        if slot < 0:
            # room full with two *other* peers
            pub_ip, pub_port = addr_to_wire(from_addr)
            send_registered(sock, from_addr, pub_ip, pub_port, REG_ROOM_FULL)
            send_error(sock, from_addr, ERR_ROOM_FULL)
            return

    pub_ip, pub_port = addr_to_wire(from_addr)

    m = room.members[slot]
    m.used = True
    m.addr = from_addr
    m.pub_ip = pub_ip
    m.pub_port = pub_port
    m.local_ip = local_ip
    m.local_port = local_port
    m.last_seen = now

    # local ip/port are raw network-order bytes; render for logging.
    lan_ip = socket.inet_ntoa(local_ip)
    lan_port = struct.unpack("!H", local_port)[0]
    print("[reg]  room=\"%s\" slot=%d  %s:%u (lan %s:%u)" % (
        room_code, slot, from_addr[0], from_addr[1], lan_ip, lan_port))

    send_registered(sock, from_addr, pub_ip, pub_port, REG_OK_WAITING)
    maybe_introduce(sock, room)


def handle_ping(from_addr, now):
    for room in g_rooms.values():
        slot = find_member(room, from_addr)
        if slot >= 0:
            room.members[slot].last_seen = now
            return


def handle_bye(from_addr):
    for code, room in g_rooms.items():
        slot = find_member(room, from_addr)
        if slot >= 0:
            print("[bye]  room=\"%s\" slot=%d" % (code, slot))
            room.members[slot] = Member()
            room.peer_sent = False
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
    except OSError:
        sys.stderr.write("Failed to bind on UDP port %u\n" % port)
        sock.close()
        return 1

    # Receive timeout so we can reap stale members and notice shutdown.
    sock.settimeout(0.5)

    print("============================================================")
    print("  MusiConnect Signaling Server")
    print("============================================================")
    print("  Listening : UDP 0.0.0.0:%u" % port)
    print("  Max rooms : %d   member timeout: %ds" % (MAX_ROOMS, MEMBER_TIMEOUT_S))
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
            # wrong magic/version
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

    print("\n[server] shutting down (%u active rooms)" % len(g_rooms))
    sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
