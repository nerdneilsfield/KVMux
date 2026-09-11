#include "app/kvm_session.hpp"
#include "network/relay_client.hpp"
#include "render/video_renderer.hpp"
#include "support/config.hpp"
#include "support/diagnostics.hpp"

#include <glad/gl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <iostream>
#include <string_view>
#include <spdlog/spdlog.h>
#include <optional>
#include <string>
#include <vector>

namespace {
using namespace kvmux;

const char* capture_state(CaptureState state) {
    switch (state) { case CaptureState::stopped: return "Stopped"; case CaptureState::starting: return "Starting"; case CaptureState::streaming: return "Streaming"; case CaptureState::stopping: return "Stopping"; case CaptureState::permission_denied: return "Permission denied"; case CaptureState::fault: return "Fault"; }
    return "Unknown";
}
const char* control_state(ControlConnectionState state) {
    switch (state) { case ControlConnectionState::disconnected: return "Disconnected"; case ControlConnectionState::opening: return "Opening"; case ControlConnectionState::monitoring: return "USB not ready"; case ControlConnectionState::clearing: return "Clearing input"; case ControlConnectionState::ready: return "Ready"; case ControlConnectionState::stalled: return "Stalled"; case ControlConnectionState::reconnecting: return "Reconnecting"; case ControlConnectionState::fault: return "Fault"; case ControlConnectionState::stopping: return "Stopping"; }
    return "Unknown";
}
const char* input_state(InputState state) {
    switch (state) { case InputState::preview: return "Preview"; case InputState::arming: return "Arming"; case InputState::captured: return "Captured"; case InputState::releasing: return "Releasing"; case InputState::fault: return "Fault"; }
    return "Unknown";
}
std::string mode_text(const CaptureMode& mode) {
    return std::to_string(mode.width) + "x" + std::to_string(mode.height) + " @ " +
        std::to_string(mode.frame_rate.numerator) + "/" + std::to_string(mode.frame_rate.denominator) +
        " " + mode.device_format_name;
}
std::optional<std::filesystem::path> preference_file() {
    char* raw = SDL_GetPrefPath("kvmux", "kvmux");
    if (!raw) return std::nullopt;
    std::filesystem::path result(raw); SDL_free(raw); return result / "config.json";
}

std::optional<std::uint16_t> usb_usage_from_sdl(const SDL_Scancode scancode) {
    static constexpr std::array<std::uint16_t, 232> table = [] {
        std::array<std::uint16_t, 232> values{};
        for (std::uint16_t usage = 0x04; usage <= 0x73; ++usage) values[usage] = usage;
        for (std::uint16_t usage = 0x7f; usage <= 0x82; ++usage) values[usage] = usage;
        for (std::uint16_t usage = 0x85; usage <= 0x87; ++usage) values[usage] = usage;
        for (std::uint16_t usage = 0x89; usage <= 0x8f; ++usage) values[usage] = usage;
        for (std::uint16_t usage = 0xe0; usage <= 0xe7; ++usage) values[usage] = usage;
        return values;
    }();
    const auto index = static_cast<std::size_t>(scancode);
    if (index >= table.size() || table[index] == 0) return std::nullopt;
    return table[index];
}
InputEvent to_input(const SDL_Event& event) {
    if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
        const auto usage = usb_usage_from_sdl(event.key.scancode);
        return {InputKey{usage.value_or(0), event.key.down, event.key.repeat}};
    }
    if (event.type == SDL_EVENT_MOUSE_MOTION) return {InputPointerMotion{event.motion.x, event.motion.y}};
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        InputMouseButton b = event.button.button == SDL_BUTTON_RIGHT ? InputMouseButton::right : event.button.button == SDL_BUTTON_MIDDLE ? InputMouseButton::middle : InputMouseButton::left;
        return {InputButton{b, event.button.down, event.button.x, event.button.y}};
    }
    if (event.type == SDL_EVENT_MOUSE_WHEEL) { const double sign = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0 : 1.0; return {InputWheel{event.wheel.y * sign, event.wheel.mouse_x, event.wheel.mouse_y}}; }
    return {InputPointerMotion{}};
}
}

int main(int argc, char** argv) {
    bool debug = false, help = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--debug") debug = true;
        else if (argument == "--help") help = true;
        else {
            std::cerr << "kvmux: Unknown argument: " << argument << "\nUsage: kvmux [--debug] [--help]\n";
            return 2;
        }
    }
    configure_console_logging(debug);
    if (help) {
        std::cout << "Usage: kvmux [--debug] [--help]\n"
                     "  --debug  Write diagnostic logs to stderr (time, thread, level)\n"
                     "  --help   Show this help without starting the GUI\n";
        return 0;
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        spdlog::error("SDL initialization failed: {}", SDL_GetError()); return 1;
    }
    auto pref = preference_file();
    Config config = pref ? load_config(*pref) : Config{};
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_Window* window = SDL_CreateWindow("KVMux", config.window.width, config.window.height, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) { SDL_Quit(); return 1; }
    SDL_SetWindowMinimumSize(window, 960, 640);
    if (config.window.maximized) SDL_MaximizeWindow(window);
    SDL_GLContext context = SDL_GL_CreateContext(window);
    if (!context || !gladLoadGL(reinterpret_cast<GLADloadfunc>(SDL_GL_GetProcAddress))) { if (context) SDL_GL_DestroyContext(context); SDL_DestroyWindow(window); SDL_Quit(); return 1; }
    SDL_GL_SetSwapInterval(config.vsync ? 1 : 0);
    IMGUI_CHECKVERSION(); ImGui::CreateContext(); ImGui::GetIO().IniFilename = nullptr; ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 6.F;
    ImGui::GetStyle().FrameRounding = 4.F;
    ImGui::GetStyle().FramePadding = {8.F, 6.F};
    ImGui_ImplSDL3_InitForOpenGL(window, context); ImGui_ImplOpenGL3_Init("#version 150");

    auto session = std::make_unique<KvmSession>(); (void)session->set_mouse_mode(config.mouse_mode); session->set_host_key(config.host_scancode); session->set_relative_gain(config.sensitivity);
    bool remote = false;
    std::shared_ptr<relay::RelayClient> remote_client;
    std::optional<relay::TrafficSnapshot> traffic_baseline;
    std::chrono::steady_clock::time_point traffic_sampled_at{};
    std::optional<double> video_bytes_per_second, control_bytes_per_second;
    char remote_host[64] = "127.0.0.1";
    int control_port = 17000, video_port = 17001;
    std::vector<std::future<void>> retired_sessions;
    auto retire_session = [&] {
        remote_client.reset();
        traffic_baseline.reset();
        video_bytes_per_second.reset(); control_bytes_per_second.reset();
        auto old = std::move(session);
        retired_sessions.push_back(std::async(std::launch::async, [old = std::move(old)]() mutable {
            old->shutdown(); old.reset();
        }));
    };
    VideoRenderer renderer; Diagnostics diagnostics;
    std::vector<DeviceInfo> devices; std::vector<CaptureMode> modes; std::vector<SerialPortInfo> ports;
    std::future<std::vector<DeviceInfo>> devices_future = session->enumerate_capture_devices();
    std::optional<std::future<std::vector<CaptureMode>>> modes_future;
    int selected_device = -1, selected_mode = -1, selected_port = -1;
    bool fullscreen{}, diagnostics_open{}, running = true;
    auto next_serial_scan = std::chrono::steady_clock::now();
    std::optional<VideoFrame> current_frame;
    std::string last_status;

    while (running) {
        std::erase_if(retired_sessions, [](auto& future) {
            return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        });
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) { running = false; continue; }
            if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) session->focus_lost();
            if (event.type == SDL_EVENT_WINDOW_MINIMIZED || event.type == SDL_EVENT_WINDOW_HIDDEN) session->minimized();
            if (event.type == SDL_EVENT_MOUSE_MOTION && config.mouse_mode == MouseMode::relative) session->handle_input({InputRelativeMotion{event.motion.xrel, event.motion.yrel}});
            else if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP || event.type == SDL_EVENT_MOUSE_WHEEL) session->handle_input(to_input(event));
        }
        if (devices_future.valid() && devices_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) devices = devices_future.get();
        if (modes_future && modes_future->wait_for(std::chrono::seconds(0)) == std::future_status::ready) { modes = modes_future->get(); selected_mode = modes.empty() ? -1 : 0; modes_future.reset(); }
        if (std::chrono::steady_clock::now() >= next_serial_scan) { ports = enumerate_serial_ports(); next_serial_scan = std::chrono::steady_clock::now() + std::chrono::seconds(1); }
        session->tick();
        if (auto newest = session->take_latest_frame()) { current_frame = std::move(newest); diagnostics.record_decode(current_frame->decoded); }
        if (current_frame && renderer.upload(*current_frame, config.color_override)) { diagnostics.record_sample_to_gpu_submit(std::chrono::steady_clock::now() - current_frame->arrival); diagnostics.record_present(current_frame->generation, current_frame->sequence); }
        const auto snapshot = session->snapshot();
        if (debug) {
            const auto status = std::string("capture=") + capture_state(snapshot.capture.state) +
                " error=" + snapshot.capture.error + " control=" + control_state(snapshot.control.state) +
                " error=" + snapshot.control.error + " input=" + input_state(snapshot.input_state) +
                " session-error=" + snapshot.session_error;
            if (status != last_status) { spdlog::debug("Session {}", status); last_status = status; }
        }
        const std::string resolution = current_frame && current_frame->frame
            ? std::to_string(current_frame->frame->width) + "x" + std::to_string(current_frame->frame->height)
            : "--";
        diagnostics.set_capture_mode(remote ? "Relay MJPEG (decoded video: " + resolution + ")"
            : snapshot.capture.actual_mode.width ? mode_text(snapshot.capture.actual_mode) : "--");
        const bool relay_connected = remote_client && snapshot.capture.state == CaptureState::streaming &&
            snapshot.control.state != ControlConnectionState::disconnected &&
            snapshot.control.state != ControlConnectionState::fault;
        if (relay_connected) {
            const auto now = std::chrono::steady_clock::now();
            const auto traffic = remote_client->traffic_snapshot();
            if (!traffic_baseline) {
                traffic_baseline = traffic; traffic_sampled_at = now;
            } else if (const double seconds = std::chrono::duration<double>(now - traffic_sampled_at).count(); seconds >= 1.0) {
                video_bytes_per_second = static_cast<double>(traffic.video_received_bytes - traffic_baseline->video_received_bytes) / seconds;
                control_bytes_per_second = (static_cast<double>(traffic.control_received_bytes - traffic_baseline->control_received_bytes) +
                    static_cast<double>(traffic.control_sent_bytes - traffic_baseline->control_sent_bytes)) / seconds;
                traffic_baseline = traffic; traffic_sampled_at = now;
            }
        } else {
            traffic_baseline.reset();
            video_bytes_per_second.reset(); control_bytes_per_second.reset();
        }
        diagnostics.set_mailbox_overwrites(snapshot.capture.overwritten_samples, snapshot.video.overwritten_frames);
        diagnostics.record_ack_rtt(snapshot.control.last_ack_rtt);
        diagnostics.set_pixel_path(renderer.snapshot().pixel_path); if (!renderer.snapshot().error.empty()) diagnostics.set_recent_error(renderer.snapshot().error);

        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplSDL3_NewFrame(); ImGui::NewFrame();
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        const bool captured = snapshot.input_state == InputState::captured;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.F);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.F);
        ImGui::Begin("KVMuxRoot", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_MenuBar);
        if (ImGui::BeginMenuBar()) {
            if (ImGui::MenuItem(fullscreen ? "Exit fullscreen" : "Fullscreen")) {
                fullscreen = !fullscreen; SDL_SetWindowFullscreen(window, fullscreen);
            }
            if (ImGui::MenuItem("Diagnostics")) diagnostics_open = !diagnostics_open;
            ImGui::EndMenuBar();
        }
        if (ImGui::CollapsingHeader("Connections", ImGuiTreeNodeFlags_DefaultOpen)) {
            const bool controls_enabled = !captured;
            ImGui::BeginDisabled(!controls_enabled);
            const bool retiring = !retired_sessions.empty();
            if (retiring) ImGui::TextUnformatted("Stopping previous session...");
            ImGui::BeginDisabled(retiring);
            bool requested_remote = remote;
            if (ImGui::RadioButton("Local", !remote)) requested_remote = false;
            ImGui::SameLine();
            if (ImGui::RadioButton("Remote", remote)) requested_remote = true;
            if (requested_remote != remote && !retiring) {
                retire_session(); remote = requested_remote;
                session = std::make_unique<KvmSession>();
                devices.clear(); modes.clear(); selected_device = selected_mode = -1;
                modes_future.reset(); devices_future = session->enumerate_capture_devices();
                current_frame.reset(); renderer.destroy();
                (void)session->set_mouse_mode(config.mouse_mode);
                session->set_host_key(config.host_scancode);
            }
            if (remote) {
                ImGui::InputText("IPv4 host", remote_host, sizeof(remote_host));
                ImGui::InputInt("Control port", &control_port);
                ImGui::InputInt("Video port", &video_port);
                if (ImGui::Button("Connect relay") && retired_sessions.empty() && control_port > 0 && control_port <= 65535 && video_port > 0 && video_port <= 65535) {
                    retire_session();
                    devices_future = {}; modes_future.reset();
                    devices.clear(); modes.clear(); selected_device = selected_mode = -1;
                    auto client = std::make_shared<relay::RelayClient>(relay::ClientOptions{
                        remote_host, static_cast<std::uint16_t>(control_port), static_cast<std::uint16_t>(video_port)});
                    remote_client = client;
                    auto capture = std::make_unique<relay::NetworkCaptureSource>(client);
                    const auto device = capture->enumerate_devices().front();
                    const auto mode = capture->enumerate_modes(device.stable_id).front();
                    session = std::make_unique<KvmSession>(std::move(capture), std::make_unique<relay::NetworkControlSink>(client));
                    session->set_host_key(config.host_scancode);
                    (void)session->set_mouse_mode(config.mouse_mode);
                    (void)session->select_capture(device, mode);
                    current_frame.reset(); renderer.destroy();
                }
                ImGui::SameLine();
                if (ImGui::Button("Disconnect relay")) {
                    session->release_control(); (void)session->stop_capture(); (void)session->disconnect_control();
                    remote_client.reset(); traffic_baseline.reset();
                    video_bytes_per_second.reset(); control_bytes_per_second.reset();
                    current_frame.reset(); renderer.destroy();
                }
            }
            ImGui::EndDisabled();
            if (!remote) {
            if (ImGui::BeginTable("connections", 4, ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("setting", ImGuiTableColumnFlags_WidthFixed, 120.F);
                ImGui::TableSetupColumn("choice", ImGuiTableColumnFlags_WidthStretch, 2.F);
                ImGui::TableSetupColumn("setting2", ImGuiTableColumnFlags_WidthFixed, 100.F);
                ImGui::TableSetupColumn("choice2", ImGuiTableColumnFlags_WidthStretch, 1.F);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Capture device");
                ImGui::TableSetColumnIndex(1);
                if (ImGui::BeginCombo("##capture_device", selected_device >= 0 ? devices[selected_device].display_name.c_str() : "Select device")) {
                    for (int i = 0; i < static_cast<int>(devices.size()); ++i) {
                        if (ImGui::Selectable(devices[i].display_name.c_str(), i == selected_device)) {
                            selected_device = i; modes.clear(); selected_mode = -1;
                            modes_future = session->enumerate_capture_modes(devices[i].stable_id);
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted("Serial port");
                ImGui::TableSetColumnIndex(3);
                if (ImGui::BeginCombo("##serial_port", selected_port >= 0 && selected_port < static_cast<int>(ports.size()) ? ports[selected_port].name.c_str() : "Select port")) {
                    for (int i = 0; i < static_cast<int>(ports.size()); ++i)
                        if (ImGui::Selectable(ports[i].name.c_str(), i == selected_port)) selected_port = i;
                    ImGui::EndCombo();
                }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Capture mode");
                ImGui::TableSetColumnIndex(1);
                if (ImGui::BeginCombo("##capture_mode", selected_mode >= 0 ? mode_text(modes[selected_mode]).c_str() : "Select mode")) {
                    for (int i = 0; i < static_cast<int>(modes.size()); ++i)
                        if (ImGui::Selectable(mode_text(modes[i]).c_str(), i == selected_mode)) selected_mode = i;
                    ImGui::EndCombo();
                }
                ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted("Baud rate");
                ImGui::TableSetColumnIndex(3); ImGui::InputInt("##baud", &config.serial_baud_rate);
                config.serial_baud_rate = std::max(1200, config.serial_baud_rate);
                ImGui::EndTable();
            }
            if (ImGui::Button("Start preview") && selected_device >= 0 && selected_mode >= 0) {
                (void)session->select_capture(devices[selected_device], modes[selected_mode]);
                current_frame.reset(); renderer.destroy();
            }
            ImGui::SameLine();
            if (ImGui::Button("Connect") && selected_port >= 0)
                (void)session->connect_control(ports[selected_port].name, config.serial_baud_rate, config.serial_address);
            ImGui::SameLine(); if (ImGui::Button("Disconnect")) (void)session->disconnect_control();
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Absolute", config.mouse_mode == MouseMode::absolute)) {
                config.mouse_mode = MouseMode::absolute; (void)session->set_mouse_mode(config.mouse_mode);
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Relative", config.mouse_mode == MouseMode::relative)) {
                config.mouse_mode = MouseMode::relative; (void)session->set_mouse_mode(config.mouse_mode);
            }
            ImGui::SameLine(); ImGui::SetNextItemWidth(180.F);
            float sensitivity = static_cast<float>(config.sensitivity);
            if (ImGui::SliderFloat("Sensitivity", &sensitivity, 0.1F, 4.F)) config.sensitivity = sensitivity;
            session->set_relative_gain(config.sensitivity);
            ImGui::SameLine(); if (ImGui::Button("Send Ctrl+Alt+Del")) (void)session->send_special(SpecialKeys::control_alt_delete);
            ImGui::SameLine(); if (ImGui::Button("Send Alt+Tab")) (void)session->send_special(SpecialKeys::alt_tab);
            ImGui::SameLine(); ImGui::Checkbox("VSync", &config.vsync);
            ImGui::SameLine(); ImGui::SetNextItemWidth(145.F);
            const char* color_names[] = {"Color: automatic", "Color: BT.601 limited", "Color: BT.601 full", "Color: BT.709 limited", "Color: BT.709 full"};
            int color = static_cast<int>(config.color_override);
            if (ImGui::Combo("##color", &color, color_names, 5)) config.color_override = static_cast<ColorOverride>(color);
            ImGui::SameLine(); ImGui::SetNextItemWidth(140.F);
    #if defined(__APPLE__)
            const char* host_names[] = {"Host: Right Command", "Host: Right Control"};
            int host = config.host_scancode == 228 ? 1 : 0;
            if (ImGui::Combo("##host", &host, host_names, 2)) config.host_scancode = host == 0 ? 231 : 228;
    #else
            const char* host_names[] = {"Host: Right Control", "Host: Right GUI"};
            int host = config.host_scancode == 231 ? 1 : 0;
            if (ImGui::Combo("##host", &host, host_names, 2)) config.host_scancode = host == 0 ? 228 : 231;
    #endif
            session->set_host_key(config.host_scancode);
            ImGui::EndDisabled();
        }
        SDL_GL_SetSwapInterval(config.vsync ? 1 : 0);
        if (captured) ImGui::TextUnformatted("Control captured. Host key releases control.");
        ImGui::Separator();
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float status_height = ImGui::GetTextLineHeightWithSpacing() * 3.F;
        const ImVec2 status_pos{ImGui::GetCursorScreenPos().x,
            ImGui::GetCursorScreenPos().y + available.y - status_height};
        // ImGui and SDL pointer coordinates are logical pixels. Use this same fitted
        // rectangle for drawing and input; only glViewport uses framebuffer pixels.
        const ImVec2 video_size{available.x, std::max(1.F, available.y - status_height - ImGui::GetStyle().ItemSpacing.y)};
        const ImVec2 start = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##video_surface", video_size);
        ImGui::GetWindowDrawList()->AddRectFilled(start, {start.x + video_size.x, start.y + video_size.y}, IM_COL32(0, 0, 0, 255));
        if (renderer.texture_id() && renderer.width() > 0) {
            const auto fit = fit_video_rect({start.x, start.y, video_size.x, video_size.y}, renderer.width(), renderer.height());
            session->set_video_rect(fit);
            ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(renderer.texture_id()),
                {static_cast<float>(fit.x), static_cast<float>(fit.y)},
                {static_cast<float>(fit.x + fit.width), static_cast<float>(fit.y + fit.height)}, {0, 0}, {1, 1});
        } else {
            session->set_video_rect({});
        }
        if (!snapshot.video_fresh) {
            const ImVec2 warning_pos{start.x + 16.F, start.y + 16.F};
            ImGui::GetWindowDrawList()->AddText(warning_pos, IM_COL32(255, 100, 80, 255),
                "VIDEO UNAVAILABLE - control disabled");
        }
        ImGui::SetCursorScreenPos(status_pos);
        ImGui::BeginChild("Status", {available.x, status_height}, ImGuiChildFlags_None,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const auto d = diagnostics.snapshot();
        ImGui::Text("Video: %s  |  Decode: %.1f fps  |  Present: %.1f fps",
            resolution.c_str(), d.decode_fps, d.unique_present_fps);
        if (video_bytes_per_second && control_bytes_per_second)
            ImGui::Text("Video in: %.1f KiB/s  |  Control in+out: %.1f KiB/s",
                *video_bytes_per_second / 1024.0, *control_bytes_per_second / 1024.0);
        else ImGui::TextUnformatted("Video in: --  |  Control in+out: --");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(
            "Relay protocol bytes, including 12-byte packet headers.\n"
            "Video: received. Control: received + sent. Updated every second.\n"
            "Excludes TCP/IP headers, retransmissions and incomplete packets.\n"
            "Unavailable for local capture, while disconnected, or before the first interval.");
        ImGui::Text("%s | Capture: %s | %s: %s | USB: %s | Input: %s",
            remote ? "Remote" : "Local", capture_state(snapshot.capture.state), remote ? "Relay" : "Serial",
            control_state(snapshot.control.state), snapshot.control.target_usb_ready ? "ready" : "not ready",
            input_state(snapshot.input_state));
        if (ImGui::IsItemHovered() && (!snapshot.capture.error.empty() || !snapshot.control.error.empty() || !snapshot.session_error.empty()))
            ImGui::SetTooltip("Capture: %s\nControl: %s\nSession: %s", snapshot.capture.error.c_str(),
                snapshot.control.error.c_str(), snapshot.session_error.c_str());
        ImGui::EndChild();
        ImGui::End();
        ImGui::PopStyleVar(2);
        if (diagnostics_open) {
            const auto d = diagnostics.snapshot();
            ImGui::Begin("Diagnostics", &diagnostics_open);
            ImGui::Text("Decoded video resolution: %s", resolution.c_str());
            ImGui::TextWrapped("Capture mode: %s", d.capture_mode.c_str());
            ImGui::Text("Decode %.1f fps  Present %.1f fps", d.decode_fps, d.unique_present_fps);
            ImGui::Text("Sample/frame overwrites: %llu / %llu", static_cast<unsigned long long>(d.sample_mailbox_overwrites), static_cast<unsigned long long>(d.frame_mailbox_overwrites));
            ImGui::Text("ACK %.2f ms  timeouts %llu", d.ack_rtt.latest_ms, static_cast<unsigned long long>(d.timeouts));
            ImGui::Text("Pixel path: %s", d.pixel_path.c_str());
            ImGui::TextWrapped("%s", d.recent_error.c_str());
            ImGui::End();
        }
        ImGui::Render(); int dw{}, dh{}; SDL_GetWindowSizeInPixels(window, &dw, &dh); glViewport(0,0,dw,dh); glClearColor(.05F,.05F,.06F,1); glClear(GL_COLOR_BUFFER_BIT); ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); const auto before = std::chrono::steady_clock::now(); SDL_GL_SwapWindow(window); diagnostics.record_present_blocking(std::chrono::steady_clock::now() - before);
    }
    session->shutdown(); if (pref) { int w{}, h{}; SDL_GetWindowSize(window, &w, &h); config.window.width = w; config.window.height = h; try { save_config(*pref, config); } catch (...) {} }
    renderer.destroy(); ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplSDL3_Shutdown(); ImGui::DestroyContext(); SDL_GL_DestroyContext(context); SDL_DestroyWindow(window); SDL_Quit(); return 0;
}
