import socket
import struct
import pyaudio

# =============================================================================
# AUDIO RECEIVER — Receives raw PCM over UDP, plays IMMEDIATELY (no buffer)
#
# This is the "best case" baseline: zero buffering beyond one frame.
# You WILL hear glitches if packets arrive late or out of order.
# That's the point — this measures the floor.
#
# Packet format: [seq (4 bytes)] + [timestamp (8 bytes)] + [PCM audio data]
# =============================================================================

# Connection
IP = '0.0.0.0'
PORT = 12345
SENDER_LISTEN_PORT = 12346

# Audio settings (must match sender)
RATE = 48000
CHANNELS = 1
FORMAT = pyaudio.paInt16       # 2 bytes per sample
FRAME_SIZE = 96                # 96 samples = 2ms at 48kHz
HEADER_SIZE = 12               # 4 (seq) + 8 (timestamp)
AUDIO_BYTES = FRAME_SIZE * 2   # 2 bytes per sample * 96 samples

# Setup socket
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2048)
sock.bind((IP, PORT))

# Setup audio output
pa = pyaudio.PyAudio()
stream = pa.open(
    format=FORMAT,
    channels=CHANNELS,
    rate=RATE,
    output=True,
    frames_per_buffer=FRAME_SIZE
)

print(f"[Receiver] Listening on port {PORT}")
print(f"[Receiver] Playing immediately — no jitter buffer")
print(f"[Receiver] Frame: {FRAME_SIZE} samples ({FRAME_SIZE/RATE*1000:.1f}ms)")
print(f"[Receiver] Press Ctrl+C to stop\n")

packets_received = 0
packets_out_of_order = 0
last_seq = -1

try:
    while True:
        data, addr = sock.recvfrom(HEADER_SIZE + AUDIO_BYTES + 64)

        # Unpack header
        seq, t_sent = struct.unpack('<Id', data[:HEADER_SIZE])
        audio_data = data[HEADER_SIZE:]

        # Track ordering
        if seq <= last_seq:
            packets_out_of_order += 1
        last_seq = seq
        packets_received += 1

        # Play immediately — no buffer
        stream.write(audio_data)

        # Echo header back for RTT measurement
        sock.sendto(data[:HEADER_SIZE], (addr[0], SENDER_LISTEN_PORT))

        # Stats every ~1 second
        if packets_received % 500 == 0:
            print(f"  [Stats] received={packets_received} out_of_order={packets_out_of_order}")

except KeyboardInterrupt:
    pass

# Cleanup
stream.stop_stream()
stream.close()
pa.terminate()
sock.close()

print(f"\n--- Summary ---")
print(f"Packets received: {packets_received}")
print(f"Out of order: {packets_out_of_order}")
