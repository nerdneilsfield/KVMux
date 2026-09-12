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
#include <cstdio>
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

const char* decoder_backend_label(CodecBackend backend) {
    switch (backend) {
    case CodecBackend::automatic: return "Auto";
    case CodecBackend::videotoolbox: return "VideoToolbox";
    case CodecBackend::ffmpeg_software: return "FFmpeg software";
    case CodecBackend::jetson_gstreamer: return "Jetson GStreamer";
    }
    return "Unknown";
}
const char* capture_state(CaptureState state) {
    switch (state) { case CaptureState::stopped: return "Stopped"; case CaptureState::starting: return "Starting"; case CaptureState::streaming: return "Streaming"; case CaptureState::stopping: return "Stopping"; case CaptureState::permission_denied: return "Permission denied"; case CaptureState::fault: return "Fault"; }
    return "Unknown";
}
const char* control_state(ControlConnectionState state) {
    switch (state) { case ControlConnectionState::disconnected: return "Disconnected"; case ControlConnectionState::opening: return "Opening"; case ControlConnectionState::monitoring: return "USB not ready"; case ControlConnectionState::clearing: return "Clearing input"; case ControlConnectionState::ready: return "Ready"; case ControlConnectionState::stalled: return "Stalled"; case ControlConnectionState::reconnecting: return "Reconnecting"; case ControlConnectionState::fault: return "Fault"; case ControlConnectionState::stopping: return "Stopping"; }
    return "Unknown";
}
const char* input_state(InputState state) {
    switch (state) { case InputState::preview: return "Preview"; case InputState::arming: return "Arming"; case InputState::captured: return "Captured"; case InputState::recovering: return "Recovering"; case InputState::releasing: return "Releasing"; case InputState::fault: return "Fault"; }
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
    ImGui::GetStyle().FramePadding = {6.F, 3.F};
    ImGui::GetStyle().ItemSpacing = {6.F, 4.F};
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
    bool show_status = true, chrome_visible = true, was_captured = false;
    double chrome_until = 0.0, captured_at = 0.0;
    bool close_connections_when_ready = false;
    bool popup_open = false;
    std::vector<ImVec4> local_regions;
    Uint32 local_buttons = 0;
    auto next_serial_scan = std::chrono::steady_clock::now();
    std::optional<VideoFrame> current_frame;
    std::string last_status;

    while (running) {
        std::erase_if(retired_sessions, [](auto& future) {
            return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        });
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            const auto event_state = session->snapshot().input_state;
            const bool remote_input = event_state == InputState::captured ||
                event_state == InputState::recovering || event_state == InputState::arming;
            // Local overlays must not activate capture through the video underneath.
            // Once armed, never let ImGui consume remote edges (including Host release).
            bool local_click = false;
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                const Uint32 bit = SDL_BUTTON_MASK(event.button.button);
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event_state == InputState::preview) {
                    local_click = popup_open || std::any_of(local_regions.begin(), local_regions.end(), [&](const ImVec4& r) {
                        return event.button.x >= r.x && event.button.x < r.z &&
                            event.button.y >= r.y && event.button.y < r.w;
                    });
                    if (local_click) local_buttons |= bit;
                } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && (local_buttons & bit)) {
                    local_buttons &= ~bit;
                    local_click = true;
                }
            }
            if (!remote_input) ImGui_ImplSDL3_ProcessEvent(&event);
            if (local_click && !remote_input) continue;
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
        if (current_frame && current_frame->generation != session->snapshot().capture.generation) current_frame.reset();
        if (current_frame && renderer.upload(*current_frame, config.color_override)) { session->video_presented(current_frame->generation, current_frame->sequence); diagnostics.record_sample_to_gpu_submit(std::chrono::steady_clock::now() - current_frame->arrival); diagnostics.record_present(current_frame->generation, current_frame->sequence); }
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
        local_regions.clear();
        const auto record_local_region = [&] {
            const auto pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
            local_regions.push_back({pos.x, pos.y, pos.x + size.x, pos.y + size.y});
        };
        const bool captured = snapshot.input_state == InputState::captured || snapshot.input_state == InputState::recovering;
        const bool remote_input = captured || snapshot.input_state == InputState::arming;
        const double now = ImGui::GetTime();
        if (captured && !was_captured) captured_at = now;
        if (remote_input) {
            ImGui::GetIO().ClearInputKeys();
            ImGui::GetIO().ClearEventsQueue();
            chrome_until = 0.0;
        }
        was_captured = captured;
        const bool relative_capture = captured && config.mouse_mode == MouseMode::relative;
        if (SDL_GetWindowRelativeMouseMode(window) != relative_capture)
            SDL_SetWindowRelativeMouseMode(window, relative_capture);
        const auto mouse = ImGui::GetIO().MousePos;
        const bool at_top = mouse.x >= viewport->Pos.x && mouse.x < viewport->Pos.x + viewport->Size.x &&
            mouse.y >= viewport->Pos.y && mouse.y < viewport->Pos.y + 8.F;
        if (!remote_input && at_top) chrome_until = now + 1.0;
        chrome_visible = !remote_input && (!fullscreen || now < chrome_until ||
            ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId));
        const auto overlay_flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.F);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.F);
        if (chrome_visible) {
            ImGui::SetNextWindowPos(viewport->Pos);
            ImGui::SetNextWindowSize({viewport->Size.x, ImGui::GetFrameHeight()});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, {0.F, 0.F});
            ImGui::Begin("Chrome", nullptr, overlay_flags | ImGuiWindowFlags_MenuBar |
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::PopStyleVar();
            record_local_region();
            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) chrome_until = now + 1.0;
            if (ImGui::BeginMenuBar()) {
                if (ImGui::MenuItem("Connections")) ImGui::OpenPopup("Connections");
                if (ImGui::MenuItem(fullscreen ? "Exit fullscreen" : "Fullscreen")) {
                    fullscreen = !fullscreen; SDL_SetWindowFullscreen(window, fullscreen);
                }
                if (ImGui::MenuItem("Diagnostics")) diagnostics_open = !diagnostics_open;
                ImGui::MenuItem("Status overlay", nullptr, &show_status);
                ImGui::SetNextWindowPos({viewport->Pos.x + 8.F, viewport->Pos.y + ImGui::GetFrameHeight()});
                ImGui::SetNextWindowSize({std::min(900.F, viewport->Size.x - 16.F), 0.F});
                if (ImGui::BeginPopup("Connections")) {
                    record_local_region();
                    if (close_connections_when_ready && snapshot.video_fresh &&
                        snapshot.control.state == ControlConnectionState::ready) {
                        ImGui::CloseCurrentPopup();
                        close_connections_when_ready = false;
                    }
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
                        if (ImGui::BeginCombo("Decode", decoder_backend_label(config.decoder_backend))) {
                            for (const auto backend : {CodecBackend::automatic, CodecBackend::videotoolbox,
                                                       CodecBackend::ffmpeg_software}) {
                                if (ImGui::Selectable(decoder_backend_label(backend), config.decoder_backend == backend))
                                    config.decoder_backend = backend;
                            }
                            ImGui::EndCombo();
                        }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("H.265 decoder for the next connection. Auto prefers hardware, then falls back to CPU.\nExplicit backends do not fall back; MJPEG is unchanged.");
                        if (ImGui::Button("Connect relay") && retired_sessions.empty() && control_port > 0 && control_port <= 65535 && video_port > 0 && video_port <= 65535) {
                            close_connections_when_ready = true;
                            retire_session();
                            devices_future = {}; modes_future.reset();
                            devices.clear(); modes.clear(); selected_device = selected_mode = -1;
                            relay::ClientOptions client_options{
                                remote_host, static_cast<std::uint16_t>(control_port), static_cast<std::uint16_t>(video_port)};
                            client_options.decoder_backend = config.decoder_backend;
                            auto client = std::make_shared<relay::RelayClient>(std::move(client_options));
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
                        close_connections_when_ready = true;
                        (void)session->select_capture(devices[selected_device], modes[selected_mode]);
                        current_frame.reset(); renderer.destroy();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Connect") && selected_port >= 0) {
                        close_connections_when_ready = true;
                        (void)session->connect_control(ports[selected_port].name, config.serial_baud_rate, config.serial_address);
                    }
                    ImGui::SameLine(); if (ImGui::Button("Disconnect")) (void)session->disconnect_control();
                    }
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
                    ImGui::BeginDisabled(snapshot.input_state != InputState::preview);
                    ImGui::SetNextItemWidth(150.F);
                    const char* aspect_names[] = {"Full frame", "16:9", "16:10", "4:3"};
                    int aspect = static_cast<int>(config.target_aspect);
                    if (ImGui::Combo("Target aspect", &aspect, aspect_names, 4))
                        config.target_aspect = static_cast<TargetAspect>(aspect);
                    ImGui::EndDisabled();
                    ImGui::Checkbox("VSync", &config.vsync);
                    ImGui::SameLine(); ImGui::SetNextItemWidth(180.F);
                    const char* color_names[] = {"Color: automatic", "Color: BT.601 limited", "Color: BT.601 full", "Color: BT.709 limited", "Color: BT.709 full"};
                    int color = static_cast<int>(config.color_override);
                    if (ImGui::Combo("##color", &color, color_names, 5)) config.color_override = static_cast<ColorOverride>(color);
                    ImGui::SameLine(); ImGui::SetNextItemWidth(190.F);
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
                    ImGui::EndPopup();
                }
                ImGui::EndMenuBar();
            }
            ImGui::End();
        }
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.F, 0.F});
        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(viewport->Size);
        ImGui::Begin("KVMuxRoot", nullptr, overlay_flags | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoBringToFrontOnFocus);
        SDL_GL_SetSwapInterval(config.vsync ? 1 : 0);
        // Chrome overlays never change the video fit or the remote input mapping.
        const ImVec2 video_size = viewport->Size;
        const ImVec2 start = viewport->Pos;
        ImGui::GetWindowDrawList()->AddRectFilled(start, {start.x + video_size.x, start.y + video_size.y}, IM_COL32(0, 0, 0, 255));
        if (renderer.texture_id() && renderer.width() > 0) {
            const auto fit = fit_video_rect({start.x, start.y, video_size.x, video_size.y}, renderer.width(), renderer.height());
            session->set_video_rect(target_input_rect(fit, config.target_aspect));
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
        if (captured) {
            const float alpha = static_cast<float>(std::clamp(3.0 - (now - captured_at), 0.0, 1.0));
            const char* hint = config.host_scancode == 231 ? "Captured - Right GUI/Command releases control" :
                "Captured - Right Control releases control";
            const auto size = ImGui::CalcTextSize(hint);
            const ImVec2 pos{start.x + (video_size.x - size.x) * .5F, start.y + 36.F};
            auto* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled({pos.x - 8.F, pos.y - 4.F}, {pos.x + size.x + 8.F, pos.y + size.y + 4.F},
                IM_COL32(0, 0, 0, static_cast<int>(180.F * alpha)), 4.F);
            draw->AddText(pos, IM_COL32(255, 255, 255, static_cast<int>(255.F * alpha)), hint);
        }
        ImGui::End();
        ImGui::PopStyleVar();
        const auto d = diagnostics.snapshot();
        char rates[64];
        if (video_bytes_per_second && control_bytes_per_second)
            std::snprintf(rates, sizeof(rates), "V:%.2fM/s C:%.2fK/s",
                *video_bytes_per_second / (1024.0 * 1024.0), *control_bytes_per_second / 1024.0);
        else std::snprintf(rates, sizeof(rates), "V:-- C:--");
        // Layout may have cleared event-time diagnostics after the earlier session snapshot.
        const auto pointer_snapshot = session->snapshot().pointer;
        char pointer[96];
        if (pointer_snapshot.video_local)
            std::snprintf(pointer, sizeof(pointer), "P:%.0f,%.0f",
                pointer_snapshot.video_local->first, pointer_snapshot.video_local->second);
        else std::snprintf(pointer, sizeof(pointer), "P:--");
        char submitted[64];
        if (pointer_snapshot.submitted_absolute)
            std::snprintf(submitted, sizeof(submitted), "H:%u,%u",
                static_cast<unsigned>(pointer_snapshot.submitted_absolute->first),
                static_cast<unsigned>(pointer_snapshot.submitted_absolute->second));
        else if (pointer_snapshot.submitted_relative)
            std::snprintf(submitted, sizeof(submitted), "d:%d,%d",
                pointer_snapshot.submitted_relative->first, pointer_snapshot.submitted_relative->second);
        else std::snprintf(submitted, sizeof(submitted), "H:--");
        char line[320];
        std::snprintf(line, sizeof(line), "%s | %s | %.1f/%.1f fps | %s | %s | %s > %s",
            remote ? "LAN" : "Local", resolution.c_str(), d.decode_fps, d.unique_present_fps,
            rates, input_state(snapshot.input_state), pointer, submitted);
        if (show_status) {
            const float status_height = ImGui::GetTextLineHeight() + 8.F;
            ImGui::SetNextWindowPos({viewport->Pos.x, viewport->Pos.y + viewport->Size.y - status_height});
            ImGui::SetNextWindowSize({viewport->Size.x, status_height});
            ImGui::SetNextWindowBgAlpha(.65F);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {6.F, 4.F});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, {0.F, 0.F});
            ImGui::Begin("Status", nullptr, overlay_flags | ImGuiWindowFlags_NoFocusOnAppearing |
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                (remote_input ? ImGuiWindowFlags_NoInputs : 0));
            ImGui::PopStyleVar();
            if (!remote_input) record_local_region();
            const auto origin = ImGui::GetCursorScreenPos();
            const float line_height = ImGui::GetTextLineHeight();
            const bool connected = snapshot.video_fresh && snapshot.control.state == ControlConnectionState::ready;
            const ImU32 state_color = connected ? IM_COL32(80, 210, 120, 255) : IM_COL32(230, 155, 70, 255);
            auto* draw = ImGui::GetWindowDrawList();
            draw->AddCircleFilled({origin.x + 4.F, origin.y + line_height * .5F}, 3.F, state_color);
            const float text_width = ImGui::CalcTextSize(line).x;
            const float room = std::max(1.F, ImGui::GetContentRegionAvail().x - 14.F);
            const float font_size = ImGui::GetFontSize() * std::min(1.F, room / std::max(1.F, text_width));
            draw->AddText(ImGui::GetFont(), font_size,
                {origin.x + 14.F, origin.y + (line_height - font_size) * .5F},
                ImGui::GetColorU32(ImGuiCol_Text), line);
            ImGui::Dummy({ImGui::GetContentRegionAvail().x, line_height});
            if (!remote_input && ImGui::IsItemHovered())
                ImGui::SetTooltip("Video: %s | Control: %s | USB: %s\n%s\n%s",
                    capture_state(snapshot.capture.state), control_state(snapshot.control.state),
                    snapshot.control.target_usb_ready ? "ready" : "not ready",
                    snapshot.capture.error.c_str(), snapshot.control.error.c_str());
            ImGui::End();
            ImGui::PopStyleVar();
        }
        ImGui::PopStyleVar(2);
        if (diagnostics_open && !remote_input) {
            const auto d = diagnostics.snapshot();
            ImGui::Begin("Diagnostics", &diagnostics_open);
            record_local_region();
            ImGui::Text("Decoded video resolution: %s", resolution.c_str());
            if (remote_client) {
                const auto video = remote_client->video_snapshot();
                ImGui::TextUnformatted("Relay transport: UDP v3 / KCP control");
                ImGui::Text("Relay codec: %s", video.codec == VideoCodec::hevc ? "H.265" : "MJPEG");
                ImGui::Text("Decoder backend: %s", video.decoder_backend
                    ? decoder_backend_label(*video.decoder_backend) : "-- (not active)");
                ImGui::Text("Hardware decoding: %s", !video.hardware_verified ? "unverified" :
                    video.hardware_active ? "active" : "not active");
                if (!video.decoder_diagnostic.empty())
                    ImGui::TextWrapped("Decoder detail: %s", video.decoder_diagnostic.c_str());
                ImGui::Text("Decoder recoveries: %llu", static_cast<unsigned long long>(video.recoveries));
                const auto& media = video.media;
                ImGui::Text("Media completed: %llu  lost: %llu  XOR fragments/frames: %llu/%llu",
                    static_cast<unsigned long long>(media.received_frames), static_cast<unsigned long long>(media.lost_frames),
                    static_cast<unsigned long long>(media.recovered_fragments), static_cast<unsigned long long>(media.recovered_frames));
                ImGui::Text("Media age/capacity/gap: %llu/%llu/%llu  waiting IDR: %s",
                    static_cast<unsigned long long>(media.age_losses), static_cast<unsigned long long>(media.capacity_losses),
                    static_cast<unsigned long long>(media.gap_losses), media.waiting_idr ? "yes" : "no");
                ImGui::Text("Media recovery reason: %s", relay::media_reason_name(video.last_recovery_reason));
                if (!video.error.empty()) ImGui::TextWrapped("Decoder error: %s", video.error.c_str());
            }
            int logical_width{}, logical_height{}, pixel_width{}, pixel_height{};
            SDL_GetWindowSize(window, &logical_width, &logical_height);
            SDL_GetWindowSizeInPixels(window, &pixel_width, &pixel_height);
            ImGui::Text("Window logical: %dx%d  Framebuffer: %dx%d",
                logical_width, logical_height, pixel_width, pixel_height);
            if (pointer_snapshot.video_rect && pointer_snapshot.video_local) {
                const auto& rect = *pointer_snapshot.video_rect;
                const auto [x, y] = *pointer_snapshot.video_local;
                ImGui::Text("Event active rect: x=%.2f y=%.2f w=%.2f h=%.2f",
                    rect.x, rect.y, rect.width, rect.height);
                ImGui::Text("Event window: %.2f,%.2f  Desktop local: %.2f,%.2f",
                    x + rect.x, y + rect.y, x, y);
            } else {
                ImGui::TextUnformatted("Event geometry: -- (move pointer while captured)");
            }
            ImGui::Text("Submitted (queue accepted, not ACK): %s", submitted);
            ImGui::TextWrapped("Capture mode: %s", d.capture_mode.c_str());
            ImGui::Text("Decode %.1f fps  Present %.1f fps", d.decode_fps, d.unique_present_fps);
            ImGui::Text("Sample/frame overwrites: %llu / %llu", static_cast<unsigned long long>(d.sample_mailbox_overwrites), static_cast<unsigned long long>(d.frame_mailbox_overwrites));
            ImGui::Text("ACK %.2f ms  timeouts %llu", d.ack_rtt.latest_ms, static_cast<unsigned long long>(d.timeouts));
            ImGui::Text("Pixel path: %s", d.pixel_path.c_str());
            ImGui::TextWrapped("%s", d.recent_error.c_str());
            ImGui::End();
        }
        popup_open = !remote_input && ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
        ImGui::Render(); int dw{}, dh{}; SDL_GetWindowSizeInPixels(window, &dw, &dh); glViewport(0,0,dw,dh); glClearColor(.05F,.05F,.06F,1); glClear(GL_COLOR_BUFFER_BIT); ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); const auto before = std::chrono::steady_clock::now(); SDL_GL_SwapWindow(window); diagnostics.record_present_blocking(std::chrono::steady_clock::now() - before);
    }
    session->shutdown(); if (pref) { int w{}, h{}; SDL_GetWindowSize(window, &w, &h); config.window.width = w; config.window.height = h; try { save_config(*pref, config); } catch (...) {} }
    renderer.destroy(); ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplSDL3_Shutdown(); ImGui::DestroyContext(); SDL_GL_DestroyContext(context); SDL_DestroyWindow(window); SDL_Quit(); return 0;
}
