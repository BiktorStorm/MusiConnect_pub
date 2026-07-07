import socket
import time
import struct

# Connection
RECEIVER_IP = '127.0.0.1'
RECEIVER_PORT = 12345
LISTEN_PORT = 12346

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(('0.0.0.0', LISTEN_PORT))
sock.setblocking(False)

rtt_list = []

for i in range(1000):
    t = time.perf_counter()
    packet = struct.pack('<Id', i, t)  # sequence 0-999 + timestamp
    sock.sendto(packet, (RECEIVER_IP, RECEIVER_PORT))
    time.sleep(0.002)

    # Check for pongs (non-blocking)
    try:
        while True:
            data, _ = sock.recvfrom(64)
            seq, t_sent = struct.unpack('<Id', data)
            rtt = (time.perf_counter() - t_sent) * 1000
            rtt_list.append(rtt)
            print(f"[{seq}] RTT: {rtt:.2f}ms")
    except (BlockingIOError, ConnectionResetError):
        pass

# Drain remaining pongs
time.sleep(0.1)
try:
    while True:
        data, _ = sock.recvfrom(64)
        seq, t_sent = struct.unpack('<Id', data)
        rtt = (time.perf_counter() - t_sent) * 1000
        rtt_list.append(rtt)
        print(f"[{seq}] RTT: {rtt:.2f}ms")
except (BlockingIOError, ConnectionResetError):
    pass

sock.close()

if rtt_list:
    avg = sum(rtt_list) / len(rtt_list)
    print(f"\n--- RTT Stats ---")
    print(f"Packets sent: 1000 | Pongs received: {len(rtt_list)}")
    print(f"Avg: {avg:.2f}ms | Min: {min(rtt_list):.2f}ms | Max: {max(rtt_list):.2f}ms")
