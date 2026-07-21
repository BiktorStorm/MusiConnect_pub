// =============================================================================
// MUSICONNECT GUI — Dear ImGui + GLFW + OpenGL3
//
// A simple settings window that replaces command-line arguments:
//   - Audio device selection (dropdown)
//   - Remote IP / port
//   - Local port
//   - Buffer size
//   - Bitrate
//   - Connect / Disconnect button
//
// The GUI runs on the main thread. The audio pipeline runs on its own
// realtime thread(s) via AudioEngine — completely independent.
// =============================================================================

#include "audio_engine.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>

#include <string>
#include <vector>
#include <cstring>
#include <cstdio>

// Helper: convert vector<string> to array of const char* for ImGui combo
static std::vector<const char*> toCStrArray(const std::vector<std::string>& v) {
    std::vector<const char*> result;
    result.reserve(v.size());
    for (auto& s : v) result.push_back(s.c_str());
    return result;
}

int main(int, char**) {
    // =========================================================================
    // GLFW + OpenGL init
    // =========================================================================
    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialize GLFW\n");
        return 1;
    }

    // OpenGL 3.3 core
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    GLFWwindow* window = glfwCreateWindow(480, 400, "MusiConnect", nullptr, nullptr);
    if (!window) {
        fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // vsync

    // =========================================================================
    // Dear ImGui init
    // =========================================================================
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    // Scale for readability
    ImGui::GetStyle().FrameRounding = 4.0f;
    ImGui::GetStyle().GrabRounding = 4.0f;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    const char* glsl_version = "#version 330 core";
#ifdef __APPLE__
    glsl_version = "#version 150";
#endif
    ImGui_ImplOpenGL3_Init(glsl_version);

    // =========================================================================
    // Application state
    // =========================================================================
    AudioEngine engine;

    // Enumerate devices once at startup
    std::vector<std::string> devices = AudioEngine::listDevices();
    auto devicesCStr = toCStrArray(devices);
    int selectedDevice = 0;

    // Settings fields
    char remoteHost[128] = "127.0.0.1";
    int remotePort = 4465;
    int localPort = 4464;
    int bufferSize = 64;
    int bitrate = 64000;

    // Status
    std::string statusMessage;
    bool statusIsError = false;

    // =========================================================================
    // Main loop
    // =========================================================================
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Full-window ImGui panel
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("MusiConnect", nullptr,
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoTitleBar);

        ImGui::TextUnformatted("MusiConnect — Ultra Low Latency P2P Audio");
        ImGui::Separator();
        ImGui::Spacing();

        bool isRunning = engine.isRunning();

        // ----- Audio Device -----
        ImGui::Text("Audio Device");
        if (isRunning) ImGui::BeginDisabled();

        if (devices.empty()) {
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "No audio devices found");
        } else {
            ImGui::SetNextItemWidth(-1);
            ImGui::Combo("##device", &selectedDevice, devicesCStr.data(),
                         static_cast<int>(devicesCStr.size()));
        }

        ImGui::Spacing();

        // ----- Network Settings -----
        ImGui::Text("Network");
        ImGui::Separator();

        ImGui::SetNextItemWidth(200);
        ImGui::InputText("Remote IP", remoteHost, sizeof(remoteHost));

        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("Remote Port", &remotePort, 0);

        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("Local Port", &localPort, 0);

        ImGui::Spacing();

        // ----- Audio Settings -----
        ImGui::Text("Audio Settings");
        ImGui::Separator();

        ImGui::SetNextItemWidth(120);
        if (ImGui::BeginCombo("Buffer Size", std::to_string(bufferSize).c_str())) {
            int sizes[] = {32, 64, 128, 256, 512};
            for (int s : sizes) {
                bool selected = (bufferSize == s);
                char label[32];
                snprintf(label, sizeof(label), "%d samples (%.2f ms)", s, s / 48.0);
                if (ImGui::Selectable(label, selected)) bufferSize = s;
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::SetNextItemWidth(120);
        if (ImGui::BeginCombo("Bitrate", (std::to_string(bitrate / 1000) + " kbps").c_str())) {
            int rates[] = {32000, 48000, 64000, 96000, 128000, 192000, 256000};
            for (int r : rates) {
                bool selected = (bitrate == r);
                char label[32];
                snprintf(label, sizeof(label), "%d kbps", r / 1000);
                if (ImGui::Selectable(label, selected)) bitrate = r;
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        if (isRunning) ImGui::EndDisabled();

        ImGui::Spacing();
        ImGui::Spacing();

        // ----- Connect / Disconnect -----
        if (!isRunning) {
            if (ImGui::Button("Connect", ImVec2(120, 36))) {
                EngineConfig config;
                config.audioDevice = devices.empty() ? "" : devices[selectedDevice];
                config.sampleRate = 48000;
                config.bufferSize = bufferSize;
                config.bitrate = bitrate;
                config.remoteHost = remoteHost;
                config.remotePort = remotePort;
                config.localPort = localPort;

                if (engine.start(config)) {
                    statusMessage = "Connected";
                    statusIsError = false;
                } else {
                    statusMessage = engine.getLastError();
                    statusIsError = true;
                }
            }
        } else {
            if (ImGui::Button("Disconnect", ImVec2(120, 36))) {
                engine.stop();
                statusMessage = "Disconnected";
                statusIsError = false;
            }

            // Show live stats
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Text("Status: Running");

            EngineStats stats = engine.getStats();
            ImGui::Text("Buffer: %d samples (%.2f ms)",
                        stats.actualBufferSize,
                        stats.actualBufferSize / 48.0);
            ImGui::Text("Latency: ~%.1f ms", stats.latencyMs);
            ImGui::Text("Sent: %llu  |  Received: %llu",
                        (unsigned long long)stats.packetsSent,
                        (unsigned long long)stats.packetsReceived);
            ImGui::Text("Lost: %llu  |  Underruns: %llu",
                        (unsigned long long)stats.packetsLost,
                        (unsigned long long)stats.underruns);
        }

        // ----- Status message -----
        if (!statusMessage.empty()) {
            ImGui::Spacing();
            ImVec4 color = statusIsError ? ImVec4(1, 0.3f, 0.3f, 1) : ImVec4(0.3f, 1, 0.3f, 1);
            ImGui::TextColored(color, "%s", statusMessage.c_str());
        }

        // ----- Refresh devices button -----
        ImGui::Spacing();
        if (!isRunning) {
            if (ImGui::SmallButton("Refresh Devices")) {
                devices = AudioEngine::listDevices();
                devicesCStr = toCStrArray(devices);
                selectedDevice = 0;
            }
        }

        ImGui::End();

        // Render
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // =========================================================================
    // Cleanup
    // =========================================================================
    engine.stop();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
