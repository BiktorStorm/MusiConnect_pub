import socket
import struct
import time
import pyaudio

# =============================================================================
# AUDIO SENDER — Captures mic, sends raw PCM frames over UDP
#
# Each packet: [seq (4 bytes)] + [timestamp (8 bytes)] + [PCM audio data]
# =============================================================================

# Connection
RECEIVER_IP = '127.0.0.1'
RECEIVER_PORT = 12345
LISTEN_PORT = 12346

# Audio settings
RATE = 48000
CHANNELS = 1
FORMAT = pyaudio.paInt16       # 2 bytes per sample
FRAME_SIZE = 96                # 96 samples = 2ms at 48kHz

# Setup socket
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(('0.0.0.0', LISTEN_PORT))
sock.setblocking(False)

# Setup audio capture
pa = pyaudio.PyAudio()
stream = pa.open(
    format=FORMAT,
    channels=CHANNELS,
    rate=RATE,
    input=True,
    frames_per_buffer=FRAME_SIZE
)

print(f"[Sender] Capturing mic: {FRAME_SIZE} samples ({FRAME_SIZE/RATE*1000:.1f}ms) per packet")
print(f"[Sender] Sending to {RECEIVER_IP}:{RECEIVER_PORT}")
print(f"[Sender] Press Ctrl+C to stop\n")

seq = 0
rtt_list = []

try:
    while True:
        # Capture one frame from mic
        audio_data = stream.read(FRAME_SIZE, exception_on_overflow=False)

        # Pack: seq + timestamp + audio
        t = time.perf_counter()
        header = struct.pack('<Id', seq, t)
        sock.sendto(header + audio_data, (RECEIVER_IP, RECEIVER_PORT))
        seq += 1

        # Check for RTT pongs (non-blocking)
        try:
            while True:
                data, _ = sock.recvfrom(64)
                pong_seq, t_sent = struct.unpack('<Id', data)
                rtt = (time.perf_counter() - t_sent) * 1000
                rtt_list.append(rtt)
                if seq % 500 == 0:  # print every ~1 second
                    print(f"  [RTT] seq={pong_seq} rtt={rtt:.2f}ms")
        except (BlockingIOError, ConnectionResetError):
            pass

except KeyboardInterrupt:
    pass

# Cleanup
stream.stop_stream()
stream.close()
pa.terminate()
sock.close()

print(f"\n--- Summary ---")
print(f"Packets sent: {seq}")
if rtt_list:
    avg = sum(rtt_list) / len(rtt_list)
    print(f"RTT — Avg: {avg:.2f}ms | Min: {min(rtt_list):.2f}ms | Max: {max(rtt_list):.2f}ms")
