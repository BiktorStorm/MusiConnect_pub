import socket
import struct
import time
import pyaudio

# =============================================================================
# LOW-LATENCY AUDIO RECEIVER — WASAPI Exclusive Mode
#
# Measures TRUE end-to-end latency (capture → playback) using sender's timestamp.
# Only accurate on the SAME MACHINE (shared clock). On different machines use RTT/2.
#
# Packet: [seq (4B)] + [capture_timestamp (8B)] + [PCM audio]
# =============================================================================

# Connection
IP = '0.0.0.0'
PORT = 12345
SENDER_LISTEN_PORT = 12346

# Audio settings (must match sender)
RATE = 48000
CHANNELS = 1
FORMAT = pyaudio.paInt16
FRAME_SIZE = 96  # 2ms at 48kHz
HEADER_SIZE = 12  # 4 (seq) + 8 (timestamp)
AUDIO_BYTES = FRAME_SIZE * 2  # 16-bit = 2 bytes per sample

# Setup socket
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
sock.bind((IP, PORT))

# Setup audio — list WASAPI devices
pa = pyaudio.PyAudio()

print("=== Available OUTPUT devices ===")
wasapi_outputs = []
for i in range(pa.get_device_count()):
    info = pa.get_device_info_by_index(i)
    if info['maxOutputChannels'] > 0:
        host = pa.get_host_api_info_by_index(info['hostApi'])['name']
        print(f"  [{i}] {info['name']} ({host})")
        if 'WASAPI' in host:
            wasapi_outputs.append(i)

print(f"\nWASAPI output devices: {wasapi_outputs}")
device_index = int(input("Select output device index: "))

# Try opening output
try:
    stream = pa.open(
        format=FORMAT,
        channels=CHANNELS,
        rate=RATE,
        output=True,
        output_device_index=device_index,
        frames_per_buffer=FRAME_SIZE,
    )
    print(f"\n[Receiver] Opened device [{device_index}]")
except Exception as e:
    print(f"Failed with FRAME_SIZE={FRAME_SIZE}: {e}")
    print("Trying with larger buffer...")
    FRAME_SIZE = 128
    AUDIO_BYTES = FRAME_SIZE * 2
    stream = pa.open(
        format=FORMAT,
        channels=CHANNELS,
        rate=RATE,
        output=True,
        output_device_index=device_index,
        frames_per_buffer=FRAME_SIZE,
    )
    print(f"[Receiver] Opened with FRAME_SIZE={FRAME_SIZE} ({FRAME_SIZE/RATE*1000:.1f}ms)")

print(f"[Receiver] Frame: {FRAME_SIZE} samples ({FRAME_SIZE/RATE*1000:.1f}ms)")
print(f"[Receiver] No jitter buffer — immediate playback")
print(f"[Receiver] Press Ctrl+C to stop\n")

packets_received = 0
last_seq = -1
latency_log = []

try:
    while True:
        data, addr = sock.recvfrom(HEADER_SIZE + AUDIO_BYTES + 64)

        # Unpack
        seq, t_captured = struct.unpack('<Id', data[:HEADER_SIZE])
        audio_data = data[HEADER_SIZE:]

        # Measure true end-to-end latency (same-machine only)
        t_before_play = time.perf_counter()
        one_way = (t_before_play - t_captured) * 1000

        # Play immediately
        stream.write(audio_data)

        # Could also measure after write:
        # t_after_play = time.perf_counter()
        # total = (t_after_play - t_captured) * 1000

        latency_log.append(one_way)
        packets_received += 1

        # Echo header back for sender's RTT measurement
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

stream.stop_stream()
stream.close()
pa.terminate()
sock.close()

print(f"\n--- Summary ---")
print(f"Packets received: {packets_received}")
if latency_log:
    avg = sum(latency_log) / len(latency_log)
    print(f"Capture → Pre-play — Avg: {avg:.2f}ms | Min: {min(latency_log):.2f}ms | Max: {max(latency_log):.2f}ms")
    print(f"\nNote: this does NOT include the output device buffer (~2-10ms additional)")
    print(f"True end-to-end = reported latency + output hardware buffer")
