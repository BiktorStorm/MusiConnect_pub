import socket
import struct
import time
import signal
import sounddevice as sd
import numpy as np

# =============================================================================
# ASIO AUDIO RECEIVER — Uses sounddevice with ASIO for lowest latency
#
# Requires: pip install sounddevice numpy
# Requires: ASIO driver installed (native or ASIO4ALL)
#
# Plays immediately — no jitter buffer. Measures true end-to-end latency.
# Packet: [seq (4B)] + [capture_timestamp (8B)] + [PCM audio (16-bit)]
# =============================================================================

signal.signal(signal.SIGINT, signal.SIG_DFL)

# =============================================================================
# CONNECTION — Uncomment ONE of the two blocks below
# =============================================================================

# --- LOCALHOST (same machine) ---
IP = '0.0.0.0'
PORT = 12345
SENDER_LISTEN_PORT = 12346

# --- LAN (another computer on the same network) ---
# IP = '0.0.0.0'  # listen on all interfaces (no change needed)
# PORT = 12345
# SENDER_LISTEN_PORT = 12346
# # NOTE: receiver binds to 0.0.0.0 either way — it accepts from any sender.
# # Just make sure Windows Firewall allows UDP on ports 12345 and 12346:
# #   netsh advfirewall firewall add rule name="Audio UDP" dir=in action=allow protocol=UDP localport=12345,12346

# Audio settings (must match sender)
RATE = 48000
CHANNELS = 1
FRAME_SIZE = 96  # 2ms at 48kHz
HEADER_SIZE = 12
AUDIO_BYTES = FRAME_SIZE * 2  # 16-bit = 2 bytes per sample

# Setup socket
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
sock.settimeout(1.0)
sock.bind((IP, PORT))

# List available devices
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

device_index = input("Select output device index (or press Enter for default): ").strip()
device_index = int(device_index) if device_index else None

if device_index is not None:
    sd.default.device[1] = device_index

print(f"\n[Receiver] Frame: {FRAME_SIZE} samples ({FRAME_SIZE/RATE*1000:.1f}ms)")
print(f"[Receiver] No jitter buffer — immediate playback")
print(f"[Receiver] Press Ctrl+C to stop\n")

# Output stream
stream = sd.OutputStream(
    samplerate=RATE,
    channels=CHANNELS,
    dtype='int16',
    blocksize=FRAME_SIZE,
    latency='low',
)
stream.start()

packets_received = 0
latency_log = []

try:
    while True:
        try:
            data, addr = sock.recvfrom(HEADER_SIZE + AUDIO_BYTES + 64)
        except socket.timeout:
            continue

        # Unpack
        seq, t_captured = struct.unpack('<Id', data[:HEADER_SIZE])
        audio_bytes = data[HEADER_SIZE:]

        # Convert to numpy array for sounddevice
        audio_array = np.frombuffer(audio_bytes, dtype=np.int16).reshape(-1, CHANNELS)

        # Measure true latency (same-machine only)
        t_before_play = time.perf_counter()
        one_way = (t_before_play - t_captured) * 1000

        # Play immediately
        stream.write(audio_array)

        latency_log.append(one_way)
        packets_received += 1

        # Echo header back for RTT
        sock.sendto(data[:HEADER_SIZE], (addr[0], SENDER_LISTEN_PORT))

        # Stats every ~1 second
        if packets_received % 500 == 0:
            recent = latency_log[-500:]
            avg = sum(recent) / len(recent)
            print(f"  [Latency] capture→pre-play: avg={avg:.2f}ms | "
                  f"min={min(recent):.2f}ms | max={max(recent):.2f}ms | "
                  f"pkts={packets_received}")

except KeyboardInterrupt:
    pass

stream.stop()
stream.close()
sock.close()

print(f"\n--- Summary ---")
print(f"Packets received: {packets_received}")
if latency_log:
    avg = sum(latency_log) / len(latency_log)
    print(f"Capture → Pre-play — Avg: {avg:.2f}ms | Min: {min(latency_log):.2f}ms | Max: {max(latency_log):.2f}ms")
    print(f"\nNote: add ~1-3ms for ASIO output hardware buffer")
