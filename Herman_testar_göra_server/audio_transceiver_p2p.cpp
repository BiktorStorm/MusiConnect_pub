// =============================================================================
// LOW-LATENCY AUDIO TRANSCEIVER (P2P) — C++ / PortAudio / UDP + signaling
//
// This is audio_transceiver.cpp wired to the MusiConnect signaling client:
//   1. Register in a room on the signaling server and hole-punch to the peer.
//   2. Take the resolved peer endpoint (ip:port).
//   3. Re-bind a single UDP socket on the SAME local port (SO_REUSEADDR) so the
//      hole-punched NAT mapping is reused, then stream audio directly to the peer.
//
// The signaling server never carries audio; it only introduces the two peers.
//
// Usage:
//   audio_transceiver_p2p <server_ip> <room_code> [local_port=12345] [server_port=5000]
//
// Build (MinGW), all on one line:
//   g++ -std=c++17 -O2 audio_transceiver_p2p.cpp signaling_client.cpp -o audio_transceiver_p2p.exe -lws2_32 -lportaudio
//
// Packet format (matches audio_transceiver.cpp / the Python version):
//   [seq: uint32_t (4B)] [timestamp: double (8B)] [PCM int16 interleaved...]
// A bare 12-byte header (no PCM) is an RTT echo.
// =============================================================================

// signaling_client.h pulls in signaling_protocol.h, which provides the
// cross-platform socket glue (socket_t, INVALID_SOCK, CLOSE_SOCKET,
// platform_socket_init/cleanup, set_nonblocking) and winsock headers. Include
// it FIRST so <winsock2.h> is seen before anything that might pull <windows.h>.
#include "signaling_client.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <string>
#include <chrono>
#include <algorithm>
#include <numeric>

#include <portaudio.h>

// =============================================================================
// PLATFORM: iOS Audio Session (no-op elsewhere)
// =============================================================================
#if defined(__APPLE__)
  #include <TargetConditionals.h>
  #if TARGET_OS_IPHONE
    extern "C" void ios_configure_audio_session();
  #else
    static void ios_configure_audio_session() {}
  #endif
#else
  static void ios_configure_audio_session() {}
#endif

// =============================================================================
// SIGNAL HANDLING (clean Ctrl+C shutdown)
// =============================================================================
#include <csignal>
static std::atomic<bool> g_running{true};

static void signal_handler(int /*sig*/) {
    g_running.store(false);
}

// =============================================================================
// CONSTANTS (must match on both peers)
// =============================================================================
static const int RATE = 48000;
static const int CHANNELS = 2;
static const int FRAME_SIZE = 96;         // 2ms @ 48kHz
static const int HEADER_SIZE = 12;        // 4 (seq) + 8 (timestamp)
static const int JITTER_MS = 25;
static const int MAX_JITTER_MS = 120;
static const int MAX_PACKET = 4096;
static const uint16_t DEFAULT_LOCAL_PORT  = 12345;
static const uint16_t DEFAULT_SERVER_PORT = 5000;

// =============================================================================
// HIGH-RESOLUTION TIMER
// =============================================================================
static double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// =============================================================================
// JITTER BUFFER
// =============================================================================
class JitterBuffer {
public:
    JitterBuffer(int channels, int rate, int target_ms, int max_ms)
        : channels_(channels),
          target_frames_(rate * target_ms / 1000),
          max_frames_(rate * max_ms / 1000),
          filling_(true), underruns_(0), overflows_(0) {}

    void push(const int16_t* data, int frames) {
        std::lock_guard<std::mutex> lock(mtx_);
        size_t samples = static_cast<size_t>(frames * channels_);
        buf_.insert(buf_.end(), data, data + samples);

        int total_frames = static_cast<int>(buf_.size()) / channels_;
        if (total_frames > max_frames_) {
            int drop_frames = total_frames - max_frames_;
            buf_.erase(buf_.begin(), buf_.begin() + drop_frames * channels_);
            overflows_++;
        }
    }

    void pull(int16_t* out, int frames) {
        std::lock_guard<std::mutex> lock(mtx_);
        int total_frames = static_cast<int>(buf_.size()) / channels_;

        if (filling_) {
            if (total_frames < target_frames_) {
                std::memset(out, 0, frames * channels_ * sizeof(int16_t));
                return;
            }
            filling_ = false;
        }

        if (total_frames >= frames) {
            size_t samples = static_cast<size_t>(frames * channels_);
            std::memcpy(out, buf_.data(), samples * sizeof(int16_t));
            buf_.erase(buf_.begin(), buf_.begin() + samples);
        } else {
            // Underrun
            std::memset(out, 0, frames * channels_ * sizeof(int16_t));
            size_t have = buf_.size();
            if (have > 0) {
                std::memcpy(out, buf_.data(), have * sizeof(int16_t));
            }
            buf_.clear();
            underruns_++;
            filling_ = true;
        }
    }

    int fill_frames() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return static_cast<int>(buf_.size()) / channels_;
    }

    int underruns() const { return underruns_.load(); }
    int overflows() const { return overflows_.load(); }

private:
    int channels_;
    int target_frames_;
    int max_frames_;
    bool filling_;
    std::atomic<int> underruns_;
    std::atomic<int> overflows_;
    mutable std::mutex mtx_;
    std::vector<int16_t> buf_;
};

// =============================================================================
// GLOBAL STATE
// =============================================================================
// Single UDP socket bound to the hole-punched local port. Used for both TX and
// RX of audio (and RTT echoes) so all traffic uses the punched NAT mapping.
static socket_t g_sock = INVALID_SOCK;
static sockaddr_in g_remote_addr{};

static JitterBuffer* g_jitter = nullptr;

// Metrics
static std::atomic<uint32_t> g_tx_seq{0};
static std::atomic<float> g_tx_peak{0.0f};
static std::atomic<float> g_rx_peak{0.0f};
static std::atomic<uint32_t> g_rx_packets{0};
static std::atomic<float> g_last_one_way{0.0f};

// RTT tracking
static std::mutex g_rtt_mtx;
static std::vector<double> g_rtt_list;

// =============================================================================
// PORTAUDIO CALLBACKS
// =============================================================================

// Capture callback — sends audio over UDP immediately (single socket)
static int capture_callback(const void* input, void* /*output*/,
                            unsigned long frameCount,
                            const PaStreamCallbackTimeInfo* /*timeInfo*/,
                            PaStreamCallbackFlags /*flags*/,
                            void* /*userData*/)
{
    const int16_t* in = static_cast<const int16_t*>(input);
    if (!in) return paContinue;

    uint32_t seq = g_tx_seq.fetch_add(1);
    double t = now_seconds();

    // Build packet: header + PCM
    uint8_t packet[HEADER_SIZE + FRAME_SIZE * CHANNELS * 2];
    std::memcpy(packet, &seq, 4);
    std::memcpy(packet + 4, &t, 8);
    size_t audio_bytes = frameCount * CHANNELS * sizeof(int16_t);
    std::memcpy(packet + HEADER_SIZE, in, audio_bytes);

    sendto(g_sock, reinterpret_cast<const char*>(packet),
           static_cast<int>(HEADER_SIZE + audio_bytes), 0,
           reinterpret_cast<sockaddr*>(&g_remote_addr), sizeof(g_remote_addr));

    // Peak meter
    float peak = 0.0f;
    for (unsigned long i = 0; i < frameCount * CHANNELS; i++) {
        float v = std::abs(static_cast<float>(in[i]));
        if (v > peak) peak = v;
    }
    g_tx_peak.store(peak / 32768.0f);

    return paContinue;
}

// Playout callback — pulls from jitter buffer
static int playout_callback(const void* /*input*/, void* output,
                            unsigned long frameCount,
                            const PaStreamCallbackTimeInfo* /*timeInfo*/,
                            PaStreamCallbackFlags /*flags*/,
                            void* /*userData*/)
{
    int16_t* out = static_cast<int16_t*>(output);
    g_jitter->pull(out, static_cast<int>(frameCount));

    // Peak meter
    float peak = 0.0f;
    for (unsigned long i = 0; i < frameCount * CHANNELS; i++) {
        float v = std::abs(static_cast<float>(out[i]));
        if (v > peak) peak = v;
    }
    g_rx_peak.store(peak / 32768.0f);

    return paContinue;
}

// =============================================================================
// NETWORK RECEIVE THREAD (single socket: audio + RTT echoes)
// =============================================================================
static void net_rx_loop() {
    uint8_t buf[MAX_PACKET];
    sockaddr_in sender_addr{};
    socklen_t sender_len = sizeof(sender_addr);

    // Timeout so the thread can notice shutdown.
#ifdef _WIN32
    DWORD timeout_ms = 500;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 500000;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif

    while (g_running.load()) {
        sender_len = sizeof(sender_addr);
        int n = recvfrom(g_sock, reinterpret_cast<char*>(buf), MAX_PACKET, 0,
                         reinterpret_cast<sockaddr*>(&sender_addr), &sender_len);
        if (n <= 0) continue;                    // timeout / error

        if (n == HEADER_SIZE) {
            // Bare header = RTT echo returning to us. Measure round trip.
            double t_sent;
            std::memcpy(&t_sent, buf + 4, 8);
            double rtt = (now_seconds() - t_sent) * 1000.0;
            std::lock_guard<std::mutex> lock(g_rtt_mtx);
            g_rtt_list.push_back(rtt);
            continue;
        }
        if (n < HEADER_SIZE) continue;           // stray small packet (e.g. late PUNCH)

        // --- audio packet ---
        uint32_t seq;
        double t_captured;
        std::memcpy(&seq, buf, 4);
        std::memcpy(&t_captured, buf + 4, 8);

        int audio_bytes = n - HEADER_SIZE;
        int total_samples = audio_bytes / static_cast<int>(sizeof(int16_t));
        if (total_samples % CHANNELS != 0) continue; // malformed

        int frames = total_samples / CHANNELS;
        g_jitter->push(reinterpret_cast<int16_t*>(buf + HEADER_SIZE), frames);

        g_last_one_way.store(static_cast<float>((now_seconds() - t_captured) * 1000.0));
        g_rx_packets.fetch_add(1);

        // Echo the bare header straight back to the sender for their RTT calc.
        sendto(g_sock, reinterpret_cast<const char*>(buf), HEADER_SIZE, 0,
               reinterpret_cast<sockaddr*>(&sender_addr), sender_len);
    }
}

// =============================================================================
// DEVICE LISTING HELPERS
// =============================================================================
static void list_devices() {
    int count = Pa_GetDeviceCount();
    printf("\n=== Available Audio Devices ===\n");
    for (int i = 0; i < count; i++) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        const PaHostApiInfo* api = Pa_GetHostApiInfo(info->hostApi);
        printf("  [%2d] %-45s  in=%d out=%d  (%s)\n",
               i, info->name, info->maxInputChannels, info->maxOutputChannels,
               api->name);
    }
    printf("\n  Default input:  %d\n", Pa_GetDefaultInputDevice());
    printf("  Default output: %d\n\n", Pa_GetDefaultOutputDevice());
}

static int prompt_device(const char* direction, int default_dev) {
    char line[64];
    printf("Select %s device [Enter = %d]: ", direction, default_dev);
    fflush(stdout);
    if (fgets(line, sizeof(line), stdin)) {
        int len = static_cast<int>(strlen(line));
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len > 0) return atoi(line);
    }
    return default_dev;
}

static const char* resolve_result_str(ResolveResult r) {
    switch (r) {
        case ResolveResult::Success:           return "Success";
        case ResolveResult::ServerUnreachable: return "ServerUnreachable";
        case ResolveResult::RoomFull:          return "RoomFull";
        case ResolveResult::Timeout:           return "Timeout";
        case ResolveResult::SocketError:       return "SocketError";
        case ResolveResult::VersionMismatch:   return "VersionMismatch";
    }
    return "Unknown";
}

// =============================================================================
// MAIN
// =============================================================================
int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr,
            "Usage: %s <server_ip> <room_code> [local_port=%u] [server_port=%u]\n",
            argv[0], DEFAULT_LOCAL_PORT, DEFAULT_SERVER_PORT);
        return 2;
    }

    std::string server_ip   = argv[1];
    std::string room        = argv[2];
    uint16_t    local_port  = (argc >= 4) ? static_cast<uint16_t>(atoi(argv[3])) : DEFAULT_LOCAL_PORT;
    uint16_t    server_port = (argc >= 5) ? static_cast<uint16_t>(atoi(argv[4])) : DEFAULT_SERVER_PORT;

    platform_socket_init();
    ios_configure_audio_session();
    std::signal(SIGINT, signal_handler);
#ifndef _WIN32
    std::signal(SIGTERM, signal_handler);
#endif

    // --- Phase 1: signaling + hole punch ---------------------------------
    printf("============================================================\n");
    printf("  MusiConnect P2P transceiver — signaling phase\n");
    printf("============================================================\n");
    printf("  Server : %s:%u\n", server_ip.c_str(), server_port);
    printf("  Room   : \"%s\"\n", room.c_str());
    printf("  Local  : UDP port %u (also the audio port)\n", local_port);
    printf("  Waiting for peer... (Ctrl+C to abort)\n");
    printf("============================================================\n");

    ResolvedPeer peer;
    {
        SignalingClient sigc(local_port);
        ResolveResult rr = sigc.resolve(server_ip, server_port, room, peer);
        if (rr != ResolveResult::Success) {
            fprintf(stderr, "\n[p2p] signaling failed: %s\n", resolve_result_str(rr));
            platform_socket_cleanup();
            return 1;
        }
    } // sigc closed its socket here; we re-bind the same local port below.

    printf("\n[p2p] peer resolved: %s:%u  (%s)\n",
           peer.ip.c_str(), peer.port,
           peer.confirmed ? "CONFIRMED bidirectional path"
                          : "best-effort — may be unreliable on strict NAT");

    // --- Phase 2: bind the audio socket on the SAME local port -----------
    // SO_REUSEADDR lets us rebind immediately; the NAT mapping opened during
    // hole punching persists (mappings typically live 30s+), so peers keep the
    // direct path.
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock == INVALID_SOCK) {
        fprintf(stderr, "[p2p] failed to create audio socket\n");
        platform_socket_cleanup();
        return 1;
    }

    int yes = 1;
    setsockopt(g_sock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&yes), sizeof(yes));

    int bufsize = 1 << 16;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&bufsize), sizeof(bufsize));
    setsockopt(g_sock, SOL_SOCKET, SO_SNDBUF,
               reinterpret_cast<const char*>(&bufsize), sizeof(bufsize));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(local_port);
    if (bind(g_sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        fprintf(stderr, "[p2p] failed to bind audio socket on port %u\n", local_port);
        CLOSE_SOCKET(g_sock);
        platform_socket_cleanup();
        return 1;
    }

    // Remote endpoint = resolved peer.
    std::memset(&g_remote_addr, 0, sizeof(g_remote_addr));
    g_remote_addr.sin_family = AF_INET;
    g_remote_addr.sin_port = htons(peer.port);
    if (inet_pton(AF_INET, peer.ip.c_str(), &g_remote_addr.sin_addr) != 1) {
        fprintf(stderr, "[p2p] invalid peer IP: %s\n", peer.ip.c_str());
        CLOSE_SOCKET(g_sock);
        platform_socket_cleanup();
        return 1;
    }

    // --- PortAudio init --------------------------------------------------
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        fprintf(stderr, "PortAudio init failed: %s\n", Pa_GetErrorText(err));
        CLOSE_SOCKET(g_sock);
        platform_socket_cleanup();
        return 1;
    }

    list_devices();
    int input_dev  = prompt_device("INPUT",  Pa_GetDefaultInputDevice());
    int output_dev = prompt_device("OUTPUT", Pa_GetDefaultOutputDevice());

    g_jitter = new JitterBuffer(CHANNELS, RATE, JITTER_MS, MAX_JITTER_MS);

    // Input stream
    PaStreamParameters in_params{};
    in_params.device = input_dev;
    in_params.channelCount = CHANNELS;
    in_params.sampleFormat = paInt16;
    in_params.suggestedLatency = Pa_GetDeviceInfo(input_dev)->defaultLowInputLatency;
    in_params.hostApiSpecificStreamInfo = nullptr;

    PaStream* in_stream = nullptr;
    err = Pa_OpenStream(&in_stream, &in_params, nullptr,
                        RATE, FRAME_SIZE, paClipOff,
                        capture_callback, nullptr);
    if (err != paNoError) {
        fprintf(stderr, "Failed to open input stream: %s\n", Pa_GetErrorText(err));
        Pa_Terminate();
        CLOSE_SOCKET(g_sock);
        platform_socket_cleanup();
        return 1;
    }

    // Output stream
    PaStreamParameters out_params{};
    out_params.device = output_dev;
    out_params.channelCount = CHANNELS;
    out_params.sampleFormat = paInt16;
    out_params.suggestedLatency = Pa_GetDeviceInfo(output_dev)->defaultLowOutputLatency;
    out_params.hostApiSpecificStreamInfo = nullptr;

    PaStream* out_stream = nullptr;
    err = Pa_OpenStream(&out_stream, nullptr, &out_params,
                        RATE, FRAME_SIZE, paClipOff,
                        playout_callback, nullptr);
    if (err != paNoError) {
        fprintf(stderr, "Failed to open output stream: %s\n", Pa_GetErrorText(err));
        Pa_CloseStream(in_stream);
        Pa_Terminate();
        CLOSE_SOCKET(g_sock);
        platform_socket_cleanup();
        return 1;
    }

    const PaStreamInfo* in_info  = Pa_GetStreamInfo(in_stream);
    const PaStreamInfo* out_info = Pa_GetStreamInfo(out_stream);
    double in_hw_ms  = (in_info  ? in_info->inputLatency   : 0.0) * 1000.0;
    double out_hw_ms = (out_info ? out_info->outputLatency : 0.0) * 1000.0;

    printf("\n============================================================\n");
    printf("  TRANSCEIVER ACTIVE (P2P)\n");
    printf("============================================================\n");
    printf("  Input  : device %d | %dch @ %dHz | block %d (%.1fms)\n",
           input_dev, CHANNELS, RATE, FRAME_SIZE, FRAME_SIZE * 1000.0 / RATE);
    printf("           HW buffer: %.1f ms\n", in_hw_ms);
    printf("  Output : device %d | %dch @ %dHz | block %d (%.1fms)\n",
           output_dev, CHANNELS, RATE, FRAME_SIZE, FRAME_SIZE * 1000.0 / RATE);
    printf("           HW buffer: %.1f ms\n", out_hw_ms);
    printf("  Remote : %s:%u\n", peer.ip.c_str(), peer.port);
    printf("  Listen : 0.0.0.0:%u\n", local_port);
    printf("  Jitter : target %dms, max %dms\n", JITTER_MS, MAX_JITTER_MS);
    printf("  Press Ctrl+C to stop\n");
    printf("============================================================\n\n");

    Pa_StartStream(in_stream);
    Pa_StartStream(out_stream);
    std::thread net_thread(net_rx_loop);

    // Main loop: meters (RTT is collected inside net_rx_loop now).
    while (g_running.load()) {
        float tx_p = g_tx_peak.load();
        float rx_p = g_rx_peak.load();
        int tx_bars = static_cast<int>(tx_p * 40);
        int rx_bars = static_cast<int>(rx_p * 40);
        float buf_ms = static_cast<float>(g_jitter->fill_frames()) / RATE * 1000.0f;
        float one_way = g_last_one_way.load();

        char rtt_txt[32] = "RTT --";
        {
            std::lock_guard<std::mutex> lock(g_rtt_mtx);
            if (!g_rtt_list.empty()) {
                size_t start = g_rtt_list.size() > 100 ? g_rtt_list.size() - 100 : 0;
                double sum = 0;
                for (size_t i = start; i < g_rtt_list.size(); i++) sum += g_rtt_list[i];
                double avg = sum / static_cast<double>(g_rtt_list.size() - start);
                snprintf(rtt_txt, sizeof(rtt_txt), "RTT ~%.1fms", avg);
            }
        }

        char tx_bar[41], rx_bar[41];
        std::memset(tx_bar, ' ', 40); tx_bar[40] = '\0';
        std::memset(rx_bar, ' ', 40); rx_bar[40] = '\0';
        for (int i = 0; i < std::min(tx_bars, 40); i++) tx_bar[i] = '#';
        for (int i = 0; i < std::min(rx_bars, 40); i++) rx_bar[i] = '#';

        float est_e2e = one_way + buf_ms + static_cast<float>(out_hw_ms);

        printf("  [TX] %s %.3f  sent=%u  %s\n",
               tx_bar, tx_p, g_tx_seq.load(), rtt_txt);
        printf("  [RX] %s %.3f  recv=%u  buf=%.0fms  1way~%.0fms  e2e~%.0fms  under=%d over=%d\n\n",
               rx_bar, rx_p, g_rx_packets.load(), buf_ms, one_way, est_e2e,
               g_jitter->underruns(), g_jitter->overflows());
        fflush(stdout);

        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    // --- Shutdown --------------------------------------------------------
    g_running.store(false);
    net_thread.join();

    Pa_StopStream(in_stream);
    Pa_StopStream(out_stream);
    Pa_CloseStream(in_stream);
    Pa_CloseStream(out_stream);
    Pa_Terminate();

    CLOSE_SOCKET(g_sock);
    platform_socket_cleanup();

    printf("\n============================================================\n");
    printf("  TRANSCEIVER SUMMARY\n");
    printf("============================================================\n");
    printf("  Packets sent:     %u\n", g_tx_seq.load());
    printf("  Packets received: %u\n", g_rx_packets.load());
    printf("  Underruns: %d | Overflows: %d\n",
           g_jitter->underruns(), g_jitter->overflows());
    {
        std::lock_guard<std::mutex> lock(g_rtt_mtx);
        if (!g_rtt_list.empty()) {
            double sum = std::accumulate(g_rtt_list.begin(), g_rtt_list.end(), 0.0);
            double avg = sum / g_rtt_list.size();
            double mn = *std::min_element(g_rtt_list.begin(), g_rtt_list.end());
            double mx = *std::max_element(g_rtt_list.begin(), g_rtt_list.end());
            printf("  RTT — Avg: %.2fms | Min: %.2fms | Max: %.2fms\n", avg, mn, mx);
        }
    }
    printf("  Input HW buffer:  %.1f ms\n", in_hw_ms);
    printf("  Output HW buffer: %.1f ms\n", out_hw_ms);
    printf("  Jitter target:    %d ms\n", JITTER_MS);
    float final_e2e = static_cast<float>(in_hw_ms) + g_last_one_way.load()
                    + static_cast<float>(g_jitter->fill_frames()) / RATE * 1000.0f
                    + static_cast<float>(out_hw_ms);
    printf("  Estimated full capture->ear latency: ~%.0f ms\n", final_e2e);
    printf("============================================================\n\n");

    delete g_jitter;
    return 0;
}
