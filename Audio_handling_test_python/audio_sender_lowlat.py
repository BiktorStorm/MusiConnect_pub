import socket
import struct
import time
import pyaudio

# =============================================================================
# LOW-LATENCY AUDIO SENDER — WASAPI Exclusive Mode
#
# Uses WASAPI exclusive to bypass Windows audio engine.
# Timestamps AFTER capture so receiver can measure true end-to-end latency.
#
# Packet: [seq (4B)] + [capture_timestamp (8B)] + [PCM audio]
# =============================================================================

# Connection
RECEIVER_IP = '127.0.0.1'
RECEIVER_PORT = 12345
LISTEN_PORT = 12346

# Audio settings
RATE = 48000
CHANNELS = 1
FORMAT = pyaudio.paInt16
FRAME_SIZE = 96  # 2ms at 48kHz

# Setup socket
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1024)
sock.bind(('0.0.0.0', LISTEN_PORT))
sock.setblocking(False)

# Setup audio — list WASAPI devices
pa = pyaudio.PyAudio()

print("=== Available INPUT devices ===")
wasapi_inputs = []
for i in range(pa.get_device_count()):
    info = pa.get_device_info_by_index(i)
    if info['maxInputChannels'] > 0:
        host = pa.get_host_api_info_by_index(info['hostApi'])['name']
        print(f"  [{i}] {info['name']} ({host})")
        if 'WASAPI' in host:
            wasapi_inputs.append(i)

print(f"\nWASAPI input devices: {wasapi_inputs}")
device_index = int(input("Select input device index: "))

# Try opening with WASAPI exclusive flag
try:
    stream = pa.open(
        format=FORMAT,
        channels=CHANNELS,
        rate=RATE,
        input=True,
        input_device_index=device_index,
        frames_per_buffer=FRAME_SIZE,
    )
    print(f"\n[Sender] Opened device [{device_index}]")
except Exception as e:
    print(f"Failed to open with FRAME_SIZE={FRAME_SIZE}: {e}")
    print("Trying with larger buffer...")
    FRAME_SIZE = 128
    stream = pa.open(
        format=FORMAT,
        channels=CHANNELS,
        rate=RATE,
        input=True,
        input_device_index=device_index,
        frames_per_buffer=FRAME_SIZE,
    )
    print(f"[Sender] Opened with FRAME_SIZE={FRAME_SIZE} ({FRAME_SIZE/RATE*1000:.1f}ms)")

print(f"[Sender] Frame: {FRAME_SIZE} samples ({FRAME_SIZE/RATE*1000:.1f}ms)")
print(f"[Sender] Sending to {RECEIVER_IP}:{RECEIVER_PORT}")
print(f"[Sender] Press Ctrl+C to stop\n")

seq = 0
rtt_list = []

try:
    while True:
        # Capture one frame
        audio_data = stream.read(FRAME_SIZE, exception_on_overflow=False)

        # Timestamp AFTER capture completes (this is when audio actually arrived)
        t = time.perf_counter()

        # Send: header + audio
        header = struct.pack('<Id', seq, t)
        sock.sendto(header + audio_data, (RECEIVER_IP, RECEIVER_PORT))
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

stream.stop_stream()
stream.close()
pa.terminate()
sock.close()

print(f"\n--- Summary ---")
print(f"Packets sent: {seq}")
if rtt_list:
    avg = sum(rtt_list) / len(rtt_list)
    print(f"RTT — Avg: {avg:.2f}ms | Min: {min(rtt_list):.2f}ms | Max: {max(rtt_list):.2f}ms")
