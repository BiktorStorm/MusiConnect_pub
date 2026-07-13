import socket
import struct
import time
import numpy as np
import sounddevice as sd
from low_latency_backend import select_backend, open_stream

# =============================================================================
# LOW-LATENCY AUDIO SENDER v2 — callback-driven stereo capture
#
# WHAT'S DIFFERENT FROM audio_sender_asio.py:
#   * STEREO (CHANNELS = 2) to match receiver v2 and to unlock WASAPI EXCLUSIVE
#     mode on stereo-native interfaces (the Focusrite rejected a mono request in
#     exclusive mode, forcing the higher-latency shared mode).
#   * Otherwise same callback architecture: the audio hardware calls
#     capture_callback on its realtime thread and we send each block immediately.
#
# Packet: [seq (4B)] + [capture_timestamp (8B)] + [PCM audio (16-bit interleaved)]
# =============================================================================

# --- Connection ---
RECEIVER_IP = '192.168.1.110'   # set to the receiver's LAN IP for two machines
RECEIVER_PORT = 12345
LISTEN_PORT = 12346         # receives RTT echo packets from the receiver

# --- Audio (MUST match the receiver) ---
RATE = 48000
CHANNELS = 2                # stereo -> unlocks WASAPI exclusive
FRAME_SIZE = 96             # capture block size (2ms @ 48kHz)

# --- Socket ---
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 16)
sock.bind(('0.0.0.0', LISTEN_PORT))
sock.setblocking(False)

# --- Pick input device / backend ---
backend_info = select_backend('input')

# --- Shared state (written from the audio callback) ---
seq = 0
last_peak = 0.0


def capture_callback(indata, frames, time_info, status):
    """Audio hardware thread: send each captured block immediately over UDP."""
    global seq, last_peak
    t = time.perf_counter()
    header = struct.pack('<Id', seq, t)
    try:
        sock.sendto(header + indata.tobytes(), (RECEIVER_IP, RECEIVER_PORT))
    except OSError:
        pass
    seq += 1
    last_peak = float(np.abs(indata).max()) / 32768.0


# Open the input stream with backend fallback (exclusive -> shared).
stream, backend_label = open_stream(
    'input', backend_info,
    samplerate=RATE, channels=CHANNELS, dtype='int16',
    blocksize=FRAME_SIZE, latency='low', callback=capture_callback,
)

print(f"\n[Sender v2] Backend: {backend_label}")
print(f"[Sender v2] {CHANNELS}ch @ {RATE}Hz, block {FRAME_SIZE} ({FRAME_SIZE/RATE*1000:.1f}ms)")
in_hw_ms = float(stream.latency or 0.0) * 1000.0
print(f"[Sender v2] Input HW buffer (PortAudio negotiated): {in_hw_ms:.1f} ms")
print(f"[Sender v2] Sending to {RECEIVER_IP}:{RECEIVER_PORT}")
print(f"[Sender v2] Press Ctrl+C to stop\n")

rtt_list = []
stream.start()

try:
    while True:
        # Drain RTT pongs (non-blocking) off the audio thread
        try:
            while True:
                data, _ = sock.recvfrom(64)
                _pong_seq, t_sent = struct.unpack('<Id', data)
                rtt_list.append((time.perf_counter() - t_sent) * 1000.0)
        except (BlockingIOError, ConnectionResetError, OSError):
            pass

        bars = int(last_peak * 40)
        rtt_txt = (f"RTT ~{sum(rtt_list[-100:]) / len(rtt_list[-100:]):.1f}ms"
                   if rtt_list else "RTT --")
        print(f"  [IN ] {'#' * bars:<40} {last_peak:5.3f}  sent={seq}  {rtt_txt}")
        time.sleep(0.3)
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
