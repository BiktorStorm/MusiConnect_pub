import socket
import struct
import time
import threading
import numpy as np
import sounddevice as sd
from low_latency_backend import select_backend, open_stream, JitterBuffer

# =============================================================================
# LOW-LATENCY AUDIO TRANSCEIVER — combined sender + receiver in one process
#
# Runs both directions simultaneously:
#   - Captures from the local input device and sends over UDP (sender)
#   - Receives UDP audio from the remote peer and plays it back (receiver)
#
# Each peer runs this script. Audio flows:
#   [local mic] -> capture callback -> UDP -> [remote peer]
#   [remote peer] -> UDP -> jitter buffer -> playout callback -> [local speaker]
#
# THREADS:
#   - capture callback : audio HW pulls input -> sends UDP packet (realtime)
#   - playout callback : audio HW pulls output <- jitter buffer (realtime)
#   - net_rx thread    : recvfrom -> parse -> jitter.push() + RTT echo
#   - main thread      : prints combined meters / stats, handles Ctrl+C
#
# Packet: [seq (4B)] + [capture_timestamp (8B)] + [PCM audio (16-bit interleaved)]
# =============================================================================

# --- Connection ---
REMOTE_IP = '192.168.1.110'   # IP of the OTHER peer (change per machine)
REMOTE_PORT = 12345           # port the OTHER peer listens on for audio
LOCAL_PORT = 12345            # port THIS machine listens on for incoming audio
RTT_PORT = 12346              # port for RTT echo replies

# --- Audio (MUST match on both peers) ---
RATE = 48000
CHANNELS = 2                  # stereo -> unlocks WASAPI exclusive
FRAME_SIZE = 96               # block size for both capture and playout (2ms @ 48kHz)

# --- Jitter buffer tuning ---
JITTER_MS = 25                # target prebuffer (latency vs robustness)
MAX_JITTER_MS = 120           # hard cap so latency can't balloon

HEADER_SIZE = 12              # 4 (seq) + 8 (timestamp)

# =============================================================================
# SOCKETS
# =============================================================================

# Receive socket — listens for incoming audio packets
rx_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
rx_sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 16)
rx_sock.bind(('0.0.0.0', LOCAL_PORT))
rx_sock.settimeout(0.5)

# Send socket — sends audio to the remote peer + receives RTT echoes
tx_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
tx_sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 16)
tx_sock.bind(('0.0.0.0', RTT_PORT))
tx_sock.setblocking(False)

# =============================================================================
# DEVICE SELECTION
# =============================================================================

print("\n" + "=" * 60)
print("  AUDIO TRANSCEIVER — Select INPUT device (microphone)")
print("=" * 60)
input_backend_info = select_backend('input')

print("\n" + "=" * 60)
print("  AUDIO TRANSCEIVER — Select OUTPUT device (speakers/headphones)")
print("=" * 60)
output_backend_info = select_backend('output')

# =============================================================================
# JITTER BUFFER (receiver side)
# =============================================================================

jitter = JitterBuffer(channels=CHANNELS, rate=RATE,
                      target_ms=JITTER_MS, max_ms=MAX_JITTER_MS)

# =============================================================================
# SHARED STATE
# =============================================================================

running = True

# Sender state
tx_seq = 0
tx_peak = 0.0

# Receiver state
rx_peak = 0.0
rx_packets = 0
last_one_way = 0.0
rtt_list = []

# =============================================================================
# CALLBACKS
# =============================================================================


def capture_callback(indata, frames, time_info, status):
    """Audio HW thread (input): send each captured block immediately over UDP."""
    global tx_seq, tx_peak
    t = time.perf_counter()
    header = struct.pack('<Id', tx_seq, t)
    try:
        tx_sock.sendto(header + indata.tobytes(), (REMOTE_IP, REMOTE_PORT))
    except OSError:
        pass
    tx_seq += 1
    tx_peak = float(np.abs(indata).max()) / 32768.0


def playout_callback(outdata, frames, time_info, status):
    """Audio HW thread (output): fill outdata from the jitter buffer."""
    global rx_peak
    block = jitter.pull(frames)
    outdata[:] = block
    rx_peak = float(np.abs(block).max()) / 32768.0


# =============================================================================
# STREAMS
# =============================================================================

in_stream, in_backend_label = open_stream(
    'input', input_backend_info,
    samplerate=RATE, channels=CHANNELS, dtype='int16',
    blocksize=FRAME_SIZE, latency='low', callback=capture_callback,
)

out_stream, out_backend_label = open_stream(
    'output', output_backend_info,
    samplerate=RATE, channels=CHANNELS, dtype='int16',
    blocksize=FRAME_SIZE, latency='low', callback=playout_callback,
)

# =============================================================================
# NETWORK RECEIVE THREAD
# =============================================================================


def net_rx_loop():
    """Receive incoming audio packets and push into the jitter buffer."""
    global rx_packets, last_one_way
    while running:
        try:
            data, addr = rx_sock.recvfrom(4096)
        except socket.timeout:
            continue
        except OSError:
            break

        if len(data) <= HEADER_SIZE:
            continue

        seq, t_captured = struct.unpack('<Id', data[:HEADER_SIZE])
        samples = np.frombuffer(data[HEADER_SIZE:], dtype=np.int16)
        if samples.size % CHANNELS != 0:
            continue  # malformed

        frame = samples.reshape(-1, CHANNELS)
        jitter.push(frame)

        last_one_way = (time.perf_counter() - t_captured) * 1000.0
        rx_packets += 1

        # Echo header back so the remote peer can measure RTT
        try:
            rx_sock.sendto(data[:HEADER_SIZE], (addr[0], RTT_PORT))
        except OSError:
            pass


net_thread = threading.Thread(target=net_rx_loop, daemon=True)

# =============================================================================
# START
# =============================================================================

in_hw_ms = float(in_stream.latency or 0.0) * 1000.0
out_hw_ms = float(out_stream.latency or 0.0) * 1000.0

print(f"\n{'=' * 60}")
print(f"  TRANSCEIVER ACTIVE")
print(f"{'=' * 60}")
print(f"  Input  : {in_backend_label} | {CHANNELS}ch @ {RATE}Hz | block {FRAME_SIZE} ({FRAME_SIZE/RATE*1000:.1f}ms)")
print(f"           HW buffer: {in_hw_ms:.1f} ms")
print(f"  Output : {out_backend_label} | {CHANNELS}ch @ {RATE}Hz | block {FRAME_SIZE} ({FRAME_SIZE/RATE*1000:.1f}ms)")
print(f"           HW buffer: {out_hw_ms:.1f} ms")
print(f"  Remote : {REMOTE_IP}:{REMOTE_PORT}")
print(f"  Listen : 0.0.0.0:{LOCAL_PORT}")
print(f"  Jitter : target {JITTER_MS}ms, max {MAX_JITTER_MS}ms")
print(f"  Press Ctrl+C to stop")
print(f"{'=' * 60}\n")

in_stream.start()
out_stream.start()
net_thread.start()

try:
    while True:
        # Drain RTT echo replies (non-blocking)
        try:
            while True:
                data, _ = tx_sock.recvfrom(64)
                _pong_seq, t_sent = struct.unpack('<Id', data)
                rtt_list.append((time.perf_counter() - t_sent) * 1000.0)
        except (BlockingIOError, ConnectionResetError, OSError):
            pass

        # Compute display values
        tx_bars = int(tx_peak * 40)
        rx_bars = int(rx_peak * 40)
        buf_ms = jitter.fill_frames() / RATE * 1000.0
        rtt_txt = (f"RTT ~{sum(rtt_list[-100:]) / len(rtt_list[-100:]):.1f}ms"
                   if rtt_list else "RTT --")
        est_e2e = last_one_way + buf_ms + out_hw_ms

        print(f"  [TX] {'#' * tx_bars:<40} {tx_peak:5.3f}  sent={tx_seq}  {rtt_txt}")
        print(f"  [RX] {'#' * rx_bars:<40} {rx_peak:5.3f}  recv={rx_packets}  "
              f"buf={buf_ms:4.0f}ms  1way~{last_one_way:4.0f}ms  "
              f"e2e~{est_e2e:4.0f}ms  under={jitter.underruns} over={jitter.overflows}")
        print()

        time.sleep(0.3)
except KeyboardInterrupt:
    pass

# =============================================================================
# SHUTDOWN
# =============================================================================

running = False
time.sleep(0.1)
in_stream.stop()
out_stream.stop()
in_stream.close()
out_stream.close()
rx_sock.close()
tx_sock.close()

print(f"\n{'=' * 60}")
print(f"  TRANSCEIVER SUMMARY")
print(f"{'=' * 60}")
print(f"  Packets sent:     {tx_seq}")
print(f"  Packets received: {rx_packets}")
print(f"  Underruns: {jitter.underruns} | Overflows: {jitter.overflows}")
if rtt_list:
    avg_rtt = sum(rtt_list) / len(rtt_list)
    print(f"  RTT — Avg: {avg_rtt:.2f}ms | Min: {min(rtt_list):.2f}ms | Max: {max(rtt_list):.2f}ms")
print(f"  Input HW buffer:  {in_hw_ms:.1f} ms")
print(f"  Output HW buffer: {out_hw_ms:.1f} ms")
print(f"  Jitter target:    {JITTER_MS} ms")
est_full = in_hw_ms + last_one_way + jitter.fill_frames() / RATE * 1000.0 + out_hw_ms
print(f"  Estimated full capture->ear latency: ~{est_full:.0f} ms")
print(f"{'=' * 60}\n")
