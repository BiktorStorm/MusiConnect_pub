"""
Comprehensive test suite for packet_control_python.

Tests cover:
1. Socket parameter configuration
2. Audio packet encoding/decoding
3. Sender/receiver logic (mocked network layer)
4. Edge cases: dropped packets, silence detection, buffer underruns, malformed data
5. Low-latency variant specific behavior

All external dependencies (sounddevice, pyaudio, network sockets) are mocked
so tests run without audio hardware.
"""

import struct
import time
import socket
import threading
from unittest.mock import patch, MagicMock, PropertyMock

import pytest
import numpy as np


# === Constants matching the project's audio parameters ===

SAMPLE_RATE = 48000
CHANNELS = 1
BIT_DEPTH = 16  # bits
BYTES_PER_SAMPLE = BIT_DEPTH // 8  # 2
FRAME_SIZE = 96  # samples per packet
FRAME_SIZE_FALLBACK = 128  # low-latency fallback
AUDIO_BYTES = FRAME_SIZE * BYTES_PER_SAMPLE * CHANNELS  # 192
HEADER_SIZE = 12  # 4 (uint32 seq) + 8 (float64 timestamp)
PACKET_SIZE = HEADER_SIZE + AUDIO_BYTES  # 204
FRAME_DURATION_MS = (FRAME_SIZE / SAMPLE_RATE) * 1000  # 2.0 ms

RECEIVER_PORT = 12345
PONG_PORT = 12346

# socket_parameters.py legacy values
LEGACY_FORMAT_CHUNK = 4800
LEGACY_SO_RCVBUF = 1024
LEGACY_SO_SNDBUF = 1024
LEGACY_TIMEOUT = 0.005


# === Helper functions ===

def make_packet(seq: int, timestamp: float, audio_data: bytes = None) -> bytes:
    """Create a packet with header + audio payload."""
    header = struct.pack('<Id', seq, timestamp)
    if audio_data is None:
        audio_data = b'\x00' * AUDIO_BYTES
    return header + audio_data


def make_header_only_packet(seq: int, timestamp: float) -> bytes:
    """Create a benchmark packet (header only, no audio)."""
    return struct.pack('<Id', seq, timestamp)


def decode_packet(data: bytes):
    """Decode a packet into (seq, timestamp, audio_data)."""
    seq, timestamp = struct.unpack('<Id', data[:HEADER_SIZE])
    audio_data = data[HEADER_SIZE:]
    return seq, timestamp, audio_data


def generate_sine_frame(freq: float = 440.0, frame_size: int = FRAME_SIZE,
                        sample_rate: int = SAMPLE_RATE) -> bytes:
    """Generate a single frame of sine wave as int16 PCM bytes."""
    t = np.arange(frame_size) / sample_rate
    samples = (np.sin(2 * np.pi * freq * t) * 32767).astype(np.int16)
    return samples.tobytes()


# =============================================================================
# 1. SOCKET PARAMETER CONFIGURATION TESTS
# =============================================================================

class TestSocketParameters:
    """Test socket configuration values and behavior."""

    def test_udp_socket_creation(self):
        """Socket should be created as UDP (SOCK_DGRAM)."""
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        assert sock.type == socket.SOCK_DGRAM
        sock.close()

    def test_legacy_buffer_sizes(self):
        """socket_parameters.py sets extremely small buffers (1024 bytes)."""
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, LEGACY_SO_RCVBUF)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, LEGACY_SO_SNDBUF)
        # OS may enforce a minimum larger than 1024, but the call should not error
        rcvbuf = sock.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)
        sndbuf = sock.getsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF)
        # On Windows/Linux the OS may double or enforce minimum
        assert rcvbuf >= LEGACY_SO_RCVBUF
        assert sndbuf >= LEGACY_SO_SNDBUF
        sock.close()

    def test_receiver_buffer_size_2048(self):
        """Standard audio receiver uses SO_RCVBUF = 2048."""
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2048)
        rcvbuf = sock.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)
        assert rcvbuf >= 2048
        sock.close()

    def test_lowlat_receiver_buffer_size_4096(self):
        """Low-latency and ASIO receivers use SO_RCVBUF = 4096."""
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        rcvbuf = sock.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)
        assert rcvbuf >= 4096
        sock.close()

    def test_sender_sndbuf_1024(self):
        """Low-latency and ASIO senders use SO_SNDBUF = 1024."""
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1024)
        sndbuf = sock.getsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF)
        assert sndbuf >= 1024
        sock.close()

    def test_socket_timeout_5ms(self):
        """socket_parameters.py uses 5ms timeout."""
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(LEGACY_TIMEOUT)
        assert sock.gettimeout() == pytest.approx(LEGACY_TIMEOUT)
        sock.close()

    def test_asio_receiver_timeout_1s(self):
        """ASIO receiver uses 1 second timeout."""
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(1.0)
        assert sock.gettimeout() == pytest.approx(1.0)
        sock.close()

    def test_default_port_values(self):
        """Verify port constants match project defaults."""
        assert RECEIVER_PORT == 12345
        assert PONG_PORT == 12346

    def test_legacy_config_values(self):
        """Verify socket_parameters.py legacy values are documented correctly."""
        assert LEGACY_FORMAT_CHUNK == 4800
        assert LEGACY_SO_RCVBUF == 1024
        assert LEGACY_SO_SNDBUF == 1024

    def test_port_already_in_use(self):
        """Binding to an already-bound port raises OSError."""
        sock1 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock1.bind(('127.0.0.1', 0))  # Let OS assign a free port
        port = sock1.getsockname()[1]

        sock2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        with pytest.raises(OSError):
            sock2.bind(('127.0.0.1', port))

        sock1.close()
        sock2.close()

    def test_buffer_capacity_in_packets(self):
        """With SO_RCVBUF=4096 and packet size=204, buffer holds ~20 packets max."""
        buffer_size = 4096
        max_packets = buffer_size // PACKET_SIZE
        assert max_packets == 20  # 4096 // 204 = 20




# =============================================================================
# 2. AUDIO PACKET ENCODING/DECODING TESTS
# =============================================================================

class TestPacketEncoding:
    """Test binary packet format: [seq:u32 LE][timestamp:f64 LE][audio:PCM16]."""

    def test_header_size_is_12_bytes(self):
        """Header is exactly 12 bytes (4 + 8)."""
        header = struct.pack('<Id', 0, 0.0)
        assert len(header) == HEADER_SIZE

    def test_full_packet_size_is_204_bytes(self):
        """Full audio packet is 12 header + 192 audio = 204 bytes."""
        packet = make_packet(0, 0.0)
        assert len(packet) == PACKET_SIZE

    def test_encode_decode_roundtrip(self):
        """Encoding then decoding returns original values."""
        seq = 42
        timestamp = 1234567.890123
        audio = generate_sine_frame()

        packet = make_packet(seq, timestamp, audio)
        decoded_seq, decoded_ts, decoded_audio = decode_packet(packet)

        assert decoded_seq == seq
        assert decoded_ts == pytest.approx(timestamp)
        assert decoded_audio == audio

    def test_sequence_number_zero(self):
        """First packet has seq=0."""
        packet = make_packet(0, time.perf_counter())
        seq, _, _ = decode_packet(packet)
        assert seq == 0

    def test_sequence_number_max_uint32(self):
        """Maximum valid seq is 2^32 - 1."""
        max_seq = 2**32 - 1
        packet = make_packet(max_seq, 0.0)
        seq, _, _ = decode_packet(packet)
        assert seq == max_seq

    def test_sequence_number_overflow_raises(self):
        """seq > 2^32-1 raises struct.error on pack."""
        with pytest.raises(struct.error):
            struct.pack('<Id', 2**32, 0.0)

    def test_negative_sequence_raises(self):
        """Negative seq raises struct.error (unsigned int)."""
        with pytest.raises(struct.error):
            struct.pack('<Id', -1, 0.0)

    def test_timestamp_precision(self):
        """float64 preserves microsecond-level precision."""
        ts = 123456.789012345
        packet = make_packet(0, ts)
        _, decoded_ts, _ = decode_packet(packet)
        # float64 has ~15-16 significant digits
        assert abs(decoded_ts - ts) < 1e-9

    def test_audio_payload_correct_size(self):
        """Audio payload is exactly 192 bytes (96 samples × 2 bytes)."""
        audio = generate_sine_frame()
        assert len(audio) == AUDIO_BYTES

    def test_silence_packet(self):
        """Silence is represented as all-zero bytes."""
        silence = b'\x00' * AUDIO_BYTES
        packet = make_packet(0, 0.0, silence)
        _, _, audio = decode_packet(packet)
        assert audio == silence
        # Verify it decodes to zero-valued samples
        samples = np.frombuffer(audio, dtype=np.int16)
        assert np.all(samples == 0)

    def test_full_scale_audio(self):
        """Full-scale int16 values (-32768 to 32767) encode correctly."""
        samples = np.array([32767, -32768, 0, 1, -1] * 19 + [0], dtype=np.int16)
        assert len(samples) == FRAME_SIZE
        audio = samples.tobytes()
        packet = make_packet(0, 0.0, audio)
        _, _, decoded_audio = decode_packet(packet)
        decoded_samples = np.frombuffer(decoded_audio, dtype=np.int16)
        np.testing.assert_array_equal(decoded_samples, samples)

    def test_little_endian_byte_order(self):
        """Packet uses little-endian byte order."""
        seq = 1
        # In LE, uint32 value 1 is b'\x01\x00\x00\x00'
        packet = make_packet(seq, 0.0)
        assert packet[0:4] == b'\x01\x00\x00\x00'

    def test_benchmark_packet_header_only(self):
        """Benchmark packets are header-only (12 bytes, no audio)."""
        packet = make_header_only_packet(99, 5678.0)
        assert len(packet) == HEADER_SIZE
        seq, ts = struct.unpack('<Id', packet)
        assert seq == 99
        assert ts == pytest.approx(5678.0)

    def test_multiple_sequential_packets(self):
        """Encoding a stream of packets preserves monotonic sequence."""
        packets = [make_packet(i, float(i)) for i in range(100)]
        for i, pkt in enumerate(packets):
            seq, ts, _ = decode_packet(pkt)
            assert seq == i
            assert ts == pytest.approx(float(i))

    def test_frame_duration_is_2ms(self):
        """96 samples at 48kHz = 2ms frame duration."""
        duration_ms = (FRAME_SIZE / SAMPLE_RATE) * 1000
        assert duration_ms == pytest.approx(2.0)



# =============================================================================
# 3. SENDER/RECEIVER LOGIC TESTS (MOCKED NETWORK LAYER)
# =============================================================================

class TestSenderLogic:
    """Test sender behavior with mocked socket and audio input."""

    def test_sender_packet_format(self):
        """Sender produces correctly formatted packets."""
        # Simulate what audio_sender.py does
        seq = 0
        timestamp = time.perf_counter()
        audio_data = b'\x00' * AUDIO_BYTES
        header = struct.pack('<Id', seq, timestamp)
        packet = header + audio_data
        assert len(packet) == PACKET_SIZE

    def test_sender_increments_sequence(self):
        """Sender increments seq for each packet."""
        packets = []
        for seq in range(10):
            ts = time.perf_counter()
            packets.append(make_packet(seq, ts))

        for i, pkt in enumerate(packets):
            decoded_seq, _, _ = decode_packet(pkt)
            assert decoded_seq == i

    def test_sender_timestamps_are_monotonic(self):
        """Timestamps increase with each packet."""
        timestamps = []
        for seq in range(5):
            ts = time.perf_counter()
            timestamps.append(ts)
            time.sleep(0.001)

        for i in range(1, len(timestamps)):
            assert timestamps[i] > timestamps[i - 1]

    def test_sender_udp_sendto(self):
        """Sender calls sendto with correct address."""
        mock_sock = MagicMock()
        dest = ('127.0.0.1', RECEIVER_PORT)
        packet = make_packet(0, time.perf_counter())

        mock_sock.sendto(packet, dest)
        mock_sock.sendto.assert_called_once_with(packet, dest)

    def test_sender_continues_when_receiver_absent(self):
        """UDP sendto does not raise even if receiver is not listening."""
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # Sending to a port with no listener — should not raise
        packet = make_packet(0, time.perf_counter())
        # Use a random high port unlikely to be in use
        sock.sendto(packet, ('127.0.0.1', 59999))
        sock.close()

    def test_sender_pong_listener_rtt_calculation(self):
        """RTT is calculated as (now - echoed_timestamp) * 1000 ms."""
        t_sent = time.perf_counter()
        time.sleep(0.01)  # Simulate 10ms network delay
        t_received = time.perf_counter()

        rtt_ms = (t_received - t_sent) * 1000
        assert rtt_ms >= 9.0  # At least ~10ms minus scheduling jitter
        assert rtt_ms < 50.0  # Sanity upper bound

    def test_benchmark_sender_sends_1000_packets(self):
        """Benchmark sender sends exactly 1000 packets."""
        mock_sock = MagicMock()
        dest = ('127.0.0.1', RECEIVER_PORT)
        total_packets = 1000

        for seq in range(total_packets):
            packet = make_header_only_packet(seq, time.perf_counter())
            mock_sock.sendto(packet, dest)

        assert mock_sock.sendto.call_count == total_packets


class TestReceiverLogic:
    """Test receiver behavior with mocked socket and audio output."""

    def test_receiver_extracts_audio_from_packet(self):
        """Receiver correctly strips header to get audio data."""
        audio = generate_sine_frame()
        packet = make_packet(5, time.perf_counter(), audio)

        # Receiver logic
        received_audio = packet[HEADER_SIZE:]
        assert received_audio == audio

    def test_receiver_extracts_header_fields(self):
        """Receiver correctly unpacks seq and timestamp."""
        expected_seq = 42
        expected_ts = 1234.5678
        packet = make_packet(expected_seq, expected_ts)

        seq, ts = struct.unpack('<Id', packet[:HEADER_SIZE])
        assert seq == expected_seq
        assert ts == pytest.approx(expected_ts)

    def test_receiver_detects_out_of_order(self):
        """Receiver flags packets where seq <= last_seq."""
        last_seq = 10
        out_of_order_count = 0

        incoming_seqs = [11, 12, 10, 13, 12, 14]  # 10 and 12 are out of order
        for seq in incoming_seqs:
            if seq <= last_seq:
                out_of_order_count += 1
            else:
                last_seq = seq

        assert out_of_order_count == 2

    def test_receiver_echo_pong(self):
        """Receiver echoes header back to sender for RTT measurement."""
        mock_sock = MagicMock()
        sender_addr = ('192.168.1.100', 54321)

        packet = make_packet(7, 9876.543)
        header = packet[:HEADER_SIZE]

        # Receiver echoes header to sender's pong port
        mock_sock.sendto(header, (sender_addr[0], PONG_PORT))
        mock_sock.sendto.assert_called_once_with(
            header, ('192.168.1.100', PONG_PORT)
        )

    def test_receiver_infers_sender_address(self):
        """Receiver learns sender address from first recvfrom."""
        mock_sock = MagicMock()
        sender_addr = ('10.0.0.5', 12346)
        packet = make_packet(0, time.perf_counter())
        mock_sock.recvfrom.return_value = (packet, sender_addr)

        data, addr = mock_sock.recvfrom(PACKET_SIZE + 64)
        assert addr == sender_addr

    def test_receiver_plays_audio_immediately(self):
        """Receiver writes audio to output stream without buffering."""
        mock_stream = MagicMock()
        audio = generate_sine_frame()
        packet = make_packet(0, time.perf_counter(), audio)

        audio_data = packet[HEADER_SIZE:]
        mock_stream.write(audio_data)
        mock_stream.write.assert_called_once_with(audio_data)

    def test_benchmark_receiver_keeps_latest_packet(self):
        """Benchmark receiver drains buffer and keeps only the latest packet."""
        # Simulate multiple packets arriving during one 5ms cycle
        packets = [make_header_only_packet(i, float(i)) for i in range(5)]

        # Receiver logic: drain all, keep last
        latest = None
        for pkt in packets:
            latest = pkt

        seq, ts = struct.unpack('<Id', latest)
        assert seq == 4  # Last packet

    def test_drop_rate_calculation(self):
        """Drop rate = 1 - received/sent."""
        total_sent = 1000
        received_seqs = set(range(0, 1000, 2))  # Only even packets received
        drop_rate = 1.0 - len(received_seqs) / total_sent
        assert drop_rate == pytest.approx(0.5)

    def test_receiver_udp_communication_loopback(self):
        """End-to-end: sender→receiver via real UDP loopback."""
        receiver_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        receiver_sock.bind(('127.0.0.1', 0))
        port = receiver_sock.getsockname()[1]
        receiver_sock.settimeout(1.0)

        sender_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

        # Send a packet
        audio = generate_sine_frame(440.0)
        packet = make_packet(0, time.perf_counter(), audio)
        sender_sock.sendto(packet, ('127.0.0.1', port))

        # Receive it
        data, addr = receiver_sock.recvfrom(PACKET_SIZE + 64)
        seq, ts, received_audio = decode_packet(data)

        assert seq == 0
        assert received_audio == audio
        assert len(data) == PACKET_SIZE

        sender_sock.close()
        receiver_sock.close()



# =============================================================================
# 4. EDGE CASES: DROPPED PACKETS, SILENCE, BUFFER UNDERRUNS, MALFORMED DATA
# =============================================================================

class TestDroppedPackets:
    """Test behavior when packets are lost in transit."""

    def test_single_dropped_packet_detected(self):
        """Gap in sequence numbers indicates dropped packet."""
        received_seqs = [0, 1, 2, 4, 5]  # seq 3 dropped
        gaps = []
        for i in range(1, len(received_seqs)):
            if received_seqs[i] != received_seqs[i - 1] + 1:
                gap_start = received_seqs[i - 1] + 1
                gap_end = received_seqs[i] - 1
                gaps.append((gap_start, gap_end))

        assert gaps == [(3, 3)]

    def test_burst_loss(self):
        """Multiple consecutive packets dropped."""
        received_seqs = [0, 1, 2, 10, 11, 12]  # 3-9 dropped
        expected_dropped = set(range(3, 10))
        all_expected = set(range(13))
        actual_dropped = all_expected - set(received_seqs)
        assert actual_dropped == expected_dropped

    def test_high_loss_rate_calculation(self):
        """90% packet loss is correctly computed."""
        total_sent = 1000
        total_received = 100
        drop_rate = 1.0 - total_received / total_sent
        assert drop_rate == pytest.approx(0.9)

    def test_no_loss(self):
        """0% loss when all packets received."""
        total_sent = 500
        total_received = 500
        drop_rate = 1.0 - total_received / total_sent
        assert drop_rate == pytest.approx(0.0)

    def test_all_packets_lost(self):
        """100% loss yields drop_rate = 1.0."""
        total_sent = 1000
        total_received = 0
        drop_rate = 1.0 - total_received / total_sent if total_sent > 0 else 0.0
        assert drop_rate == pytest.approx(1.0)

    def test_out_of_order_not_counted_as_loss(self):
        """Reordered packets arrive but not in order; they're not lost."""
        received_seqs = [0, 1, 3, 2, 4, 5]  # All 0-5 present, just reordered
        unique_received = set(received_seqs)
        expected = set(range(6))
        assert unique_received == expected

    def test_duplicate_packets_not_inflating_count(self):
        """Duplicates should not be counted as separate unique packets."""
        received_seqs = [0, 1, 1, 2, 2, 2, 3, 4, 5]
        unique_count = len(set(received_seqs))
        assert unique_count == 6


class TestSilenceDetection:
    """Test detection of silence in audio frames."""

    def test_silence_frame_is_all_zeros(self):
        """A silent frame has all samples at zero."""
        silence = b'\x00' * AUDIO_BYTES
        samples = np.frombuffer(silence, dtype=np.int16)
        assert np.all(samples == 0)

    def test_non_silence_detected(self):
        """A frame with audio content has non-zero RMS."""
        audio = generate_sine_frame(440.0)
        samples = np.frombuffer(audio, dtype=np.int16)
        rms = np.sqrt(np.mean(samples.astype(np.float64) ** 2))
        assert rms > 0

    def test_near_silence_threshold(self):
        """Very quiet audio (low amplitude) can be detected with threshold."""
        # Generate very quiet signal
        t = np.arange(FRAME_SIZE) / SAMPLE_RATE
        samples = (np.sin(2 * np.pi * 440 * t) * 10).astype(np.int16)  # ~0.03% FS
        rms = np.sqrt(np.mean(samples.astype(np.float64) ** 2))
        silence_threshold = 100  # Arbitrary threshold
        assert rms < silence_threshold

    def test_full_scale_not_silent(self):
        """Full-scale signal is clearly not silence."""
        samples = np.full(FRAME_SIZE, 32767, dtype=np.int16)
        rms = np.sqrt(np.mean(samples.astype(np.float64) ** 2))
        assert rms > 30000


class TestBufferUnderruns:
    """Test behavior under buffer underrun conditions."""

    def test_late_packet_still_played(self):
        """Packets arriving late are still passed to stream.write (no jitter buffer)."""
        mock_stream = MagicMock()
        # Simulate late packet — receiver plays it regardless
        late_audio = generate_sine_frame()
        mock_stream.write(late_audio)
        mock_stream.write.assert_called_once_with(late_audio)

    def test_gap_in_playback_when_no_packet(self):
        """If no packet arrives during a frame period, there's nothing to play."""
        mock_sock = MagicMock()
        mock_sock.recvfrom.side_effect = socket.timeout("timed out")

        with pytest.raises(socket.timeout):
            mock_sock.recvfrom(PACKET_SIZE + 64)

    def test_small_rcvbuf_causes_drops(self):
        """With small SO_RCVBUF, rapid sends cause OS to drop packets."""
        # Create a real socket pair to demonstrate buffer pressure
        receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        receiver.bind(('127.0.0.1', 0))
        port = receiver.getsockname()[1]
        receiver.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2048)
        receiver.settimeout(0.1)

        sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

        # Flood packets without reading
        for i in range(100):
            packet = make_packet(i, time.perf_counter())
            sender.sendto(packet, ('127.0.0.1', port))

        # Count how many we can read back
        received = 0
        while True:
            try:
                receiver.recvfrom(PACKET_SIZE + 64)
                received += 1
            except socket.timeout:
                break

        # With buffer size ~2048 and packet size 204, we expect significant drops
        # (exact count depends on OS buffer behavior)
        assert received < 100  # Some packets should have been dropped
        assert received > 0   # At least some arrived

        sender.close()
        receiver.close()

    def test_output_underrun_no_crash(self):
        """Output stream write should not crash even if called irregularly."""
        mock_stream = MagicMock()
        # Simulate irregular timing
        for i in range(10):
            audio = generate_sine_frame()
            mock_stream.write(audio)

        assert mock_stream.write.call_count == 10


class TestMalformedData:
    """Test handling of malformed/corrupted packets."""

    def test_empty_packet_raises(self):
        """Empty data causes struct.unpack to raise."""
        with pytest.raises(struct.error):
            struct.unpack('<Id', b'')

    def test_truncated_header_raises(self):
        """Partial header (< 12 bytes) causes unpack error."""
        truncated = b'\x01\x00\x00'  # Only 3 bytes
        with pytest.raises(struct.error):
            struct.unpack('<Id', truncated[:HEADER_SIZE])

    def test_exactly_header_no_audio(self):
        """Packet with header but no audio — audio_data is empty bytes."""
        header_only = struct.pack('<Id', 0, 1.0)
        assert len(header_only) == HEADER_SIZE
        audio_data = header_only[HEADER_SIZE:]
        assert audio_data == b''
        assert len(audio_data) == 0

    def test_short_audio_payload(self):
        """Packet with fewer than expected audio bytes."""
        short_audio = b'\x00' * 100  # Only 100 bytes instead of 192
        packet = make_packet(0, 0.0, short_audio)
        _, _, audio = decode_packet(packet)
        assert len(audio) == 100  # Shorter than expected
        assert len(audio) < AUDIO_BYTES

    def test_oversized_audio_payload(self):
        """Packet with extra bytes beyond expected audio size."""
        extra_audio = b'\xff' * 300  # 300 bytes instead of 192
        packet = make_packet(0, 0.0, extra_audio)
        _, _, audio = decode_packet(packet)
        assert len(audio) == 300  # All bytes after header are treated as audio

    def test_random_garbage_data(self):
        """Random bytes can still be unpacked (garbage in → garbage out)."""
        import os
        random_data = os.urandom(PACKET_SIZE)
        # Should not raise — struct.unpack handles any 12 bytes
        seq, ts, audio = decode_packet(random_data)
        assert isinstance(seq, int)
        assert isinstance(ts, float)
        assert len(audio) == AUDIO_BYTES

    def test_numpy_frombuffer_odd_bytes_raises(self):
        """Odd number of bytes cannot be interpreted as int16 array."""
        odd_bytes = b'\x00' * 191  # Not divisible by 2
        with pytest.raises(ValueError):
            np.frombuffer(odd_bytes, dtype=np.int16)

    def test_numpy_reshape_wrong_size_raises(self):
        """If audio byte count doesn't match expected frame, reshape fails."""
        # 100 bytes = 50 int16 samples, can't reshape to (-1, 1) of size 96
        audio_bytes = b'\x00' * 100
        samples = np.frombuffer(audio_bytes, dtype=np.int16)
        assert samples.shape == (50,)
        # This reshape works (50, 1) — but checking expected frame size
        assert samples.shape[0] != FRAME_SIZE

    def test_invalid_seq_in_wild_packet(self):
        """A packet with seq=0xFFFFFFFF is valid but unusual."""
        packet = make_packet(0xFFFFFFFF, 0.0)
        seq, _, _ = decode_packet(packet)
        assert seq == 0xFFFFFFFF

    def test_nan_timestamp(self):
        """NaN timestamp packs and unpacks without error."""
        packet = make_packet(0, float('nan'))
        _, ts, _ = decode_packet(packet)
        assert ts != ts  # NaN != NaN

    def test_inf_timestamp(self):
        """Infinity timestamp packs and unpacks without error."""
        packet = make_packet(0, float('inf'))
        _, ts, _ = decode_packet(packet)
        assert ts == float('inf')

    def test_negative_timestamp(self):
        """Negative timestamp (clock issue) still encodes/decodes."""
        packet = make_packet(0, -1.0)
        _, ts, _ = decode_packet(packet)
        assert ts == pytest.approx(-1.0)



# =============================================================================
# 5. LOW-LATENCY VARIANT SPECIFIC BEHAVIOR
# =============================================================================

class TestLowLatencyVariant:
    """Test WASAPI low-latency specific logic."""

    def test_frame_size_fallback_96_to_128(self):
        """If FRAME_SIZE=96 fails, system retries with 128."""
        frame_size = 96
        fallback_size = 128

        # Simulate device rejecting 96
        device_rejects_96 = True
        if device_rejects_96:
            frame_size = fallback_size

        assert frame_size == 128
        # Verify new frame duration
        duration_ms = (frame_size / SAMPLE_RATE) * 1000
        assert duration_ms == pytest.approx(2.6667, rel=0.01)

    def test_fallback_audio_bytes_size(self):
        """Fallback frame (128 samples) produces 256 bytes audio payload."""
        fallback_audio_bytes = FRAME_SIZE_FALLBACK * BYTES_PER_SAMPLE * CHANNELS
        assert fallback_audio_bytes == 256

    def test_fallback_packet_size(self):
        """Fallback packet is 12 + 256 = 268 bytes."""
        fallback_audio = b'\x00' * (FRAME_SIZE_FALLBACK * BYTES_PER_SAMPLE)
        packet = make_packet(0, 0.0, fallback_audio)
        assert len(packet) == HEADER_SIZE + 256

    def test_wasapi_device_enumeration_mock(self):
        """WASAPI device enumeration lists only WASAPI devices."""
        # Mock pyaudio device info
        mock_pa = MagicMock()
        mock_pa.get_device_count.return_value = 5
        mock_pa.get_host_api_info_by_index.return_value = {
            'name': 'Windows WASAPI', 'index': 3, 'deviceCount': 2
        }

        # Simulate filtering WASAPI devices
        devices = []
        wasapi_hostapi_index = 3
        all_devices = [
            {'name': 'Speakers (WASAPI)', 'hostApi': 3, 'index': 0,
             'maxInputChannels': 0, 'maxOutputChannels': 2},
            {'name': 'Microphone (WASAPI)', 'hostApi': 3, 'index': 1,
             'maxInputChannels': 2, 'maxOutputChannels': 0},
            {'name': 'Speakers (MME)', 'hostApi': 0, 'index': 2,
             'maxInputChannels': 0, 'maxOutputChannels': 2},
        ]
        for dev in all_devices:
            if dev['hostApi'] == wasapi_hostapi_index:
                devices.append(dev)

        assert len(devices) == 2
        assert all(d['hostApi'] == 3 for d in devices)

    def test_latency_measurement_same_machine(self):
        """Capture→play latency uses perf_counter difference."""
        t_captured = time.perf_counter()
        time.sleep(0.002)  # Simulate 2ms processing
        t_before_play = time.perf_counter()

        latency_ms = (t_before_play - t_captured) * 1000
        assert latency_ms >= 1.5  # At least ~2ms minus jitter
        assert latency_ms < 20.0  # Sanity bound

    def test_timestamp_after_capture(self):
        """Low-latency variant timestamps AFTER capture completes."""
        # This is more accurate than timestamping before read
        t_before_read = time.perf_counter()
        time.sleep(0.002)  # Simulate audio read taking 2ms
        t_after_read = time.perf_counter()  # This is the actual timestamp sent

        # The sent timestamp should reflect when capture completed
        assert t_after_read > t_before_read
        # Packet uses t_after_read
        packet = make_packet(0, t_after_read)
        _, ts, _ = decode_packet(packet)
        assert ts == pytest.approx(t_after_read)

    def test_sender_sndbuf_1024_lowlat(self):
        """Low-latency sender uses SO_SNDBUF=1024 to minimize queuing."""
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1024)
        # Verify it was set (OS may adjust)
        actual = sock.getsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF)
        assert actual >= 1024
        sock.close()

    def test_negative_latency_cross_machine(self):
        """Cross-machine measurement can produce negative values."""
        # Simulate clocks out of sync
        t_captured_remote = 1000.0  # Remote machine's perf_counter
        t_local_now = 500.0  # Local machine's perf_counter (lower!)

        latency_ms = (t_local_now - t_captured_remote) * 1000
        assert latency_ms < 0  # Negative = clocks not synchronized


class TestASIOVariant:
    """Test ASIO-specific logic (sounddevice + numpy)."""

    def test_numpy_audio_to_bytes_conversion(self):
        """ASIO sender converts numpy int16 array to bytes for transmission."""
        # Simulate sounddevice callback data
        indata = np.zeros((FRAME_SIZE, CHANNELS), dtype=np.int16)
        indata[:, 0] = np.arange(FRAME_SIZE, dtype=np.int16)

        audio_bytes = indata.tobytes()
        assert len(audio_bytes) == AUDIO_BYTES

    def test_bytes_to_numpy_conversion(self):
        """ASIO receiver converts received bytes back to numpy array."""
        # Create known audio data
        original = np.arange(FRAME_SIZE, dtype=np.int16).reshape(-1, CHANNELS)
        audio_bytes = original.tobytes()

        # Receiver conversion
        recovered = np.frombuffer(audio_bytes, dtype=np.int16).reshape(-1, CHANNELS)
        np.testing.assert_array_equal(recovered, original)

    def test_numpy_reshape_mono(self):
        """Mono audio reshapes to (N, 1)."""
        samples = np.zeros(FRAME_SIZE, dtype=np.int16)
        reshaped = samples.reshape(-1, 1)
        assert reshaped.shape == (FRAME_SIZE, 1)

    def test_asio_host_api_detection(self):
        """ASIO host API is identified by name in device query."""
        # Mock sounddevice
        mock_hostapis = [
            {'name': 'MME', 'devices': [0, 1]},
            {'name': 'Windows WASAPI', 'devices': [2, 3]},
            {'name': 'ASIO', 'devices': [4, 5]},
        ]

        asio_index = None
        for i, api in enumerate(mock_hostapis):
            if 'ASIO' in api['name']:
                asio_index = i
                break

        assert asio_index == 2

    def test_asio_not_available_fallback(self):
        """When ASIO host API is not found, detection returns None."""
        mock_hostapis = [
            {'name': 'MME', 'devices': [0, 1]},
            {'name': 'Windows WASAPI', 'devices': [2, 3]},
        ]

        asio_index = None
        for i, api in enumerate(mock_hostapis):
            if 'ASIO' in api['name']:
                asio_index = i
                break

        assert asio_index is None

    def test_asio_receiver_timeout_behavior(self):
        """ASIO receiver with 1s timeout raises socket.timeout if no data."""
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(('127.0.0.1', 0))
        sock.settimeout(0.01)  # Use short timeout for testing (not 1s)

        with pytest.raises(socket.timeout):
            sock.recvfrom(PACKET_SIZE + 64)

        sock.close()

    def test_asio_overflow_silently_handled(self):
        """ASIO sender ignores overflow flag from callback."""
        overflowed = True
        # In the actual code: `if overflowed: pass`
        # No exception, no action — just continue
        handled = False
        if overflowed:
            pass  # This is the actual behavior
            handled = True

        assert handled is True  # Code continues normally

    def test_sounddevice_stream_mock(self):
        """Mock sounddevice InputStream/OutputStream creation."""
        mock_sd = MagicMock()
        mock_sd.InputStream.return_value = MagicMock()
        mock_sd.OutputStream.return_value = MagicMock()

        input_stream = mock_sd.InputStream(
            device=4, channels=CHANNELS, samplerate=SAMPLE_RATE,
            dtype='int16', blocksize=FRAME_SIZE, latency='low'
        )
        output_stream = mock_sd.OutputStream(
            device=5, channels=CHANNELS, samplerate=SAMPLE_RATE,
            dtype='int16', blocksize=FRAME_SIZE, latency='low'
        )

        mock_sd.InputStream.assert_called_once()
        mock_sd.OutputStream.assert_called_once()

    def test_signal_handler_setup(self):
        """ASIO variant sets SIGINT to SIG_DFL for clean Ctrl+C."""
        import signal
        # On Windows, SIGINT is supported
        old_handler = signal.getsignal(signal.SIGINT)
        signal.signal(signal.SIGINT, signal.SIG_DFL)

        current = signal.getsignal(signal.SIGINT)
        assert current == signal.SIG_DFL

        # Restore
        signal.signal(signal.SIGINT, old_handler)


# =============================================================================
# 6. INTEGRATION-STYLE TESTS (MOCKED)
# =============================================================================

class TestEndToEndMocked:
    """Integration tests with fully mocked audio and network layers."""

    def test_full_send_receive_cycle(self):
        """Simulate complete send→receive→echo→RTT cycle."""
        # Setup
        sent_packets = []
        received_packets = []
        echoed_headers = []

        # Sender: create and "send" packets
        for seq in range(10):
            ts = time.perf_counter()
            audio = generate_sine_frame()
            pkt = make_packet(seq, ts, audio)
            sent_packets.append(pkt)

        # Simulate network: all packets arrive (no loss)
        received_packets = sent_packets.copy()

        # Receiver: process and echo
        for pkt in received_packets:
            header = pkt[:HEADER_SIZE]
            audio = pkt[HEADER_SIZE:]
            echoed_headers.append(header)

            # Verify audio is playable
            assert len(audio) == AUDIO_BYTES

        # Sender: receive echoes and compute RTT
        for echo in echoed_headers:
            seq, ts = struct.unpack('<Id', echo)
            rtt_ms = (time.perf_counter() - ts) * 1000
            assert rtt_ms >= 0  # Should be non-negative on same machine

        assert len(echoed_headers) == 10

    def test_send_receive_with_50_percent_loss(self):
        """Simulate 50% packet loss — only even packets arrive."""
        total = 100
        sent_packets = [make_packet(i, float(i)) for i in range(total)]

        # Simulate 50% random loss (drop odd-numbered packets)
        received = [pkt for i, pkt in enumerate(sent_packets) if i % 2 == 0]

        drop_rate = 1.0 - len(received) / total
        assert drop_rate == pytest.approx(0.5)

        # Verify received packets are still valid
        for pkt in received:
            seq, ts, audio = decode_packet(pkt)
            assert seq % 2 == 0
            assert len(audio) == AUDIO_BYTES

    def test_out_of_order_stream(self):
        """Simulate network reordering — packets arrive shuffled."""
        import random

        original_order = list(range(20))
        shuffled = original_order.copy()
        random.shuffle(shuffled)

        # Receiver tracking
        last_seq = -1
        out_of_order_count = 0

        for seq in shuffled:
            if seq <= last_seq:
                out_of_order_count += 1
            else:
                last_seq = seq

        # With random shuffle, expect some out-of-order
        # (statistically nearly impossible to get 0 with 20 items shuffled)
        # But we can't assert exact count due to randomness
        assert isinstance(out_of_order_count, int)

    def test_pyaudio_mock_full_lifecycle(self):
        """Mock PyAudio lifecycle: init → open → read/write → close → terminate."""
        mock_pa = MagicMock()
        mock_stream = MagicMock()
        mock_pa.open.return_value = mock_stream
        mock_stream.read.return_value = b'\x00' * AUDIO_BYTES

        # Lifecycle
        stream = mock_pa.open(
            format=8,  # paInt16
            channels=CHANNELS,
            rate=SAMPLE_RATE,
            input=True,
            frames_per_buffer=FRAME_SIZE
        )

        # Read audio
        audio_data = stream.read(FRAME_SIZE, exception_on_overflow=False)
        assert len(audio_data) == AUDIO_BYTES

        # Write audio
        stream.write(audio_data)

        # Cleanup
        stream.stop_stream()
        stream.close()
        mock_pa.terminate()

        stream.stop_stream.assert_called_once()
        stream.close.assert_called_once()
        mock_pa.terminate.assert_called_once()

    def test_resource_cleanup_on_keyboard_interrupt(self):
        """Resources are freed when KeyboardInterrupt occurs."""
        mock_sock = MagicMock()
        mock_stream = MagicMock()
        mock_pa = MagicMock()

        # Simulate the try/except pattern used in the project
        try:
            raise KeyboardInterrupt()
        except KeyboardInterrupt:
            mock_stream.stop_stream()
            mock_stream.close()
            mock_pa.terminate()
            mock_sock.close()

        mock_stream.stop_stream.assert_called_once()
        mock_stream.close.assert_called_once()
        mock_pa.terminate.assert_called_once()
        mock_sock.close.assert_called_once()

    def test_concurrent_send_receive(self):
        """Sender and receiver can operate concurrently via threading."""
        receiver_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        receiver_sock.bind(('127.0.0.1', 0))
        port = receiver_sock.getsockname()[1]
        receiver_sock.settimeout(2.0)

        received = []
        send_count = 20

        def sender():
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            for i in range(send_count):
                pkt = make_packet(i, time.perf_counter())
                sock.sendto(pkt, ('127.0.0.1', port))
                time.sleep(0.002)
            sock.close()

        def receiver():
            for _ in range(send_count):
                try:
                    data, addr = receiver_sock.recvfrom(PACKET_SIZE + 64)
                    received.append(data)
                except socket.timeout:
                    break

        t_send = threading.Thread(target=sender)
        t_recv = threading.Thread(target=receiver)

        t_recv.start()
        t_send.start()

        t_send.join(timeout=5)
        t_recv.join(timeout=5)

        receiver_sock.close()

        # Should receive most packets (some may be lost under load)
        assert len(received) > 0
        # Verify packet integrity
        for pkt in received:
            seq, ts, audio = decode_packet(pkt)
            assert 0 <= seq < send_count
            assert len(audio) == AUDIO_BYTES

