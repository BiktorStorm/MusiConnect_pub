import socket
import struct
import time
import signal
import sounddevice as sd
import numpy as np
from low_latency_backend import select_backend, make_stream

# =============================================================================
# LOW-LATENCY AUDIO RECEIVER — auto-selects ASIO, else WASAPI-exclusive, else default
#
# Requires: pip install sounddevice numpy
# Best latency: ASIO driver installed (e.g. Focusrite). Falls back automatically.
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

# List available devices and pick backend (ASIO > WASAPI-exclusive > default)
backend_info = select_backend('output')

print(f"\n[Receiver] Backend: {backend_info['backend']}")
print(f"[Receiver] Frame: {FRAME_SIZE} samples ({FRAME_SIZE/RATE*1000:.1f}ms)")
print(f"[Receiver] No jitter buffer — immediate playback")
print(f"[Receiver] Press Ctrl+C to stop\n")

# Output stream
stream = make_stream('output', backend_info,
                     samplerate=RATE, channels=CHANNELS,
                     dtype='int16', blocksize=FRAME_SIZE)
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

        # --- Playout level meter (proves real audio is being received & played) ---
        if packets_received % 100 == 0:  # ~5x/sec
            peak = np.abs(audio_array).max() / 32768.0
            bars = int(peak * 40)
            print(f"  [OUT] {'#' * bars:<40} {peak:5.3f}  (recv {packets_received} pkts)")

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
