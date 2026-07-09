import socket
import struct
import time
import threading
import numpy as np
import sounddevice as sd
from low_latency_backend import select_backend, open_stream, JitterBuffer

# =============================================================================
# LOW-LATENCY AUDIO RECEIVER v2 — callback playout + jitter buffer
#
# WHAT'S DIFFERENT FROM audio_receiver_asio.py:
#   * Playout is CALLBACK-DRIVEN. The audio hardware pulls samples on its own
#     realtime thread; we no longer poll + blocking-write in a Python loop.
#   * A JITTER BUFFER decouples the network from the audio clock. The network
#     thread pushes received frames in; the audio callback pulls fixed blocks
#     out. A small prebuffer (JITTER_MS) absorbs network/timing jitter so the
#     output rarely underruns — this is what removes the crackle/distortion.
#   * STEREO (CHANNELS = 2) so WASAPI EXCLUSIVE mode can open on stereo-native
#     interfaces like the Focusrite (exclusive was rejected for a mono request).
#     Exclusive mode bypasses the Windows mixer -> lower latency, no resampling.
#
# THREADS:
#   - net_loop thread : recvfrom -> parse -> jitter.push()  (+ RTT echo)
#   - audio callback  : jitter.pull() -> outdata            (realtime, hardware)
#   - main thread     : prints meters / stats, handles Ctrl+C
#
# Packet: [seq (4B)] + [capture_timestamp (8B)] + [PCM audio (16-bit interleaved)]
# =============================================================================

# --- Connection ---
IP = '0.0.0.0'          # listen on all interfaces (accepts from any sender)
PORT = 12345
SENDER_LISTEN_PORT = 12346

# --- Audio (MUST match the sender) ---
RATE = 48000
CHANNELS = 2            # stereo -> unlocks WASAPI exclusive on the Focusrite
FRAME_SIZE = 96         # output callback block size (2ms @ 48kHz)

# --- Jitter buffer tuning (the latency <-> robustness dial) ---
JITTER_MS = 25          # target prebuffer. Lower = less latency, more dropouts.
MAX_JITTER_MS = 120     # hard cap so latency can't balloon.

HEADER_SIZE = 12        # 4 (seq) + 8 (timestamp)

# --- Socket ---
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 16)
sock.bind((IP, PORT))
sock.settimeout(0.5)    # so net_loop can notice shutdown

# --- Pick output device / backend ---
backend_info = select_backend('output')

# --- Jitter buffer between network and audio threads ---
jitter = JitterBuffer(channels=CHANNELS, rate=RATE,
                      target_ms=JITTER_MS, max_ms=MAX_JITTER_MS)

# --- Shared state (main thread reads these for the meter) ---
running = True
last_peak = 0.0
last_one_way = 0.0
packets_received = 0


def playout_callback(outdata, frames, time_info, status):
    """Audio hardware thread: fill `outdata` with the next block of audio."""
    global last_peak
    block = jitter.pull(frames)          # (frames, CHANNELS) int16, zero-filled on underrun
    outdata[:] = block
    last_peak = float(np.abs(block).max()) / 32768.0


# Open the output stream with backend fallback (exclusive -> shared).
stream, backend_label = open_stream(
    'output', backend_info,
    samplerate=RATE, channels=CHANNELS, dtype='int16',
    blocksize=FRAME_SIZE, latency='low', callback=playout_callback,
)


def net_loop():
    """Network thread: receive packets, push audio into the jitter buffer."""
    global packets_received, last_one_way
    while running:
        try:
            data, addr = sock.recvfrom(4096)
        except socket.timeout:
            continue
        except OSError:
            break

        if len(data) <= HEADER_SIZE:
            continue

        seq, t_captured = struct.unpack('<Id', data[:HEADER_SIZE])
        samples = np.frombuffer(data[HEADER_SIZE:], dtype=np.int16)
        if samples.size % CHANNELS != 0:
            continue  # malformed packet, skip

        frame = samples.reshape(-1, CHANNELS)
        jitter.push(frame)

        last_one_way = (time.perf_counter() - t_captured) * 1000.0  # same-machine only
        packets_received += 1

        # Echo the header back so the sender can measure RTT
        try:
            sock.sendto(data[:HEADER_SIZE], (addr[0], SENDER_LISTEN_PORT))
        except OSError:
            pass


net_thread = threading.Thread(target=net_loop, daemon=True)

print(f"\n[Receiver v2] Backend: {backend_label}")
print(f"[Receiver v2] {CHANNELS}ch @ {RATE}Hz, block {FRAME_SIZE} ({FRAME_SIZE/RATE*1000:.1f}ms)")
out_hw_ms = float(stream.latency or 0.0) * 1000.0
print(f"[Receiver v2] Output HW buffer (PortAudio negotiated): {out_hw_ms:.1f} ms")
print(f"[Receiver v2] Jitter target {JITTER_MS}ms (max {MAX_JITTER_MS}ms)")
print(f"[Receiver v2] Press Ctrl+C to stop\n")

stream.start()
net_thread.start()

try:
    while True:
        bars = int(last_peak * 40)
        buf_ms = jitter.fill_frames() / RATE * 1000.0
        # Estimated end-to-end = network + jitter fill + output HW buffer.
        # NOTE: excludes the SENDER's input HW buffer (this process can't see it);
        # add the sender's printed "Input HW buffer" for the full capture->ear figure.
        est_e2e = last_one_way + buf_ms + out_hw_ms
        print(f"  [OUT] {'#' * bars:<40} {last_peak:5.3f}  "
              f"recv={packets_received}  buf={buf_ms:4.0f}ms  "
              f"1way~{last_one_way:4.0f}ms  outHW={out_hw_ms:4.0f}ms  "
              f"e2e~{est_e2e:4.0f}ms(+inHW)  under={jitter.underruns} over={jitter.overflows}")
        time.sleep(0.3)
except KeyboardInterrupt:
    pass

running = False
time.sleep(0.1)
stream.stop()
stream.close()
sock.close()

print(f"\n--- Summary ---")
print(f"Packets received: {packets_received}")
print(f"Underruns: {jitter.underruns} | Overflows: {jitter.overflows}")
print(f"Output HW buffer: {out_hw_ms:.1f} ms | Jitter target: {JITTER_MS} ms")
print(f"Estimated end-to-end (this side): ~{last_one_way + jitter.fill_frames()/RATE*1000.0 + out_hw_ms:.0f} ms")
print(f"  + add the sender's 'Input HW buffer' value for the full capture->ear latency.")
