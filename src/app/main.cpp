#include <SDL3/SDL.h>
#include <glad/gl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/event_router.hpp"
#include "app/kvm_session.hpp"
#include "app/recording.hpp"
#include "app/ui_panels.hpp"
#include "app/ui_state.hpp"
#include "network/relay_client.hpp"
#include "network/relay_selection.hpp"
#include "render/video_renderer.hpp"
#include "support/config.hpp"
#include "support/diagnostics.hpp"

namespace {
using namespace kvmux;

struct TrafficRate {
  std::optional<relay::TrafficSnapshot> baseline;
  std::chrono::steady_clock::time_point sampled_at{};
  std::optional<double> bytes_per_second;
  void clear() noexcept {
    baseline.reset();
    bytes_per_second.reset();
  }
};

constexpr ImVec4 brand_color(unsigned rgb, float alpha = 1.F) {
  return {static_cast<float>((rgb >> 16) & 255) / 255.F,
          static_cast<float>((rgb >> 8) & 255) / 255.F,
          static_cast<float>(rgb & 255) / 255.F, alpha};
}
void apply_brand_style() {
  ImGui::StyleColorsDark();
  auto& style = ImGui::GetStyle();
  style.WindowRounding = 6.F;
  style.PopupRounding = 6.F;
  style.FrameRounding = 4.F;
  style.GrabRounding = 3.F;
  style.WindowBorderSize = 1.F;
  style.PopupBorderSize = 1.F;
  style.FramePadding = {6.F, 3.F};
  style.ItemSpacing = {6.F, 4.F};
  auto& c = style.Colors;
  const auto navy = brand_color(0x0B1823), panel = brand_color(0x202A33);
  const auto silver = brand_color(0xBFC5C9), white = brand_color(0xF3F1EB);
  const auto copper = brand_color(0xC45F3C), highlight = brand_color(0xED9464);
  const auto hover = brand_color(0x633A2E), active = brand_color(0x84442E);
  c[ImGuiCol_Text] = white;
  c[ImGuiCol_TextDisabled] = silver;
  c[ImGuiCol_WindowBg] = navy;
  c[ImGuiCol_ChildBg] = navy;
  c[ImGuiCol_PopupBg] = navy;
  c[ImGuiCol_MenuBarBg] = panel;
  c[ImGuiCol_Border] = brand_color(0x576570);
  c[ImGuiCol_BorderShadow] = brand_color(0, 0.F);
  c[ImGuiCol_FrameBg] = panel;
  c[ImGuiCol_FrameBgHovered] = hover;
  c[ImGuiCol_FrameBgActive] = active;
  c[ImGuiCol_TitleBg] = navy;
  c[ImGuiCol_TitleBgActive] = panel;
  c[ImGuiCol_TitleBgCollapsed] = navy;
  c[ImGuiCol_Button] = panel;
  c[ImGuiCol_ButtonHovered] = hover;
  c[ImGuiCol_ButtonActive] = active;
  c[ImGuiCol_Header] = panel;
  c[ImGuiCol_HeaderHovered] = hover;
  c[ImGuiCol_HeaderActive] = active;
  c[ImGuiCol_CheckMark] = highlight;
  c[ImGuiCol_SliderGrab] = copper;
  c[ImGuiCol_SliderGrabActive] = highlight;
  c[ImGuiCol_ScrollbarBg] = navy;
  c[ImGuiCol_ScrollbarGrab] = brand_color(0x576570);
  c[ImGuiCol_ScrollbarGrabHovered] = silver;
  c[ImGuiCol_ScrollbarGrabActive] = copper;
  c[ImGuiCol_Separator] = brand_color(0x576570);
  c[ImGuiCol_SeparatorHovered] = copper;
  c[ImGuiCol_SeparatorActive] = highlight;
  c[ImGuiCol_ResizeGrip] = brand_color(0xBFC5C9, .3F);
  c[ImGuiCol_ResizeGripHovered] = copper;
  c[ImGuiCol_ResizeGripActive] = highlight;
  c[ImGuiCol_Tab] = panel;
  c[ImGuiCol_TabHovered] = hover;
  c[ImGuiCol_TabSelected] = active;
  c[ImGuiCol_TabSelectedOverline] = copper;
  c[ImGuiCol_TabDimmed] = navy;
  c[ImGuiCol_TabDimmedSelected] = panel;
  c[ImGuiCol_TabDimmedSelectedOverline] = silver;
  c[ImGuiCol_TextSelectedBg] = active;
  c[ImGuiCol_NavCursor] = highlight;
  c[ImGuiCol_PlotLines] = silver;
  c[ImGuiCol_PlotLinesHovered] = highlight;
  c[ImGuiCol_PlotHistogram] = copper;
  c[ImGuiCol_PlotHistogramHovered] = highlight;
  c[ImGuiCol_TableHeaderBg] = panel;
  c[ImGuiCol_TableBorderStrong] = brand_color(0x576570);
  c[ImGuiCol_TableBorderLight] = panel;
}

GLuint load_brand_texture() {
  const char* base = SDL_GetBasePath();
  if (!base) return 0;
  const std::string path = std::string(base) + "assets/branding/logo.png";
  SDL_Surface* loaded = SDL_LoadPNG(path.c_str());
  if (!loaded) {
    spdlog::warn("Cannot load brand icon: {}", SDL_GetError());
    return 0;
  }
  SDL_Surface* rgba = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
  SDL_DestroySurface(loaded);
  if (!rgba) {
    spdlog::warn("Cannot convert brand icon: {}", SDL_GetError());
    return 0;
  }
  GLuint texture{};
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                  GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, rgba->pitch / 4);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rgba->w, rgba->h, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, rgba->pixels);
  glGenerateMipmap(GL_TEXTURE_2D);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
  SDL_DestroySurface(rgba);
  return texture;
}

std::optional<std::filesystem::path> preference_file() {
  char* raw = SDL_GetPrefPath("kvmux", "kvmux");
  if (!raw) return std::nullopt;
  std::filesystem::path result(raw);
  SDL_free(raw);
  return result / "config.json";
}

void copy_host(char* dest, std::size_t size, const std::string& value) {
  if (size == 0) return;
  const auto count = std::min(size - 1, value.size());
  std::memcpy(dest, value.data(), count);
  dest[count] = '\0';
}

bool same_saved_mode(const CaptureMode& saved, const CaptureMode& listed) {
  return saved.width == listed.width && saved.height == listed.height &&
         saved.frame_rate == listed.frame_rate &&
         saved.device_format == listed.device_format &&
         saved.delivered_format == listed.delivered_format;
}

void store_connection(Config& config, const ConnectionForm& form) {
  config.video_source = form.video_source;
  config.control_target = form.control_target;
  config.video_host = form.video_host;
  config.control_host = form.control_host;
  config.video_control_port = form.video_control_port;
  config.video_port = form.video_port;
  config.control_relay_port = form.control_relay_port;
  if (form.selected_device >= 0 &&
      form.selected_device < static_cast<int>(form.devices.size())) {
    const auto& device =
        form.devices[static_cast<std::size_t>(form.selected_device)];
    config.capture_stable_id = device.stable_id;
    if (form.selected_mode >= 0 &&
        form.selected_mode < static_cast<int>(form.modes.size())) {
      config.capture_mode =
          form.modes[static_cast<std::size_t>(form.selected_mode)];
    }
    config.capture_mode.device_id = config.capture_stable_id;
  }
  if (form.selected_port >= 0 &&
      form.selected_port < static_cast<int>(form.ports.size())) {
    config.serial_port =
        form.ports[static_cast<std::size_t>(form.selected_port)].name;
  }
}

void load_connection(ConnectionForm& form, const Config& config) {
  form.video_source = config.video_source;
  form.control_target = config.control_target;
  form.draft_video = config.video_source;
  form.draft_control = config.control_target;
  copy_host(form.video_host, sizeof(form.video_host), config.video_host);
  copy_host(form.control_host, sizeof(form.control_host), config.control_host);
  form.video_control_port = config.video_control_port;
  form.video_port = config.video_port;
  form.control_relay_port = config.control_relay_port;
  form.capture_only = config.control_target == ControlTarget::none;
}

}  // namespace

int main(int argc, char** argv) {
  bool debug = false, help = false;
  std::string log_file;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    if (argument == "--debug")
      debug = true;
    else if (argument == "--help")
      help = true;
    else if (argument == "--log-file") {
      if (i + 1 >= argc || std::string_view(argv[i + 1]).starts_with("--")) {
        std::cerr << "kvmux: --log-file requires a path\n";
        return 2;
      }
      if (!log_file.empty()) {
        std::cerr << "kvmux: Duplicate --log-file\n";
        return 2;
      }
      log_file = argv[++i];
      if (log_file.empty()) {
        std::cerr << "kvmux: --log-file requires a path\n";
        return 2;
      }
    } else {
      std::cerr << "kvmux: Unknown argument: " << argument
                << "\nUsage: kvmux [--debug] [--log-file PATH] [--help]\n";
      return 2;
    }
  }
  if (!configure_console_logging(debug, log_file, "kvmux")) return 2;
  if (help) {
    std::cout
        << "Usage: kvmux [--debug] [--log-file PATH] [--help]\n"
           "  --debug     Write diagnostic logs to stderr (time, thread, "
           "level)\n"
           "  --log-file  Append the same log to PATH (10 MiB, 3 rotations)\n"
           "  --help      Show this help without starting the GUI\n";
    return 0;
  }
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
    spdlog::error("SDL initialization failed: {}", SDL_GetError());
    return 1;
  }
  auto pref = preference_file();
  Config config = pref ? load_config(*pref) : Config{};
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#ifdef __APPLE__
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS,
                      SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
  SDL_Window* window = SDL_CreateWindow(
      "KVMux", config.window.width, config.window.height,
      SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window) {
    SDL_Quit();
    return 1;
  }
  SDL_SetWindowMinimumSize(window, 960, 640);
  if (config.window.maximized) SDL_MaximizeWindow(window);
  SDL_GLContext context = SDL_GL_CreateContext(window);
  if (!context ||
      !gladLoadGL(reinterpret_cast<GLADloadfunc>(SDL_GL_GetProcAddress))) {
    if (context) SDL_GL_DestroyContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  SDL_GL_SetSwapInterval(config.vsync ? 1 : 0);
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  apply_brand_style();
  const GLuint brand_texture = load_brand_texture();
  ImGui_ImplSDL3_InitForOpenGL(window, context);
  ImGui_ImplOpenGL3_Init("#version 150");

  auto diagnostics = std::make_shared<Diagnostics>();
  auto session = std::make_unique<KvmSession>(diagnostics);
  Recording recording;
  UiState ui;
  ui.config = &config;
  ui.recording = &recording;
  ui.window = window;
  ui.debug = debug;
  ui.brand_texture = brand_texture;
  EventHost host;
  host.diagnostics = diagnostics.get();
  host.window = window;
  host.main_window_id = SDL_GetWindowID(window);
  (void)session->set_mouse_mode(config.mouse_mode);
  session->set_host_key(config.host_scancode);
  session->set_relative_gain(config.sensitivity);
  session->set_keep_alive(config.keep_alive);
  std::shared_ptr<relay::RelayClient> video_client;
  std::shared_ptr<relay::RelayClient> control_client;
  TrafficRate video_rate, control_rate;
  std::vector<std::future<void>> retired_sessions;
  auto retire_session = [&] {
    video_client.reset();
    control_client.reset();
    video_rate.clear();
    control_rate.clear();
    auto old = std::move(session);
    retired_sessions.push_back(
        std::async(std::launch::async, [old = std::move(old)]() mutable {
          old->shutdown();
          old.reset();
        }));
  };
  VideoRenderer renderer;
  UploadedFrame primary_uploaded;
  const auto drop_primary_texture = [&] {
    renderer.destroy();
    primary_uploaded = {};
  };
  SDL_Window* window2 = nullptr;
  SDL_WindowID second_window_id = 0;
  std::unique_ptr<KvmSession> session2;
  VideoRenderer renderer2;
  UploadedFrame second_uploaded;
  std::optional<VideoFrame> second_frame;
  std::optional<std::future<std::vector<CaptureMode>>> modes2_future;
  bool second_valid = false;
  bool second_upload_failed = false, second_display_failed = false;
  std::chrono::steady_clock::time_point second_uploaded_at{};
  const auto close_second = [&] {
    if (!window2) return;
    if (session->text_paste_active()) log_unknown_paste_cancel("close_second");
    session->release_control(PasteCancelReason::unknown);
    auto old = std::move(session2);
    retired_sessions.push_back(
        std::async(std::launch::async, [old = std::move(old)]() mutable {
          old->shutdown();
          old.reset();
        }));
    renderer2.destroy();
    second_uploaded = {};
    second_frame.reset();
    SDL_DestroyWindow(window2);
    window2 = nullptr;
    second_window_id = 0;
    ui.connection.second_device_id.clear();
    ui.connection.screen2_open = false;
    second_valid = host.second_focused = false;
    second_upload_failed = second_display_failed = false;
    host.remote_buttons = 0;
    host.close_second_requested = false;
    ui.connection.modes2.clear();
    modes2_future.reset();
    ui.connection.selected_mode2 = -1;
  };
  std::future<std::vector<DeviceInfo>> devices_future =
      session->enumerate_capture_devices();
  std::optional<std::future<std::vector<CaptureMode>>> modes_future;
  std::optional<VideoFrame> current_frame;
  std::optional<std::pair<std::uint64_t, std::uint64_t>> recorded_frame;
  const auto rebuild_local_session = [&] {
    (void)recording.stop();
    if (ui.media.scroll_active) {
      recording.cancel_scroll();
      ui.media.scroll_active = false;
      ui.media.scroll_sample_after.reset();
    }
    close_second();
    retire_session();
    ui.connection.capture_only =
        ui.connection.control_target == ControlTarget::none;
    diagnostics = std::make_shared<Diagnostics>();
    session = std::make_unique<KvmSession>(diagnostics);
    ui.connection.devices.clear();
    ui.connection.modes.clear();
    ui.connection.selected_device = ui.connection.selected_mode = -1;
    modes_future.reset();
    devices_future = session->enumerate_capture_devices();
    current_frame.reset();
    drop_primary_texture();
    (void)session->set_mouse_mode(config.mouse_mode);
    session->set_capture_only(ui.connection.capture_only);
    session->set_host_key(config.host_scancode);
    session->set_keep_alive(config.keep_alive);
  };
  const auto connect_relay = [&] {
    auto& form = ui.connection;
    const bool want_video = form.video_source == VideoSource::relay;
    const bool want_control = form.control_target == ControlTarget::relay;
    const bool same = want_video && want_control &&
                      std::strcmp(form.video_host, form.control_host) == 0 &&
                      form.video_control_port == form.control_relay_port;
    std::shared_ptr<relay::RelayClient> new_video;
    std::shared_ptr<relay::RelayClient> new_control;
    const auto make_client = [&](relay::wire::RelayRole role, const char* host,
                                 int control, int video) {
      relay::ClientOptions options{host, static_cast<std::uint16_t>(control),
                                   static_cast<std::uint16_t>(video)};
      options.decoder_backend = config.decoder_backend;
      options.role = role;
      return std::make_shared<relay::RelayClient>(std::move(options));
    };
    if (same) {
      new_video = new_control =
          make_client(relay::wire::RelayRole::both, form.video_host,
                      form.video_control_port, form.video_port);
    } else {
      if (want_video)
        new_video = make_client(relay::wire::RelayRole::video, form.video_host,
                                form.video_control_port, form.video_port);
      if (want_control)
        new_control =
            make_client(relay::wire::RelayRole::control, form.control_host,
                        form.control_relay_port, 0);
    }
    std::unique_ptr<relay::NetworkCaptureSource> network_capture;
    DeviceInfo relay_device;
    CaptureMode relay_mode;
    if (new_video) {
      network_capture =
          std::make_unique<relay::NetworkCaptureSource>(new_video);
      const auto relay_devices = network_capture->enumerate_devices();
      if (relay_devices.empty()) {
        ui.media.media_message = "Relay returned no capture devices.";
        return;
      }
      relay_device = relay_devices.front();
      const auto relay_modes =
          network_capture->enumerate_modes(relay_device.stable_id);
      if (relay_modes.empty()) {
        ui.media.media_message = "Relay returned no capture modes.";
        return;
      }
      relay_mode = relay_modes.front();
    }
    form.close_connections_when_ready = true;
    close_second();
    retire_session();
    video_client = std::move(new_video);
    control_client = std::move(new_control);
    diagnostics = std::make_shared<Diagnostics>();
    std::unique_ptr<CaptureSource> capture =
        video_client
            ? std::unique_ptr<CaptureSource>(std::move(network_capture))
            : create_platform_capture_source();
    std::unique_ptr<ControlSink> sink =
        control_client
            ? std::unique_ptr<ControlSink>(
                  std::make_unique<relay::NetworkControlSink>(control_client))
            : std::unique_ptr<ControlSink>(
                  std::make_unique<Ch9329ControlSink>(diagnostics));
    devices_future = {};
    modes_future.reset();
    form.devices.clear();
    form.modes.clear();
    form.selected_device = form.selected_mode = -1;
    session = std::make_unique<KvmSession>(std::move(capture), std::move(sink),
                                           diagnostics);
    session->set_host_key(config.host_scancode);
    (void)session->set_mouse_mode(config.mouse_mode);
    session->set_keep_alive(config.keep_alive);
    form.capture_only = form.control_target == ControlTarget::none;
    session->set_capture_only(form.capture_only);
    if (video_client)
      (void)session->select_capture(relay_device, relay_mode);
    else
      devices_future = session->enumerate_capture_devices();
    if (control_client && control_client != video_client)
      control_client->start();
    current_frame.reset();
    drop_primary_texture();
  };
  const auto disconnect_relay = [&] {
    session->release_control(PasteCancelReason::link_lost);
    if (video_client) {
      (void)session->stop_capture();
      video_rate.clear();
      current_frame.reset();
      drop_primary_texture();
    }
    if (control_client) {
      (void)session->disconnect_control();
      control_rate.clear();
    }
    video_client.reset();
    control_client.reset();
  };
  const auto open_screen2 = [&] {
    auto& form = ui.connection;
    const auto device_index = static_cast<std::size_t>(form.selected_device2);
    const auto mode_index = static_cast<std::size_t>(form.selected_mode2);
    window2 = SDL_CreateWindow("KVMux - Screen 2", 960, 540,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                                   SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window2) {
      ui.media.media_message = "Could not open the second window.";
      return;
    }
    second_window_id = SDL_GetWindowID(window2);
    form.second_device_id = form.devices[device_index].stable_id;
    form.screen2_open = true;
    session2 = std::make_unique<KvmSession>();
    session2->set_capture_only(true);
    (void)session2->select_capture(form.devices[device_index],
                                   form.modes2[mode_index]);
  };
  load_connection(ui.connection, config);
  session->set_capture_only(ui.connection.capture_only);
  bool restore_capture = config.video_source == VideoSource::local &&
                         !config.capture_stable_id.empty() &&
                         config.capture_mode.width > 0;
  bool restore_mode = false;
  if (config.video_source == VideoSource::relay ||
      config.control_target == ControlTarget::relay) {
    connect_relay();
    if (ui.connection.video_source != VideoSource::local)
      restore_capture = false;
  }
  if (config.auto_connect_serial &&
      config.control_target == ControlTarget::local_serial &&
      !config.serial_port.empty()) {
    (void)session->connect_control(config.serial_port, config.serial_baud_rate,
                                   config.serial_address);
  }
  const auto apply = [&](const UiActions& actions) {
    for (const auto& item : actions.items) {
      switch (item.kind) {
        case UiActionKind::set_keep_alive:
          session->set_keep_alive(config.keep_alive);
          break;
        case UiActionKind::rebuild_local:
          rebuild_local_session();
          break;
        case UiActionKind::connect_relay:
          connect_relay();
          break;
        case UiActionKind::disconnect_relay:
          disconnect_relay();
          break;
        case UiActionKind::enumerate_modes:
          modes_future = session->enumerate_capture_modes(item.text);
          break;
        case UiActionKind::enumerate_screen2_modes:
          modes2_future = session->enumerate_capture_modes(item.text);
          break;
        case UiActionKind::start_preview:
          (void)session->select_capture(
              ui.connection.devices[static_cast<std::size_t>(
                  ui.connection.selected_device)],
              ui.connection.modes[static_cast<std::size_t>(
                  ui.connection.selected_mode)]);
          current_frame.reset();
          drop_primary_texture();
          break;
        case UiActionKind::connect_serial:
          (void)session->connect_control(
              ui.connection
                  .ports[static_cast<std::size_t>(ui.connection.selected_port)]
                  .name,
              config.serial_baud_rate, config.serial_address);
          break;
        case UiActionKind::disconnect_serial:
          (void)session->disconnect_control();
          break;
        case UiActionKind::open_screen2:
          open_screen2();
          break;
        case UiActionKind::close_screen2:
          close_second();
          break;
        case UiActionKind::set_mouse_mode:
          (void)session->set_mouse_mode(item.mouse);
          break;
        case UiActionKind::set_relative_gain:
          session->set_relative_gain(item.gain);
          break;
        case UiActionKind::send_special:
          (void)session->send_special(item.special);
          break;
        case UiActionKind::set_host_key:
          session->set_host_key(item.usage);
          break;
        case UiActionKind::cancel_text_paste:
          session->cancel_text_paste();
          break;
        case UiActionKind::start_text_paste: {
          const auto result = session->start_text_paste(item.text);
          ui.paste.error = result.error;
          ui.paste.cancel_reason = result.cancel_reason;
          if (result) {
            ui.paste.buffer.fill('\0');
            ui.paste.removed = 0;
          }
          break;
        }
        case UiActionKind::activate_control:
          session->activate_control();
          break;
        case UiActionKind::set_video_rect:
          session->set_video_rect(item.video_rect);
          break;
      }
    }
  };

  while (host.running) {
    const auto frame_started = std::chrono::steady_clock::now();
    std::erase_if(retired_sessions, [](auto& future) {
      return future.wait_for(std::chrono::seconds(0)) ==
             std::future_status::ready;
    });
    host.session = session.get();
    host.diagnostics = diagnostics.get();
    host.window2 = window2;
    host.second_window_id = second_window_id;
    host.second_valid = second_valid;
    host.current_frame = current_frame ? &*current_frame : nullptr;
    host.renderer_width = renderer.width();
    host.renderer_height = renderer.height();
    SDL_Event event;
    while (SDL_PollEvent(&event)) route_sdl_event(event, host, ui);
    settle_window_focus(host);
    auto& form = ui.connection;
    if (devices_future.valid() && devices_future.wait_for(std::chrono::seconds(
                                      0)) == std::future_status::ready) {
      form.devices = devices_future.get();
      int matched_device = -1;
      const bool restoring_device = restore_capture;
      if (restore_capture) {
        restore_capture = false;
        for (int i = 0; i < static_cast<int>(form.devices.size()); ++i) {
          if (form.devices[static_cast<std::size_t>(i)].stable_id ==
              config.capture_stable_id) {
            matched_device = i;
            break;
          }
        }
      }
      if (matched_device >= 0) {
        form.selected_device = matched_device;
        modes_future = session->enumerate_capture_modes(
            form.devices[static_cast<std::size_t>(matched_device)].stable_id);
        restore_mode = true;
      } else if (!restoring_device &&
                 (form.selected_device < 0 ||
                  form.selected_device >=
                      static_cast<int>(form.devices.size()))) {
        form.selected_device = form.devices.empty() ? -1 : 0;
      }
    }
    if (modes_future && modes_future->wait_for(std::chrono::seconds(0)) ==
                            std::future_status::ready) {
      form.modes = modes_future->get();
      modes_future.reset();
      int matched_mode = -1;
      const bool restoring_mode =
          restore_mode && form.selected_device >= 0 &&
          form.selected_device < static_cast<int>(form.devices.size()) &&
          form.devices[static_cast<std::size_t>(form.selected_device)]
                  .stable_id == config.capture_stable_id;
      restore_mode = false;
      if (restoring_mode) {
        for (int i = 0; i < static_cast<int>(form.modes.size()); ++i) {
          if (same_saved_mode(config.capture_mode,
                              form.modes[static_cast<std::size_t>(i)])) {
            matched_mode = i;
            break;
          }
        }
      }
      if (matched_mode >= 0) {
        form.selected_mode = matched_mode;
        const auto device_index =
            static_cast<std::size_t>(form.selected_device);
        form.primary_device_id = form.devices[device_index].stable_id;
        (void)session->select_capture(
            form.devices[device_index],
            form.modes[static_cast<std::size_t>(matched_mode)]);
        current_frame.reset();
        drop_primary_texture();
      } else if (!restoring_mode) {
        form.selected_mode = form.modes.empty() ? -1 : 0;
      }
    }
    if (form.control_target == ControlTarget::local_serial &&
        form.open_connections &&
        session->snapshot().input_state == InputState::preview &&
        std::chrono::steady_clock::now() >= form.next_serial_scan) {
      const std::string previous =
          form.selected_port >= 0 &&
                  form.selected_port < static_cast<int>(form.ports.size())
              ? form.ports[static_cast<std::size_t>(form.selected_port)].name
              : std::string{};
      form.ports = enumerate_serial_ports();
      form.selected_port = -1;
      std::string wanted = previous;
      if (wanted.empty()) {
        try {
          wanted = relay::select_serial(form.ports);
        } catch (const std::exception&) {
        }
      }
      for (int i = 0; i < static_cast<int>(form.ports.size()); ++i)
        if (form.ports[static_cast<std::size_t>(i)].name == wanted)
          form.selected_port = i;
      form.next_serial_scan =
          std::chrono::steady_clock::now() + std::chrono::seconds(1);
    }
    if (host.close_second_requested) close_second();
    if (modes2_future && modes2_future->wait_for(std::chrono::seconds(0)) ==
                             std::future_status::ready) {
      form.modes2 = modes2_future->get();
      form.selected_mode2 = form.modes2.empty() ? -1 : 0;
      modes2_future.reset();
    }
    if (session2) {
      session2->tick();
      if (auto newest = session2->take_latest_frame())
        second_frame = std::move(newest);
      if (second_frame &&
          second_frame->generation != session2->snapshot().capture.generation)
        second_frame.reset();
      if (second_frame) {
        const auto decision =
            frame_upload(second_uploaded, second_frame->generation,
                         second_frame->sequence, config.color_override);
        if (decision != FrameUpload::skip) {
          if (renderer2.upload(*second_frame, config.color_override)) {
            second_uploaded = {true, second_frame->generation,
                               second_frame->sequence, config.color_override};
            second_upload_failed = false;
            if (decision == FrameUpload::new_frame)
              second_uploaded_at = std::chrono::steady_clock::now();
          } else {
            second_upload_failed = true;
          }
        }
      }
      const bool fresh = session2->snapshot().video_fresh &&
                         renderer2.width() > 0 && !second_upload_failed &&
                         !second_display_failed &&
                         std::chrono::steady_clock::now() - second_uploaded_at <
                             std::chrono::milliseconds(500);
      if (second_valid && !fresh) {
        session->release_control(PasteCancelReason::video_stale);
        host.remote_buttons = 0;
      }
      second_valid = fresh;
    }
    session->tick();
    if (auto newest = session->take_latest_frame()) {
      current_frame = std::move(newest);
      diagnostics->record_decode(current_frame->decoded);
    }
    if (current_frame &&
        current_frame->generation != session->snapshot().capture.generation)
      current_frame.reset();
    if (ui.media.scroll_active && ui.media.scroll_sample_after &&
        current_frame &&
        current_frame->generation == ui.media.scroll_sample_after->first &&
        current_frame->sequence > ui.media.scroll_sample_after->second) {
      if (recording.sample_scroll(*current_frame))
        ui.media.scroll_sample_after.reset();
    }
    if (current_frame) {
      const auto decision =
          frame_upload(primary_uploaded, current_frame->generation,
                       current_frame->sequence, config.color_override);
      if (decision != FrameUpload::skip &&
          renderer.upload(*current_frame, config.color_override)) {
        primary_uploaded = {true, current_frame->generation,
                            current_frame->sequence, config.color_override};
        if (decision == FrameUpload::new_frame) {
          session->video_presented(current_frame->generation,
                                   current_frame->sequence);
          diagnostics->record_sample_to_gpu_submit(
              std::chrono::steady_clock::now() - current_frame->arrival);
          diagnostics->record_present(current_frame->generation,
                                      current_frame->sequence);
        }
      }
    }
    const auto snapshot = session->snapshot();
    if (ui.media.scroll_active &&
        (!snapshot.video_fresh || snapshot.input_state == InputState::fault)) {
      recording.cancel_scroll();
      ui.media.scroll_active = false;
      ui.media.scroll_sample_after.reset();
    }
    if (current_frame && snapshot.video_fresh &&
        current_frame->generation == snapshot.capture.generation &&
        recorded_frame !=
            std::pair{current_frame->generation, current_frame->sequence}) {
      recorded_frame =
          std::pair{current_frame->generation, current_frame->sequence};
      if (recording.status().state == RecordingState::recording)
        (void)recording.append(*current_frame);
    }
    const std::string resolution =
        current_frame && current_frame->frame
            ? std::to_string(current_frame->frame->width) + "x" +
                  std::to_string(current_frame->frame->height)
            : "--";
    diagnostics->set_capture_mode(
        form.video_source == VideoSource::relay
            ? "Relay MJPEG (decoded video: " + resolution + ")"
        : snapshot.capture.actual_mode.width
            ? mode_text(snapshot.capture.actual_mode)
            : "--");
    const auto sample_rate = [](TrafficRate& rate,
                                const relay::TrafficSnapshot& traffic,
                                auto bytes_of) {
      const auto now = std::chrono::steady_clock::now();
      if (!rate.baseline) {
        rate.baseline = traffic;
        rate.sampled_at = now;
        return;
      }
      const double seconds =
          std::chrono::duration<double>(now - rate.sampled_at).count();
      if (seconds < 1.0) return;
      rate.bytes_per_second =
          static_cast<double>(bytes_of(traffic) - bytes_of(*rate.baseline)) /
          seconds;
      rate.baseline = traffic;
      rate.sampled_at = now;
    };
    if (video_client && snapshot.capture.state == CaptureState::streaming)
      sample_rate(video_rate, video_client->traffic_snapshot(),
                  [](const relay::TrafficSnapshot& traffic) {
                    return traffic.video_received_bytes;
                  });
    else
      video_rate.clear();
    if (control_client &&
        snapshot.control.state != ControlConnectionState::disconnected &&
        snapshot.control.state != ControlConnectionState::fault)
      sample_rate(control_rate, control_client->traffic_snapshot(),
                  [](const relay::TrafficSnapshot& traffic) {
                    return traffic.control_received_bytes +
                           traffic.control_sent_bytes;
                  });
    else
      control_rate.clear();
    diagnostics->set_mailbox_overwrites(snapshot.capture.overwritten_samples,
                                        snapshot.video.overwritten_frames);
    diagnostics->set_pixel_path(renderer.snapshot().pixel_path);
    if (!renderer.snapshot().error.empty())
      diagnostics->set_recent_error(renderer.snapshot().error);

    SessionView view;
    view.snapshot = snapshot;
    view.video = make_video_link(view.snapshot);
    view.control = make_control_link(view.snapshot);
    view.retiring = !retired_sessions.empty();
    view.captured = snapshot.input_state == InputState::captured ||
                    snapshot.input_state == InputState::recovering;
    view.remote_input =
        view.captured || snapshot.input_state == InputState::arming;
    view.relative_capture =
        view.captured && config.mouse_mode == MouseMode::relative;
    view.frame = current_frame ? &*current_frame : nullptr;
    view.valid_visible_cpu_frame =
        view.frame && view.frame->frame && !view.frame->frame->hw_frames_ctx &&
        view.frame->generation == snapshot.capture.generation &&
        snapshot.video_fresh && renderer.texture_id() != 0;
    view.resolution = resolution;
    view.relay_video_open = static_cast<bool>(video_client);
    view.relay_control_open = static_cast<bool>(control_client);
    view.renderer_width = renderer.width();
    view.renderer_height = renderer.height();
    view.pointer = snapshot.pointer;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ui.overlay.local_regions.clear();
    prepare_overlay_frame(ui, view);
    view.recording = recording.status();
    sync_media_feedback(ui, view.recording);
    sync_paste_feedback(ui, session->text_paste_progress());
    if (debug) log_debug_session(ui, view, session->text_paste_progress());
    view.paste = session->text_paste_progress();
    if (SDL_GetWindowRelativeMouseMode(window) != view.relative_capture)
      SDL_SetWindowRelativeMouseMode(window, view.relative_capture);
    if (window2 &&
        SDL_GetWindowRelativeMouseMode(window2) != view.relative_capture)
      SDL_SetWindowRelativeMouseMode(window2, view.relative_capture);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.F);
    apply(draw_chrome(ui, view));
    view.frame = nullptr;
    view.texture = renderer.texture_id();
    view.renderer_width = renderer.width();
    view.renderer_height = renderer.height();
    view.has_texture = view.texture != 0 && view.renderer_width > 0;
    SDL_GL_SetSwapInterval(config.vsync ? 1 : 0);
    apply(draw_video(ui, view));
    // Chrome may have replaced the diagnostics object. Read it after the
    // video rect, matching the previous status and diagnostics windows.
    view.diagnostics = diagnostics->snapshot();
    view.pointer = session->snapshot().pointer;
    view.video_bytes_per_second = video_rate.bytes_per_second;
    view.control_bytes_per_second = control_rate.bytes_per_second;
    if (video_client)
      view.relay_video = video_client->video_snapshot();
    else
      view.relay_video.reset();
    apply(draw_status(ui, view));
    ImGui::PopStyleVar(2);
    apply(draw_diagnostics(ui, view));
    draw_menu_icon(ui, view);
    ui.overlay.popup_open =
        !view.remote_input &&
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    ImGui::Render();
    int dw{}, dh{};
    SDL_GetWindowSizeInPixels(window, &dw, &dh);
    glViewport(0, 0, dw, dh);
    glClearColor(11.F / 255.F, 24.F / 255.F, 35.F / 255.F, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    const auto before = std::chrono::steady_clock::now();
    SDL_GL_SwapWindow(window);
    diagnostics->record_present_blocking(std::chrono::steady_clock::now() -
                                         before);
    if (!config.vsync) {
      constexpr auto frame_budget = std::chrono::milliseconds(1000 / 120);
      const auto elapsed = std::chrono::steady_clock::now() - frame_started;
      if (elapsed < frame_budget)
        SDL_Delay(static_cast<Uint32>(
            std::chrono::duration_cast<std::chrono::milliseconds>(frame_budget -
                                                                  elapsed)
                .count()));
    }
    if (window2) {
      if (SDL_GL_MakeCurrent(window2, context)) {
        second_display_failed = false;
        SDL_GL_SetSwapInterval(0);
        int w2{}, h2{};
        SDL_GetWindowSizeInPixels(window2, &w2, &h2);
        glViewport(0, 0, w2, h2);
        glClearColor(0.F, 0.F, 0.F, 1.F);
        glClear(GL_COLOR_BUFFER_BIT);
        if (second_valid) {
          const auto fit = fit_video_rect(
              {0, 0, static_cast<double>(w2), static_cast<double>(h2)},
              renderer2.width(), renderer2.height());
          renderer2.blit(
              static_cast<int>(std::lround(fit.x)),
              static_cast<int>(std::lround(h2 - fit.y)),
              static_cast<int>(std::lround(fit.x + fit.width)),
              static_cast<int>(std::lround(h2 - fit.y - fit.height)));
        }
        SDL_GL_SwapWindow(window2);
      } else {
        spdlog::error("Could not bind the screen 2 GL context: {}",
                      SDL_GetError());
        second_display_failed = true;
      }
      if (!SDL_GL_MakeCurrent(window, context)) {
        spdlog::error("Could not restore the main GL context: {}",
                      SDL_GetError());
        host.running = false;
      }
    }
  }
  close_second();
  recording.shutdown();
  session->shutdown();
  if (pref) {
    int w{}, h{};
    SDL_GetWindowSize(window, &w, &h);
    config.window.width = w;
    config.window.height = h;
    store_connection(config, ui.connection);
    try {
      save_config(*pref, config);
    } catch (...) {
    }
  }
  glDeleteTextures(1, &brand_texture);
  renderer.destroy();
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_GL_DestroyContext(context);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
