// =============================================================================
// LOW-LATENCY AUDIO TRANSCEIVER — C++ / PortAudio / UDP
//
// Cross-platform: macOS/iOS, Windows, Linux
// Build: link against PortAudio (-lportaudio) and platform socket libs.
//
// Packet format (matches the Python version):
//   [seq: uint32_t (4B)] [timestamp: double (8B)] [PCM int16 interleaved...]
// =============================================================================

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
// PLATFORM: Sockets
// =============================================================================
#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h> //For windows
  #include <ws2tcpip.h> // for iOS
  #pragma comment(lib, "ws2_32.lib")
  typedef int socklen_t;
  static void platform_socket_init() {
      WSADATA wsa;
      WSAStartup(MAKEWORD(2, 2), &wsa);
  }
  static void platform_socket_cleanup() { WSACleanup(); }
  static void set_nonblocking(SOCKET s) {
      u_long mode = 1;
      ioctlsocket(s, FIONBIO, &mode);
  }
  typedef SOCKET socket_t;
  #define INVALID_SOCK INVALID_SOCKET
  #define CLOSE_SOCKET closesocket
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  static void platform_socket_init() {}
  static void platform_socket_cleanup() {}
  static void set_nonblocking(int s) {
      int flags = fcntl(s, F_GETFL, 0);
      fcntl(s, F_SETFL, flags | O_NONBLOCK);
  }
  typedef int socket_t;
  #define INVALID_SOCK (-1)
  #define CLOSE_SOCKET close
#endif

// =============================================================================
// PLATFORM: iOS Audio Session
// =============================================================================
#if defined(__APPLE__)
  #include <TargetConditionals.h>
  #if TARGET_OS_IPHONE
    // On iOS, configure AVAudioSession before PortAudio opens a stream.
    // This must be compiled as Objective-C++ (.mm) or linked separately.
    // Stub here — implement in a .mm file if building for iOS.
    extern "C" void ios_configure_audio_session();
  #else
    static void ios_configure_audio_session() {} // macOS — no-op
  #endif
#else
  static void ios_configure_audio_session() {} // Windows/Linux — no-op
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
static const int REMOTE_PORT = 12345;
static const int LOCAL_PORT = 12345;
static const int RTT_PORT = 12346;
static const int HEADER_SIZE = 12;        // 4 (seq) + 8 (timestamp)
static const int JITTER_MS = 25;
static const int MAX_JITTER_MS = 120;
static const int MAX_PACKET = 4096;

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

// Sockets
static socket_t g_rx_sock = INVALID_SOCK;
static socket_t g_tx_sock = INVALID_SOCK;
static sockaddr_in g_remote_addr{};

// Jitter buffer
static JitterBuffer* g_jitter = nullptr;

// Metrics
static std::atomic<uint32_t> g_tx_seq{0};
static std::atomic<float> g_tx_peak{0.0f};
static std::atomic<float> g_rx_peak{0.0f};
static std::atomic<uint32_t> g_rx_packets{0};
static std::atomic<float> g_last_one_way{0.0f};

// RTT tracking (main thread only)
static std::mutex g_rtt_mtx;
static std::vector<double> g_rtt_list;

// =============================================================================
// PORTAUDIO CALLBACKS
// =============================================================================

// Capture callback — sends audio over UDP immediately
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

    sendto(g_tx_sock, reinterpret_cast<const char*>(packet),
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
// NETWORK RECEIVE THREAD
// =============================================================================
static void net_rx_loop() {
    uint8_t buf[MAX_PACKET];
    sockaddr_in sender_addr{};
    socklen_t sender_len = sizeof(sender_addr);

    // Set a timeout so the thread can notice shutdown
#ifdef _WIN32
    DWORD timeout_ms = 500;
    setsockopt(g_rx_sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 500000;
    setsockopt(g_rx_sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif

    while (g_running.load()) {
        int n = recvfrom(g_rx_sock, reinterpret_cast<char*>(buf), MAX_PACKET, 0,
                         reinterpret_cast<sockaddr*>(&sender_addr), &sender_len);
        if (n <= HEADER_SIZE) continue;

        // Parse header
        uint32_t seq;
        double t_captured;
        std::memcpy(&seq, buf, 4);
        std::memcpy(&t_captured, buf + 4, 8);

        // Parse audio
        int audio_bytes = n - HEADER_SIZE;
        int total_samples = audio_bytes / static_cast<int>(sizeof(int16_t));
        if (total_samples % CHANNELS != 0) continue; // malformed

        int frames = total_samples / CHANNELS;
        g_jitter->push(reinterpret_cast<int16_t*>(buf + HEADER_SIZE), frames);

        g_last_one_way.store(static_cast<float>((now_seconds() - t_captured) * 1000.0));
        g_rx_packets.fetch_add(1);

        // Echo header back for RTT measurement
        sockaddr_in echo_addr = sender_addr;
        echo_addr.sin_port = htons(RTT_PORT);
        sendto(g_rx_sock, reinterpret_cast<const char*>(buf), HEADER_SIZE, 0,
               reinterpret_cast<sockaddr*>(&echo_addr), sizeof(echo_addr));
    }
}


// =============================================================================
// DEVICE LISTING HELPER
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
        // Trim newline
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len > 0) return atoi(line);
    }
    return default_dev;
}

// =============================================================================
// MAIN
// =============================================================================
int main() {
    // --- Platform init ---
    platform_socket_init();
    ios_configure_audio_session();
    std::signal(SIGINT, signal_handler);
#ifndef _WIN32
    std::signal(SIGTERM, signal_handler);
#endif

    // --- Get remote IP ---
    char remote_ip[64];
    printf("Enter remote peer IP address: ");
    fflush(stdout);
    if (!fgets(remote_ip, sizeof(remote_ip), stdin)) {
        fprintf(stderr, "Failed to read IP\n");
        return 1;
    }
    // Trim newline
    int len = static_cast<int>(strlen(remote_ip));
    while (len > 0 && (remote_ip[len-1] == '\n' || remote_ip[len-1] == '\r'))
        remote_ip[--len] = '\0';

    // --- Set up remote address ---
    std::memset(&g_remote_addr, 0, sizeof(g_remote_addr));
    g_remote_addr.sin_family = AF_INET;
    g_remote_addr.sin_port = htons(REMOTE_PORT);
    if (inet_pton(AF_INET, remote_ip, &g_remote_addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid IP address: %s\n", remote_ip);
        return 1;
    }

    // --- Create sockets ---
    g_rx_sock = socket(AF_INET, SOCK_DGRAM, 0);
    g_tx_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_rx_sock == INVALID_SOCK || g_tx_sock == INVALID_SOCK) {
        fprintf(stderr, "Failed to create sockets\n");
        return 1;
    }

    // Set socket buffer sizes
    int bufsize = 1 << 16;
    setsockopt(g_rx_sock, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&bufsize), sizeof(bufsize));
    setsockopt(g_tx_sock, SOL_SOCKET, SO_SNDBUF,
               reinterpret_cast<const char*>(&bufsize), sizeof(bufsize));

    // Bind rx socket
    sockaddr_in rx_addr{};
    rx_addr.sin_family = AF_INET;
    rx_addr.sin_addr.s_addr = INADDR_ANY;
    rx_addr.sin_port = htons(LOCAL_PORT);
    if (bind(g_rx_sock, reinterpret_cast<sockaddr*>(&rx_addr), sizeof(rx_addr)) != 0) {
        fprintf(stderr, "Failed to bind rx socket on port %d\n", LOCAL_PORT);
        return 1;
    }

    // Bind tx socket (for RTT echo replies)
    sockaddr_in tx_addr{};
    tx_addr.sin_family = AF_INET;
    tx_addr.sin_addr.s_addr = INADDR_ANY;
    tx_addr.sin_port = htons(RTT_PORT);
    if (bind(g_tx_sock, reinterpret_cast<sockaddr*>(&tx_addr), sizeof(tx_addr)) != 0) {
        fprintf(stderr, "Failed to bind tx socket on port %d\n", RTT_PORT);
        return 1;
    }
    set_nonblocking(g_tx_sock);

    // --- Init PortAudio ---
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        fprintf(stderr, "PortAudio init failed: %s\n", Pa_GetErrorText(err));
        return 1;
    }

    list_devices();

    int input_dev = prompt_device("INPUT", Pa_GetDefaultInputDevice());
    int output_dev = prompt_device("OUTPUT", Pa_GetDefaultOutputDevice());

    // --- Create jitter buffer ---
    g_jitter = new JitterBuffer(CHANNELS, RATE, JITTER_MS, MAX_JITTER_MS);

    // --- Open input stream ---
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
        return 1;
    }

    // --- Open output stream ---
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
        return 1;
    }

    // --- Get negotiated latencies ---
    const PaStreamInfo* in_info = Pa_GetStreamInfo(in_stream);
    const PaStreamInfo* out_info = Pa_GetStreamInfo(out_stream);
    double in_hw_ms = (in_info ? in_info->inputLatency : 0.0) * 1000.0;
    double out_hw_ms = (out_info ? out_info->outputLatency : 0.0) * 1000.0;

    // --- Print config ---
    printf("\n============================================================\n");
    printf("  TRANSCEIVER ACTIVE\n");
    printf("============================================================\n");
    printf("  Input  : device %d | %dch @ %dHz | block %d (%.1fms)\n",
           input_dev, CHANNELS, RATE, FRAME_SIZE, FRAME_SIZE * 1000.0 / RATE);
    printf("           HW buffer: %.1f ms\n", in_hw_ms);
    printf("  Output : device %d | %dch @ %dHz | block %d (%.1fms)\n",
           output_dev, CHANNELS, RATE, FRAME_SIZE, FRAME_SIZE * 1000.0 / RATE);
    printf("           HW buffer: %.1f ms\n", out_hw_ms);
    printf("  Remote : %s:%d\n", remote_ip, REMOTE_PORT);
    printf("  Listen : 0.0.0.0:%d\n", LOCAL_PORT);
    printf("  Jitter : target %dms, max %dms\n", JITTER_MS, MAX_JITTER_MS);
    printf("  Press Ctrl+C to stop\n");
    printf("============================================================\n\n");

    // --- Start streams and network thread ---
    Pa_StartStream(in_stream);
    Pa_StartStream(out_stream);
    std::thread net_thread(net_rx_loop);

    // --- Main loop: meters + RTT drain ---
    while (g_running.load()) {
        // Drain RTT echo replies (non-blocking from tx_sock)
        uint8_t rtt_buf[64];
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        while (true) {
            int n = recvfrom(g_tx_sock, reinterpret_cast<char*>(rtt_buf), 64, 0,
                             reinterpret_cast<sockaddr*>(&from), &from_len);
            if (n < HEADER_SIZE) break;
            double t_sent;
            std::memcpy(&t_sent, rtt_buf + 4, 8);
            double rtt = (now_seconds() - t_sent) * 1000.0;
            std::lock_guard<std::mutex> lock(g_rtt_mtx);
            g_rtt_list.push_back(rtt);
        }

        // Display
        float tx_p = g_tx_peak.load();
        float rx_p = g_rx_peak.load();
        int tx_bars = static_cast<int>(tx_p * 40);
        int rx_bars = static_cast<int>(rx_p * 40);
        float buf_ms = static_cast<float>(g_jitter->fill_frames()) / RATE * 1000.0f;
        float one_way = g_last_one_way.load();

        // RTT average
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

        // Build meter bars
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

    // --- Shutdown ---
    g_running.store(false);
    net_thread.join();

    Pa_StopStream(in_stream);
    Pa_StopStream(out_stream);
    Pa_CloseStream(in_stream);
    Pa_CloseStream(out_stream);
    Pa_Terminate();

    CLOSE_SOCKET(g_rx_sock);
    CLOSE_SOCKET(g_tx_sock);
    platform_socket_cleanup();

    // --- Summary ---
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
