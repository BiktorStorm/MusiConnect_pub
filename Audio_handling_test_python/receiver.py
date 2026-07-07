import socket
import struct
import time

# Connection
IP = '0.0.0.0'
PORT = 12345
SENDER_LISTEN_PORT = 12346

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
sock.bind((IP, PORT))
sock.setblocking(False)

print("[Receiver] Waiting for sender...")

received = set()

# Block until first packet arrives
sock.setblocking(True)
data, addr = sock.recvfrom(64)
sock.setblocking(False)

seq, _ = struct.unpack('<Id', data)
received.add(seq)
print(seq)
# Echo back for RTT
sock.sendto(data, (addr[0], SENDER_LISTEN_PORT))

try:
    for _ in range(999):
        time.sleep(0.005)
        latest = None
        latest_addr = None
        try:
            while True:
                data, addr = sock.recvfrom(64)
                latest = data
                latest_addr = addr
        except BlockingIOError:
            pass

        if latest:
            seq, _ = struct.unpack('<Id', latest)
            print(seq)
            received.add(seq)
            # Echo back the latest packet for RTT measurement
            sock.sendto(latest, (latest_addr[0], SENDER_LISTEN_PORT))
        else:
            print('nothing in buffer - dropped')

except KeyboardInterrupt:
    print("\n[Receiver] Stopped by user.")

sock.close()

drop_rate = 1 - len(received) / 1000
print(f"\nDrop rate: {drop_rate:.1%}")
