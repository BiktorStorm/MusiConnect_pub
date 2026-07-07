import socket
import struct
import time
import signal
import sounddevice as sd
import numpy as np

# =============================================================================
# ASIO AUDIO SENDER — Uses sounddevice with ASIO for lowest latency
#
# Requires: pip install sounddevice numpy
# Requires: ASIO driver installed (native or ASIO4ALL)
#
# Packet: [seq (4B)] + [capture_timestamp (8B)] + [PCM audio (16-bit)]
# =============================================================================

signal.signal(signal.SIGINT, signal.SIG_DFL)

# =============================================================================
# CONNECTION — Uncomment ONE of the two blocks below
# =============================================================================

# --- LOCALHOST (same machine) ---
RECEIVER_IP = '127.0.0.1'
RECEIVER_PORT = 12345
LISTEN_PORT = 12346

# --- LAN (another computer on the same network) ---
# RECEIVER_IP = '192.168.1.XXX'  # <-- replace with receiver's local IP (run ipconfig on that machine)
# RECEIVER_PORT = 12345
# LISTEN_PORT = 12346

# Audio settings
RATE = 48000
CHANNELS = 1
FRAME_SIZE = 96  # 2ms at 48kHz. Try 64 (1.3ms) or 128 (2.7ms)

# Setup socket
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1024)
sock.bind(('0.0.0.0', LISTEN_PORT))
sock.setblocking(False)

# List available devices — look for ASIO ones
print("=== Available Audio Devices ===")
print(sd.query_devices())
print()

# Filter ASIO devices
asio_hostapi = None
for i, api in enumerate(sd.query_hostapis()):
    if 'ASIO' in api['name']:
        asio_hostapi = i
        break

if asio_hostapi is not None:
    print(f"ASIO host API found (index {asio_hostapi})")
    asio_devices = [d for d in range(len(sd.query_devices()))
                    if sd.query_devices(d)['hostapi'] == asio_hostapi]
    print(f"ASIO devices: {[(d, sd.query_devices(d)['name']) for d in asio_devices]}")
else:
    print("WARNING: No ASIO host API found! Install ASIO4ALL or use an audio interface.")
    print("Falling back to default device.\n")

device_index = input("Select input device index (or press Enter for default): ").strip()
device_index = int(device_index) if device_index else None

if device_index is not None:
    sd.default.device[0] = device_index

print(f"\n[Sender] Frame: {FRAME_SIZE} samples ({FRAME_SIZE/RATE*1000:.1f}ms)")
print(f"[Sender] Sending to {RECEIVER_IP}:{RECEIVER_PORT}")
print(f"[Sender] Press Ctrl+C to stop\n")

seq = 0
rtt_list = []

# Blocking capture loop using sounddevice InputStream
stream = sd.InputStream(
    samplerate=RATE,
    channels=CHANNELS,
    dtype='int16',
    blocksize=FRAME_SIZE,
    latency='low',
)
stream.start()

try:
    while True:
        # Read one frame (blocks until FRAME_SIZE samples are captured)
        audio_data, overflowed = stream.read(FRAME_SIZE)
        if overflowed:
            pass  # frame was late, but keep going

        # Timestamp after capture
        t = time.perf_counter()

        # Send: header + raw PCM bytes
        header = struct.pack('<Id', seq, t)
        sock.sendto(header + audio_data.tobytes(), (RECEIVER_IP, RECEIVER_PORT))
        seq += 1

        # Check for RTT pongs
        try:
            while True:
                data, _ = sock.recvfrom(64)
                pong_seq, t_sent = struct.unpack('<Id', data)
                rtt = (time.perf_counter() - t_sent) * 1000
                rtt_list.append(rtt)
                if len(rtt_list) % 200 == 0:
                    avg = sum(rtt_list[-200:]) / 200
                    print(f"  [RTT] last 200 avg: {avg:.2f}ms | latest: {rtt:.2f}ms")
        except (BlockingIOError, ConnectionResetError):
            pass

except KeyboardInterrupt:
    pass

stream.stop()
stream.close()
sock.close()

print(f"\n--- Summary ---")
print(f"Packets sent: {seq}")
if rtt_list:
    avg = sum(rtt_list) / len(rtt_list)
    print(f"RTT — Avg: {avg:.2f}ms | Min: {min(rtt_list):.2f}ms | Max: {max(rtt_list):.2f}ms")
