import socket
import struct
import time
import signal
import threading
import sounddevice as sd
import numpy as np
from low_latency_backend import select_backend, open_stream

# =============================================================================
# LOW-LATENCY AUDIO SENDER — callback-driven capture
#
# The audio hardware calls `capture_callback` on its own realtime thread each
# time a block of samples is ready. We send that block immediately over UDP.
# This gives stable, hardware-timed capture instead of a Python polling loop.
#
# Requires: pip install sounddevice numpy
# Auto-selects ASIO > WASAPI-exclusive > shared (see low_latency_backend.py).
#
# Packet: [seq (4B)] + [capture_timestamp (8B)] + [PCM audio (16-bit)]
# =============================================================================

signal.signal(signal.SIGINT, signal.SIG_DFL)

# --- Connection ---
RECEIVER_IP = '127.0.0.1'
RECEIVER_PORT = 12345
LISTEN_PORT = 12346

# --- Audio settings (must match receiver) ---
RATE = 48000
CHANNELS = 1
FRAME_SIZE = 96  # 2ms at 48kHz

# Setup socket
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 16)
sock.bind(('0.0.0.0', LISTEN_PORT))
sock.setblocking(False)

backend_info = select_backend('input')

# Shared state (written from the audio callback, read from main thread)
seq = 0
last_peak = 0.0
running = True


def capture_callback(indata, frames, time_info, status):
    """Called by the audio hardware thread with `frames` samples of input."""
    global seq, last_peak
    if status:
        # e.g. input overflow — audio thread couldn't keep up
        pass
    t = time.perf_counter()
    header = struct.pack('<Id', seq, t)
    try:
        sock.sendto(header + indata.tobytes(), (RECEIVER_IP, RECEIVER_PORT))
    except OSError:
        pass
    seq += 1
    # Cheap peak for the meter (max abs sample, normalized)
    last_peak = float(np.abs(indata).max()) / 32768.0


stream, backend_label = open_stream(
    'input', backend_info,
    samplerate=RATE, channels=CHANNELS, dtype='int16',
    blocksize=FRAME_SIZE, latency='low', callback=capture_callback,
)

print(f"\n[Sender] Backend: {backend_label}")
print(f"[Sender] Frame: {FRAME_SIZE} samples ({FRAME_SIZE/RATE*1000:.1f}ms)")
print(f"[Sender] Sending to {RECEIVER_IP}:{RECEIVER_PORT}")
print(f"[Sender] Press Ctrl+C to stop\n")

rtt_list = []
stream.start()

try:
    while True:
        # Drain RTT pongs (non-blocking) and print meter — off the audio thread
        try:
            while True:
                data, _ = sock.recvfrom(64)
                pong_seq, t_sent = struct.unpack('<Id', data)
                rtt = (time.perf_counter() - t_sent) * 1000
                rtt_list.append(rtt)
        except (BlockingIOError, ConnectionResetError, OSError):
            pass

        bars = int(last_peak * 40)
        rtt_txt = f"RTT ~{sum(rtt_list[-100:])/len(rtt_list[-100:]):.1f}ms" if rtt_list else "RTT --"
        print(f"  [IN ] {'#' * bars:<40} {last_peak:5.3f}  sent={seq}  {rtt_txt}")
        time.sleep(0.2)

except KeyboardInterrupt:
    pass

running = False
stream.stop()
stream.close()
sock.close()

print(f"\n--- Summary ---")
print(f"Packets sent: {seq}")
if rtt_list:
    avg = sum(rtt_list) / len(rtt_list)
    print(f"RTT — Avg: {avg:.2f}ms | Min: {min(rtt_list):.2f}ms | Max: {max(rtt_list):.2f}ms")
