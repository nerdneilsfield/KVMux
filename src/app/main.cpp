#include <SDL3/SDL.h>
#include <glad/gl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/kvm_session.hpp"
#include "app/recording.hpp"
#include "network/relay_client.hpp"
#include "render/video_renderer.hpp"
#include "support/config.hpp"
#include "support/diagnostics.hpp"

namespace {
using namespace kvmux;

// Keep these base colors aligned with assets/branding/logo.svg.
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
  // Dark copper fills keep small warm-white labels readable in every state.
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

// Load once while the main-thread GL context is current. SDL supplies PNG
// decoding.
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

const char* decoder_backend_label(CodecBackend backend) {
  switch (backend) {
    case CodecBackend::automatic:
      return "Auto";
    case CodecBackend::videotoolbox:
      return "VideoToolbox";
    case CodecBackend::ffmpeg_software:
      return "FFmpeg software";
    case CodecBackend::jetson_gstreamer:
      return "Jetson GStreamer";
  }
  return "Unknown";
}
const char* capture_state(CaptureState state) {
  switch (state) {
    case CaptureState::stopped:
      return "Stopped";
    case CaptureState::starting:
      return "Starting";
    case CaptureState::streaming:
      return "Streaming";
    case CaptureState::stopping:
      return "Stopping";
    case CaptureState::permission_denied:
      return "Permission denied";
    case CaptureState::fault:
      return "Fault";
  }
  return "Unknown";
}
const char* control_state(ControlConnectionState state) {
  switch (state) {
    case ControlConnectionState::disconnected:
      return "Disconnected";
    case ControlConnectionState::opening:
      return "Opening";
    case ControlConnectionState::monitoring:
      return "USB not ready";
    case ControlConnectionState::clearing:
      return "Clearing input";
    case ControlConnectionState::ready:
      return "Ready";
    case ControlConnectionState::stalled:
      return "Stalled";
    case ControlConnectionState::reconnecting:
      return "Reconnecting";
    case ControlConnectionState::fault:
      return "Fault";
    case ControlConnectionState::stopping:
      return "Stopping";
  }
  return "Unknown";
}
const char* recording_state(RecordingState state) {
  switch (state) {
    case RecordingState::idle:
      return "Idle";
    case RecordingState::starting:
      return "Starting";
    case RecordingState::recording:
      return "Recording";
    case RecordingState::paused:
      return "Paused";
    case RecordingState::stopping:
      return "Stopping";
    case RecordingState::failed:
      return "Failed";
  }
  return "Unknown";
}
const char* input_state(InputState state) {
  switch (state) {
    case InputState::preview:
      return "Preview";
    case InputState::arming:
      return "Arming";
    case InputState::captured:
      return "Captured";
    case InputState::recovering:
      return "Recovering";
    case InputState::releasing:
      return "Releasing";
    case InputState::fault:
      return "Fault";
  }
  return "Unknown";
}
const char* text_paste_error(TextPasteError error) {
  switch (error) {
    case TextPasteError::none:
      return "Ready";
    case TextPasteError::empty:
      return "Enter text to type.";
    case TextPasteError::bare_carriage_return:
      return "Unsupported line ending.";
    case TextPasteError::non_ascii:
      return "Non-ASCII text is not supported.";
    case TextPasteError::unsupported:
      return "Unsupported characters found.";
    case TextPasteError::too_long:
      return "Text is limited to 1024 characters.";
  }
  return "Could not prepare text.";
}
const char* debug_text_paste_error(TextPasteError error) {
  switch (error) {
    case TextPasteError::none:
      return "idle";
    case TextPasteError::empty:
      return "error-empty";
    case TextPasteError::bare_carriage_return:
      return "error-line-ending";
    case TextPasteError::non_ascii:
      return "error-non-ascii";
    case TextPasteError::unsupported:
      return "error-unsupported";
    case TextPasteError::too_long:
      return "error-too-long";
  }
  return "error-unknown";
}
const char* debug_recording_state(RecordingState state) {
  switch (state) {
    case RecordingState::idle:
      return "idle";
    case RecordingState::starting:
      return "starting";
    case RecordingState::recording:
      return "recording";
    case RecordingState::paused:
      return "paused";
    case RecordingState::stopping:
      return "stopping";
    case RecordingState::failed:
      return "failed";
  }
  return "unknown";
}
std::string mode_text(const CaptureMode& mode) {
  return std::to_string(mode.width) + "x" + std::to_string(mode.height) +
         " @ " + std::to_string(mode.frame_rate.numerator) + "/" +
         std::to_string(mode.frame_rate.denominator) + " " +
         mode.device_format_name;
}
std::optional<std::filesystem::path> preference_file() {
  char* raw = SDL_GetPrefPath("kvmux", "kvmux");
  if (!raw) return std::nullopt;
  std::filesystem::path result(raw);
  SDL_free(raw);
  return result / "config.json";
}

std::optional<std::uint16_t> usb_usage_from_sdl(const SDL_Scancode scancode) {
  static constexpr std::array<std::uint16_t, 232> table = [] {
    std::array<std::uint16_t, 232> values{};
    for (std::uint16_t usage = 0x04; usage <= 0x73; ++usage)
      values[usage] = usage;
    for (std::uint16_t usage = 0x7f; usage <= 0x82; ++usage)
      values[usage] = usage;
    for (std::uint16_t usage = 0x85; usage <= 0x87; ++usage)
      values[usage] = usage;
    for (std::uint16_t usage = 0x89; usage <= 0x8f; ++usage)
      values[usage] = usage;
    for (std::uint16_t usage = 0xe0; usage <= 0xe7; ++usage)
      values[usage] = usage;
    return values;
  }();
  const auto index = static_cast<std::size_t>(scancode);
  if (index >= table.size() || table[index] == 0) return std::nullopt;
  return table[index];
}
// Own fixed-icon pointer gestures before ImGui and remote routing. Keyboard and
// lifecycle events must still reach the session while the icon owns a click.
struct FloatingMenuIcon {
  ImVec2 pos{16.F, 64.F};
  static constexpr float size = 32.F;
  Uint32 buttons{}, abandoned_buttons{};
  bool cancelled{}, clicked{};

  void clamp(float width, float height) {
    pos.x = std::clamp(pos.x, 0.F, std::max(0.F, width - size));
    pos.y = std::clamp(pos.y, 0.F, std::max(0.F, height - size));
  }
  bool contains(float x, float y) const {
    return x >= pos.x && x < pos.x + size && y >= pos.y && y < pos.y + size;
  }
  bool consume(const SDL_Event& event, bool enabled, float width,
               float height) {
    clamp(width, height);
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST ||
        event.type == SDL_EVENT_WINDOW_MINIMIZED ||
        event.type == SDL_EVENT_WINDOW_HIDDEN) {
      abandoned_buttons |= buttons;
      buttons = 0;
      cancelled = true;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
        (abandoned_buttons & SDL_BUTTON_MASK(event.button.button))) {
      abandoned_buttons &= ~SDL_BUTTON_MASK(event.button.button);
      return true;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
      abandoned_buttons &= ~SDL_BUTTON_MASK(event.button.button);
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        (buttons || (enabled && contains(event.button.x, event.button.y)))) {
      if (!buttons) {
        cancelled = event.button.button != SDL_BUTTON_LEFT;
      } else
        cancelled = true;
      buttons |= SDL_BUTTON_MASK(event.button.button);
      return true;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
        (buttons & SDL_BUTTON_MASK(event.button.button))) {
      buttons &= ~SDL_BUTTON_MASK(event.button.button);
      clicked =
          !buttons && !cancelled && contains(event.button.x, event.button.y);
      return true;
    }
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
      if (buttons) return true;
      return enabled && contains(event.motion.x, event.motion.y);
    }
    if (event.type == SDL_EVENT_MOUSE_WHEEL)
      return buttons ||
             (enabled && contains(event.wheel.mouse_x, event.wheel.mouse_y));
    return false;
  }
};

InputEvent to_input(const SDL_Event& event) {
  if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
    const auto usage = usb_usage_from_sdl(event.key.scancode);
    return {InputKey{usage.value_or(0), event.key.down, event.key.repeat}};
  }
  if (event.type == SDL_EVENT_MOUSE_MOTION)
    return {InputPointerMotion{event.motion.x, event.motion.y}};
  if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
      event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
    InputMouseButton b =
        event.button.button == SDL_BUTTON_RIGHT    ? InputMouseButton::right
        : event.button.button == SDL_BUTTON_MIDDLE ? InputMouseButton::middle
                                                   : InputMouseButton::left;
    return {InputButton{b, event.button.down, event.button.x, event.button.y}};
  }
  if (event.type == SDL_EVENT_MOUSE_WHEEL) {
    const double sign =
        event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0 : 1.0;
    return {InputWheel{event.wheel.y * sign, event.wheel.mouse_x,
                       event.wheel.mouse_y}};
  }
  return {InputPointerMotion{}};
}
}  // namespace

int main(int argc, char** argv) {
  bool debug = false, help = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    if (argument == "--debug")
      debug = true;
    else if (argument == "--help")
      help = true;
    else {
      std::cerr << "kvmux: Unknown argument: " << argument
                << "\nUsage: kvmux [--debug] [--help]\n";
      return 2;
    }
  }
  configure_console_logging(debug);
  if (help) {
    std::cout
        << "Usage: kvmux [--debug] [--help]\n"
           "  --debug  Write diagnostic logs to stderr (time, thread, level)\n"
           "  --help   Show this help without starting the GUI\n";
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

  auto session = std::make_unique<KvmSession>();
  Recording recording;
  (void)session->set_mouse_mode(config.mouse_mode);
  session->set_host_key(config.host_scancode);
  session->set_relative_gain(config.sensitivity);
  session->set_keep_alive(config.keep_alive);
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
    video_bytes_per_second.reset();
    control_bytes_per_second.reset();
    auto old = std::move(session);
    retired_sessions.push_back(
        std::async(std::launch::async, [old = std::move(old)]() mutable {
          old->shutdown();
          old.reset();
        }));
  };
  VideoRenderer renderer;
  Diagnostics diagnostics;
  std::vector<DeviceInfo> devices;
  std::vector<CaptureMode> modes;
  std::vector<SerialPortInfo> ports;
  std::future<std::vector<DeviceInfo>> devices_future =
      session->enumerate_capture_devices();
  std::optional<std::future<std::vector<CaptureMode>>> modes_future;
  int selected_device = -1, selected_mode = -1, selected_port = -1;
  bool fullscreen{}, diagnostics_open{}, running = true;
  bool show_status = true, chrome_visible = true, was_captured = false;
  double chrome_until = 0.0, captured_at = 0.0;
  bool close_connections_when_ready = false;
  bool popup_open = false;
  bool open_text_paste = false;
  std::array<char, 4097> text_paste_buffer{};
  std::size_t text_paste_removed = 0;
  TextPasteError paste_error = TextPasteError::none;
  std::size_t clipboard_loaded_bytes = 0, clipboard_loaded_characters = 0;
  std::vector<ImVec4> local_regions;
  Uint32 local_buttons = 0, remote_buttons = 0;
  FloatingMenuIcon menu_icon;
  bool open_floating_menu = false, open_connections = false;
  auto next_serial_scan = std::chrono::steady_clock::now();
  std::optional<VideoFrame> current_frame;
  std::optional<std::pair<std::uint64_t, std::uint64_t>> recorded_frame;
  std::string media_message;
  std::filesystem::path displayed_snapshot_path, displayed_scroll_path;
  std::string displayed_snapshot_error, displayed_scroll_error;
  bool snapshot_queued = false;
  std::optional<FrameCrop> saved_region;
  bool region_selecting = false, region_dragging = false, region_ready = false;
  bool region_confirm_requested = false;
  bool scroll_active = false;
  std::optional<std::pair<std::uint64_t, std::uint64_t>> scroll_sample_after;
  ImVec2 region_start{}, region_end{};
  Rect displayed_video_rect{};
  bool paste_was_active = false, paste_cancelled = false;
  bool local_ui_input_seen = false;
  std::string last_status;
  const auto confirm_region = [&] {
    const double left = std::min(region_start.x, region_end.x);
    const double top = std::min(region_start.y, region_end.y);
    const double right = std::max(region_start.x, region_end.x);
    const double bottom = std::max(region_start.y, region_end.y);
    const unsigned w = renderer.width(), h = renderer.height();
    const unsigned x = static_cast<unsigned>(
        std::clamp(std::floor((left - displayed_video_rect.x) * w /
                              displayed_video_rect.width),
                   0.0, static_cast<double>(w)));
    const unsigned y = static_cast<unsigned>(
        std::clamp(std::floor((top - displayed_video_rect.y) * h /
                              displayed_video_rect.height),
                   0.0, static_cast<double>(h)));
    const unsigned r = static_cast<unsigned>(
        std::clamp(std::ceil((right - displayed_video_rect.x) * w /
                             displayed_video_rect.width),
                   0.0, static_cast<double>(w)));
    const unsigned b = static_cast<unsigned>(
        std::clamp(std::ceil((bottom - displayed_video_rect.y) * h /
                             displayed_video_rect.height),
                   0.0, static_cast<double>(h)));
    if (r > x && b > y) {
      saved_region = FrameCrop{x, y, r - x, b - y};
      media_message = "Region saved.";
      region_selecting = region_ready = false;
    } else {
      media_message = "Select a larger region.";
    }
    region_confirm_requested = false;
  };

  while (running) {
    const auto frame_started = std::chrono::steady_clock::now();
    std::erase_if(retired_sessions, [](auto& future) {
      return future.wait_for(std::chrono::seconds(0)) ==
             std::future_status::ready;
    });
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      const auto event_state = session->snapshot().input_state;
      const bool remote_input = event_state == InputState::captured ||
                                event_state == InputState::recovering ||
                                event_state == InputState::arming;
      int event_width{}, event_height{};
      SDL_GetWindowSize(window, &event_width, &event_height);
      const bool icon_enabled =
          !popup_open && !remote_buttons &&
          !(remote_input && config.mouse_mode == MouseMode::relative);
      const bool preview = event_state == InputState::preview;
      if (scroll_active && (event.type == SDL_EVENT_WINDOW_FOCUS_LOST ||
                            event.type == SDL_EVENT_WINDOW_MINIMIZED ||
                            event.type == SDL_EVENT_WINDOW_HIDDEN ||
                            (event.type == SDL_EVENT_KEY_DOWN &&
                             event.key.scancode == SDL_SCANCODE_ESCAPE))) {
        recording.cancel_scroll();
        scroll_active = false;
        scroll_sample_after.reset();
      }
      if (region_selecting) {
        if (!preview || event.type == SDL_EVENT_WINDOW_FOCUS_LOST ||
            event.type == SDL_EVENT_WINDOW_MINIMIZED ||
            event.type == SDL_EVENT_WINDOW_HIDDEN ||
            (event.type == SDL_EVENT_KEY_DOWN &&
             event.key.scancode == SDL_SCANCODE_ESCAPE)) {
          region_selecting = region_dragging = region_ready = false;
          region_confirm_requested = false;
          if (event.type == SDL_EVENT_KEY_DOWN) continue;
        }
        if (preview && region_ready &&
            event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
            event.button.button == SDL_BUTTON_LEFT &&
            event.button.clicks >= 2 &&
            event.button.x >= std::min(region_start.x, region_end.x) &&
            event.button.x <= std::max(region_start.x, region_end.x) &&
            event.button.y >= std::min(region_start.y, region_end.y) &&
            event.button.y <= std::max(region_start.y, region_end.y)) {
          confirm_region();
          continue;
        }
        if (preview && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
            event.button.button == SDL_BUTTON_LEFT &&
            displayed_video_rect.contains(event.button.x, event.button.y)) {
          region_start = region_end = {event.button.x, event.button.y};
          region_dragging = true;
          region_ready = false;
          continue;
        }
        if (region_dragging && event.type == SDL_EVENT_MOUSE_MOTION) {
          region_end = {
              static_cast<float>(std::clamp(
                  static_cast<double>(event.motion.x), displayed_video_rect.x,
                  displayed_video_rect.x + displayed_video_rect.width)),
              static_cast<float>(std::clamp(
                  static_cast<double>(event.motion.y), displayed_video_rect.y,
                  displayed_video_rect.y + displayed_video_rect.height))};
          continue;
        }
        if (region_dragging && event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
            event.button.button == SDL_BUTTON_LEFT) {
          region_dragging = false;
          region_ready = true;
          continue;
        }
        if (preview && (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                        event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
                        event.type == SDL_EVENT_MOUSE_MOTION ||
                        event.type == SDL_EVENT_MOUSE_WHEEL))
          continue;
      }
      if (menu_icon.consume(event, icon_enabled,
                            static_cast<float>(event_width),
                            static_cast<float>(event_height))) {
        if (menu_icon.clicked) {
          session->release_control();
          open_floating_menu = true;
          menu_icon.clicked = false;
        }
        continue;
      }
      if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST ||
          event.type == SDL_EVENT_WINDOW_MINIMIZED ||
          event.type == SDL_EVENT_WINDOW_HIDDEN) {
        remote_buttons = 0;
        open_floating_menu = false;
      }
      // Local overlays must not activate capture through the video underneath.
      // Once armed, never let ImGui consume remote edges (including Host
      // release).
      bool local_click = false;
      if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
          event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        const Uint32 bit = SDL_BUTTON_MASK(event.button.button);
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
          local_click =
              (event_state == InputState::preview &&
               (popup_open || open_floating_menu ||
                std::any_of(local_regions.begin(), local_regions.end(),
                            [&](const ImVec4& r) {
                              return event.button.x >= r.x &&
                                     event.button.x < r.z &&
                                     event.button.y >= r.y &&
                                     event.button.y < r.w;
                            })));
          if (local_click) local_buttons |= bit;
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
                   (local_buttons & bit)) {
          local_buttons &= ~bit;
          local_click = true;
        }
      }
      if (!remote_input || local_click) {
        ImGui_ImplSDL3_ProcessEvent(&event);
        if (local_click) local_ui_input_seen = true;
      }
      if (event.type == SDL_EVENT_QUIT) {
        session->release_control();
        running = false;
        continue;
      }
      if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) session->focus_lost();
      if (event.type == SDL_EVENT_WINDOW_MINIMIZED ||
          event.type == SDL_EVENT_WINDOW_HIDDEN)
        session->minimized();
      const bool injectable_event = event.type == SDL_EVENT_KEY_DOWN ||
                                    event.type == SDL_EVENT_KEY_UP ||
                                    event.type == SDL_EVENT_MOUSE_MOTION ||
                                    event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                                    event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
                                    event.type == SDL_EVENT_MOUSE_WHEEL;
      // A local popup owns injectable input, but SDL and ImGui still receive
      // close and window-management events.
      if (local_click && injectable_event) continue;
      const bool paste_cancel_event =
          session->snapshot().text_paste_active ||
          (event.type == SDL_EVENT_KEY_DOWN ||
           event.type == SDL_EVENT_KEY_UP) &&
              event.key.scancode == config.host_scancode;
      if (popup_open && !remote_input && injectable_event &&
          !paste_cancel_event)
        continue;
      if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && remote_input)
        remote_buttons |= SDL_BUTTON_MASK(event.button.button);
      if (event.type == SDL_EVENT_MOUSE_BUTTON_UP)
        remote_buttons &= ~SDL_BUTTON_MASK(event.button.button);
      if (scroll_active && remote_input &&
          event.type == SDL_EVENT_MOUSE_WHEEL) {
        const double sign =
            event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0 : 1.0;
        if (event.wheel.y * sign < 0.0 && current_frame)
          scroll_sample_after =
              std::pair{current_frame->generation, current_frame->sequence};
      }
      if (event.type == SDL_EVENT_MOUSE_MOTION &&
          config.mouse_mode == MouseMode::relative)
        session->handle_input(
            {InputRelativeMotion{event.motion.xrel, event.motion.yrel}});
      else if (injectable_event)
        session->handle_input(to_input(event));
    }
    if (devices_future.valid() && devices_future.wait_for(std::chrono::seconds(
                                      0)) == std::future_status::ready) {
      devices = devices_future.get();
      if (selected_device < 0 ||
          selected_device >= static_cast<int>(devices.size()))
        selected_device = devices.empty() ? -1 : 0;
    }
    if (modes_future && modes_future->wait_for(std::chrono::seconds(0)) ==
                            std::future_status::ready) {
      modes = modes_future->get();
      selected_mode = modes.empty() ? -1 : 0;
      modes_future.reset();
    }
    if (!remote && open_connections &&
        session->snapshot().input_state == InputState::preview &&
        std::chrono::steady_clock::now() >= next_serial_scan) {
      ports = enumerate_serial_ports();
      next_serial_scan =
          std::chrono::steady_clock::now() + std::chrono::seconds(1);
    }
    session->tick();
    if (auto newest = session->take_latest_frame()) {
      current_frame = std::move(newest);
      diagnostics.record_decode(current_frame->decoded);
    }
    if (current_frame &&
        current_frame->generation != session->snapshot().capture.generation)
      current_frame.reset();
    if (scroll_active && scroll_sample_after && current_frame &&
        current_frame->generation == scroll_sample_after->first &&
        current_frame->sequence > scroll_sample_after->second) {
      if (recording.sample_scroll(*current_frame)) scroll_sample_after.reset();
    }
    if (current_frame &&
        renderer.upload(*current_frame, config.color_override)) {
      session->video_presented(current_frame->generation,
                               current_frame->sequence);
      diagnostics.record_sample_to_gpu_submit(std::chrono::steady_clock::now() -
                                              current_frame->arrival);
      diagnostics.record_present(current_frame->generation,
                                 current_frame->sequence);
    }
    const auto snapshot = session->snapshot();
    if (scroll_active &&
        (!snapshot.video_fresh || snapshot.input_state == InputState::fault)) {
      recording.cancel_scroll();
      scroll_active = false;
      scroll_sample_after.reset();
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
    diagnostics.set_capture_mode(
        remote ? "Relay MJPEG (decoded video: " + resolution + ")"
        : snapshot.capture.actual_mode.width
            ? mode_text(snapshot.capture.actual_mode)
            : "--");
    const bool relay_connected =
        remote_client && snapshot.capture.state == CaptureState::streaming &&
        snapshot.control.state != ControlConnectionState::disconnected &&
        snapshot.control.state != ControlConnectionState::fault;
    if (relay_connected) {
      const auto now = std::chrono::steady_clock::now();
      const auto traffic = remote_client->traffic_snapshot();
      if (!traffic_baseline) {
        traffic_baseline = traffic;
        traffic_sampled_at = now;
      } else if (const double seconds =
                     std::chrono::duration<double>(now - traffic_sampled_at)
                         .count();
                 seconds >= 1.0) {
        video_bytes_per_second =
            static_cast<double>(traffic.video_received_bytes -
                                traffic_baseline->video_received_bytes) /
            seconds;
        control_bytes_per_second =
            (static_cast<double>(traffic.control_received_bytes -
                                 traffic_baseline->control_received_bytes) +
             static_cast<double>(traffic.control_sent_bytes -
                                 traffic_baseline->control_sent_bytes)) /
            seconds;
        traffic_baseline = traffic;
        traffic_sampled_at = now;
      }
    } else {
      traffic_baseline.reset();
      video_bytes_per_second.reset();
      control_bytes_per_second.reset();
    }
    diagnostics.set_mailbox_overwrites(snapshot.capture.overwritten_samples,
                                       snapshot.video.overwritten_frames);
    diagnostics.record_ack_rtt(snapshot.control.last_ack_rtt);
    diagnostics.set_pixel_path(renderer.snapshot().pixel_path);
    if (!renderer.snapshot().error.empty())
      diagnostics.set_recent_error(renderer.snapshot().error);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    local_regions.clear();
    const auto record_local_region = [&] {
      const auto pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
      local_regions.push_back({pos.x, pos.y, pos.x + size.x, pos.y + size.y});
    };
    const bool captured = snapshot.input_state == InputState::captured ||
                          snapshot.input_state == InputState::recovering;
    const bool remote_input =
        captured || snapshot.input_state == InputState::arming;
    const double now = ImGui::GetTime();
    if (captured && !was_captured) captured_at = now;
    if (remote_input && !local_ui_input_seen) {
      ImGui::GetIO().ClearInputKeys();
      ImGui::GetIO().ClearEventsQueue();
      chrome_until = 0.0;
    }
    local_ui_input_seen = false;
    was_captured = captured;
    const bool relative_capture =
        captured && config.mouse_mode == MouseMode::relative;
    const auto media_status = recording.status();
    if (media_status.snapshot_error != displayed_snapshot_error) {
      displayed_snapshot_error = media_status.snapshot_error;
      if (!displayed_snapshot_error.empty()) {
        snapshot_queued = false;
        media_message = "Screenshot error: " + displayed_snapshot_error;
      }
    }
    if (media_status.last_snapshot_path != displayed_snapshot_path) {
      displayed_snapshot_path = media_status.last_snapshot_path;
      if (!displayed_snapshot_path.empty()) {
        snapshot_queued = false;
      }
    }
    if (media_status.scroll_error != displayed_scroll_error) {
      displayed_scroll_error = media_status.scroll_error;
      if (!displayed_scroll_error.empty()) {
        media_message = "Scrolling screenshot error: " + displayed_scroll_error;
        scroll_active = false;
        scroll_sample_after.reset();
        recording.cancel_scroll();
      }
    }
    if (media_status.last_scroll_path != displayed_scroll_path) {
      displayed_scroll_path = media_status.last_scroll_path;
      if (!displayed_scroll_path.empty()) {
        scroll_active = false;
        scroll_sample_after.reset();
      }
    }
    if (debug) {
      const auto paste = session->text_paste_progress();
      const char* paste_status = debug_text_paste_error(paste.error);
      if (snapshot.text_paste_active) {
        paste_status = "active";
        paste_was_active = true;
      } else if (paste_was_active) {
        paste_status = paste_cancelled ? "cancelled" : "scheduled";
        paste_was_active = false;
        paste_cancelled = false;
      }
      const char* const snapshot_status =
          !media_status.snapshot_error.empty()       ? "failed"
          : snapshot_queued                          ? "queued"
          : !media_status.last_snapshot_path.empty() ? "saved"
                                                     : "idle";
      const auto status =
          std::string("capture=") + capture_state(snapshot.capture.state) +
          " capture-error=" +
          (snapshot.capture.error.empty() ? "none" : "present") +
          " control=" + control_state(snapshot.control.state) +
          " control-error=" +
          (snapshot.control.error.empty() ? "none" : "present") +
          " input=" + input_state(snapshot.input_state) +
          " paste=" + paste_status +
          " paste-bytes=" + std::to_string(paste.source_bytes) +
          " paste-chars=" + std::to_string(paste.normalized_characters) +
          " paste-scheduled=" + std::to_string(paste.scheduled_gestures) + "/" +
          std::to_string(paste.planned_gestures) + " media=snapshot-" +
          snapshot_status +
          " recording=" + debug_recording_state(media_status.state) +
          " session-error=" +
          (snapshot.session_error.empty() ? "none" : "present");
      if (status != last_status) {
        spdlog::debug("Session {}", status);
        last_status = status;
      }
    }
    const auto text_paste_actions = [&] {
      const auto paste = session->text_paste_progress();
      if (snapshot.text_paste_active) {
        ImGui::Text("Typing ASCII: %zu/%zu scheduled", paste.scheduled_gestures,
                    paste.planned_gestures);
        if (ImGui::Button("Cancel typing")) {
          paste_cancelled = true;
          session->cancel_text_paste();
          text_paste_buffer.fill('\0');
          text_paste_removed = 0;
        }
        return;
      }
      ImGui::TextUnformatted(
          "US ASCII only. Target keyboard layout must be US.");
      ImGui::InputTextMultiline("##ascii-text", text_paste_buffer.data(),
                                text_paste_buffer.size(),
                                {-1.F, ImGui::GetTextLineHeight() * 6.F});
      if (text_paste_removed)
        ImGui::Text("Removed unsupported characters: %zu", text_paste_removed);
      if (paste_error != TextPasteError::none) {
        ImGui::TextWrapped("%s", text_paste_error(paste_error));
        if (paste_error == TextPasteError::bare_carriage_return ||
            paste_error == TextPasteError::non_ascii ||
            paste_error == TextPasteError::unsupported) {
          if (ImGui::Button("Remove unsupported")) {
            const auto filtered =
                filter_us_ascii_text(text_paste_buffer.data());
            text_paste_removed = filtered.removed;
            std::snprintf(text_paste_buffer.data(), text_paste_buffer.size(),
                          "%s", filtered.text.c_str());
            paste_error = TextPasteError::none;
          }
          ImGui::SameLine();
        }
      }
      ImGui::BeginDisabled(snapshot.input_state != InputState::preview);
      const auto prepared = map_us_ascii_text(text_paste_buffer.data());
      ImGui::Text("Prepared: %zu characters", prepared.normalized_characters);
      if (ImGui::Button("Type ASCII")) {
        const auto result = session->start_text_paste(text_paste_buffer.data());
        paste_error = result.error;
        if (result) {
          text_paste_buffer.fill('\0');
          text_paste_removed = 0;
        }
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        text_paste_buffer.fill('\0');
        text_paste_removed = 0;
        paste_error = TextPasteError::none;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Type host clipboard")) {
        char* clipboard = SDL_GetClipboardText();
        if (!clipboard) {
          paste_error = TextPasteError::unsupported;
        } else {
          clipboard_loaded_bytes = std::strlen(clipboard);
          const auto prepared = map_us_ascii_text(clipboard);
          clipboard_loaded_characters = prepared.normalized_characters;
          if (debug)
            spdlog::debug(
                "Clipboard text loaded: bytes={} characters={} mapping={}",
                clipboard_loaded_bytes, clipboard_loaded_characters,
                debug_text_paste_error(prepared.error));
          std::snprintf(text_paste_buffer.data(), text_paste_buffer.size(),
                        "%s", clipboard);
          SDL_free(clipboard);
          text_paste_removed = 0;
          paste_error = prepared.error;
        }
      }
      if (clipboard_loaded_bytes) {
        ImGui::Text("Clipboard loaded: %zu bytes, %zu characters",
                    clipboard_loaded_bytes, clipboard_loaded_characters);
      }
    };
    const auto media_actions = [&] {
      const auto& status = media_status;
      const bool valid_visible_cpu_frame =
          current_frame && current_frame->frame &&
          !current_frame->frame->hw_frames_ctx &&
          current_frame->generation == snapshot.capture.generation &&
          snapshot.video_fresh && renderer.texture_id() != 0;
      if (region_selecting) {
        if (region_confirm_requested ||
            ImGui::MenuItem("Confirm region", nullptr, false, region_ready)) {
          const double left = std::min(region_start.x, region_end.x),
                       top = std::min(region_start.y, region_end.y);
          const double right = std::max(region_start.x, region_end.x),
                       bottom = std::max(region_start.y, region_end.y);
          const unsigned w = renderer.width(), h = renderer.height();
          const unsigned x = static_cast<unsigned>(
              std::clamp(std::floor((left - displayed_video_rect.x) * w /
                                    displayed_video_rect.width),
                         0.0, static_cast<double>(w)));
          const unsigned y = static_cast<unsigned>(
              std::clamp(std::floor((top - displayed_video_rect.y) * h /
                                    displayed_video_rect.height),
                         0.0, static_cast<double>(h)));
          const unsigned r = static_cast<unsigned>(
              std::clamp(std::ceil((right - displayed_video_rect.x) * w /
                                   displayed_video_rect.width),
                         0.0, static_cast<double>(w)));
          const unsigned b = static_cast<unsigned>(
              std::clamp(std::ceil((bottom - displayed_video_rect.y) * h /
                                   displayed_video_rect.height),
                         0.0, static_cast<double>(h)));
          if (r > x && b > y) {
            saved_region = FrameCrop{x, y, r - x, b - y};
            media_message = "Region saved.";
            region_selecting = region_ready = false;
            region_confirm_requested = false;
          } else
            media_message = "Select a larger region.";
        }
        if (ImGui::MenuItem("Cancel region selection")) {
          region_selecting = region_dragging = region_ready = false;
          region_confirm_requested = false;
        }
        ImGui::Separator();
      }
      ImGui::BeginDisabled(!valid_visible_cpu_frame || !saved_region);
      if (ImGui::MenuItem("Save screenshot")) {
        if (recording.snapshot(*current_frame, *saved_region)) {
          snapshot_queued = true;
          media_message = "Screenshot queued.";
          ImGui::CloseCurrentPopup();
        } else
          media_message = "Could not queue screenshot.";
      }
      if (ImGui::MenuItem("Start scrolling")) {
        if (recording.start_scroll(*current_frame, *saved_region)) {
          scroll_active = true;
          session->activate_control();
          media_message = "Scrolling capture started.";
          ImGui::CloseCurrentPopup();
        } else
          media_message = "Could not start scrolling capture.";
      }
      ImGui::EndDisabled();
      ImGui::BeginDisabled(!valid_visible_cpu_frame || !saved_region ||
                           status.state != RecordingState::idle);
      if (ImGui::MenuItem("Start recording")) {
        media_message = recording.start(*current_frame, *saved_region)
                            ? "Recording started."
                            : "Could not start recording.";
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndDisabled();
      if (ImGui::MenuItem("Select region")) {
        region_selecting = true;
        region_dragging = region_ready = false;
        ImGui::CloseCurrentPopup();
      }
      if (status.state == RecordingState::recording) {
        if (ImGui::MenuItem("Pause recording")) (void)recording.pause();
      } else if (status.state == RecordingState::paused) {
        if (ImGui::MenuItem("Resume recording")) (void)recording.resume();
      }
      ImGui::BeginDisabled(status.state != RecordingState::recording &&
                           status.state != RecordingState::paused &&
                           status.state != RecordingState::failed);
      if (ImGui::MenuItem(status.state == RecordingState::failed
                              ? "Reset failed recording"
                              : "Stop recording"))
        (void)recording.stop();
      ImGui::EndDisabled();
      if (scroll_active && ImGui::MenuItem("Finish scrolling screenshot")) {
        (void)recording.finish_scroll();
        scroll_active = false;
      }
    };
    if (SDL_GetWindowRelativeMouseMode(window) != relative_capture)
      SDL_SetWindowRelativeMouseMode(window, relative_capture);
    const auto mouse = ImGui::GetIO().MousePos;
    const bool at_top = mouse.x >= viewport->Pos.x &&
                        mouse.x < viewport->Pos.x + viewport->Size.x &&
                        mouse.y >= viewport->Pos.y &&
                        mouse.y < viewport->Pos.y + 8.F;
    if (!remote_input && at_top) chrome_until = now + 1.0;
    chrome_visible = !remote_input &&
                     (!fullscreen || open_floating_menu || open_connections ||
                      now < chrome_until ||
                      ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId));
    const auto overlay_flags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.F);
    if (chrome_visible) {
      ImGui::SetNextWindowPos(viewport->Pos);
      ImGui::SetNextWindowSize({viewport->Size.x, ImGui::GetFrameHeight()});
      ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, {0.F, 0.F});
      ImGui::Begin("Chrome", nullptr,
                   overlay_flags | ImGuiWindowFlags_MenuBar |
                       ImGuiWindowFlags_NoScrollbar |
                       ImGuiWindowFlags_NoScrollWithMouse);
      ImGui::PopStyleVar();
      record_local_region();
      if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
        chrome_until = now + 1.0;
      if (ImGui::BeginMenuBar()) {
        if (open_floating_menu) {
          ImGui::OpenPopup("Floating menu");
          open_floating_menu = false;
        }
        if (ImGui::MenuItem("Connections")) open_connections = true;
        if (ImGui::BeginMenu("Media")) {
          media_actions();
          ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Type ASCII")) open_text_paste = true;
        ImGui::SetNextWindowPos(
            {viewport->Pos.x + menu_icon.pos.x,
             viewport->Pos.y + menu_icon.pos.y + FloatingMenuIcon::size},
            ImGuiCond_Appearing);
        if (ImGui::BeginPopup("Floating menu")) {
          record_local_region();
          if (ImGui::MenuItem("Connections / settings"))
            open_connections = true;
          ImGui::Separator();
          media_actions();
          ImGui::Separator();
          if (ImGui::MenuItem("Type ASCII")) open_text_paste = true;
          ImGui::MenuItem("Status overlay", nullptr, &show_status);
          if (ImGui::MenuItem("Keep target awake", nullptr, &config.keep_alive))
            session->set_keep_alive(config.keep_alive);
          if (ImGui::MenuItem(fullscreen ? "Exit fullscreen" : "Fullscreen")) {
            fullscreen = !fullscreen;
            SDL_SetWindowFullscreen(window, fullscreen);
          }
          if (ImGui::MenuItem("Diagnostics"))
            diagnostics_open = !diagnostics_open;
          ImGui::EndPopup();
        }
        if (ImGui::MenuItem(fullscreen ? "Exit fullscreen" : "Fullscreen")) {
          fullscreen = !fullscreen;
          SDL_SetWindowFullscreen(window, fullscreen);
        }
        if (ImGui::MenuItem("Diagnostics"))
          diagnostics_open = !diagnostics_open;
        ImGui::MenuItem("Status overlay", nullptr, &show_status);
        if (open_text_paste) {
          ImGui::OpenPopup("Type ASCII");
          open_text_paste = false;
        }
        ImGui::SetNextWindowPos(
            {viewport->Pos.x + 8.F, viewport->Pos.y + ImGui::GetFrameHeight()});
        ImGui::SetNextWindowSize(
            {std::min(600.F, viewport->Size.x - 16.F), 0.F});
        if (ImGui::BeginPopup("Type ASCII")) {
          record_local_region();
          text_paste_actions();
          ImGui::EndPopup();
        }
        ImGui::SetNextWindowPos(
            {viewport->Pos.x + 8.F, viewport->Pos.y + ImGui::GetFrameHeight()});
        ImGui::SetNextWindowSize(
            {std::min(900.F, viewport->Size.x - 16.F), 0.F});
        if (open_connections &&
            ImGui::Begin("Connections", &open_connections,
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_AlwaysAutoResize)) {
          record_local_region();
          if (close_connections_when_ready && snapshot.video_fresh &&
              snapshot.control.state == ControlConnectionState::ready) {
            open_connections = false;
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
            (void)recording.stop();
            if (scroll_active) {
              recording.cancel_scroll();
              scroll_active = false;
              scroll_sample_after.reset();
            }
            retire_session();
            remote = requested_remote;
            session = std::make_unique<KvmSession>();
            devices.clear();
            modes.clear();
            selected_device = selected_mode = -1;
            modes_future.reset();
            devices_future = session->enumerate_capture_devices();
            current_frame.reset();
            renderer.destroy();
            (void)session->set_mouse_mode(config.mouse_mode);
            session->set_host_key(config.host_scancode);
            session->set_keep_alive(config.keep_alive);
          }
          if (remote) {
            ImGui::InputText("IPv4 host", remote_host, sizeof(remote_host));
            ImGui::InputInt("Control port", &control_port);
            ImGui::InputInt("Video port", &video_port);
            if (ImGui::BeginCombo(
                    "Decode", decoder_backend_label(config.decoder_backend))) {
              for (const auto backend :
                   {CodecBackend::automatic, CodecBackend::videotoolbox,
                    CodecBackend::ffmpeg_software}) {
                if (ImGui::Selectable(decoder_backend_label(backend),
                                      config.decoder_backend == backend))
                  config.decoder_backend = backend;
              }
              ImGui::EndCombo();
            }
            if (ImGui::Button("Connect relay") && retired_sessions.empty() &&
                control_port > 0 && control_port <= 65535 && video_port > 0 &&
                video_port <= 65535) {
              relay::ClientOptions client_options{
                  remote_host, static_cast<std::uint16_t>(control_port),
                  static_cast<std::uint16_t>(video_port)};
              client_options.decoder_backend = config.decoder_backend;
              auto client = std::make_shared<relay::RelayClient>(
                  std::move(client_options));
              auto capture =
                  std::make_unique<relay::NetworkCaptureSource>(client);
              const auto remote_devices = capture->enumerate_devices();
              if (remote_devices.empty()) {
                media_message = "Relay returned no capture devices.";
              } else {
                const auto remote_device = remote_devices.front();
                const auto remote_modes =
                    capture->enumerate_modes(remote_device.stable_id);
                if (remote_modes.empty()) {
                  media_message = "Relay returned no capture modes.";
                } else {
                  const auto remote_mode = remote_modes.front();
                  close_connections_when_ready = true;
                  retire_session();
                  devices_future = {};
                  modes_future.reset();
                  devices.clear();
                  modes.clear();
                  selected_device = selected_mode = -1;
                  remote_client = client;
                  session = std::make_unique<KvmSession>(
                      std::move(capture),
                      std::make_unique<relay::NetworkControlSink>(client));
                  session->set_host_key(config.host_scancode);
                  (void)session->set_mouse_mode(config.mouse_mode);
                  session->set_keep_alive(config.keep_alive);
                  (void)session->select_capture(remote_device, remote_mode);
                  current_frame.reset();
                  renderer.destroy();
                }
              }
            }
            ImGui::SameLine();
            if (ImGui::Button("Disconnect relay")) {
              session->release_control();
              (void)session->stop_capture();
              (void)session->disconnect_control();
              remote_client.reset();
              traffic_baseline.reset();
              video_bytes_per_second.reset();
              control_bytes_per_second.reset();
              current_frame.reset();
              renderer.destroy();
            }
          }
          ImGui::EndDisabled();
          if (!remote) {
            if (ImGui::BeginTable("connections", 4,
                                  ImGuiTableFlags_SizingStretchProp)) {
              ImGui::TableSetupColumn("setting",
                                      ImGuiTableColumnFlags_WidthFixed, 120.F);
              ImGui::TableSetupColumn("choice",
                                      ImGuiTableColumnFlags_WidthStretch, 2.F);
              ImGui::TableSetupColumn("setting2",
                                      ImGuiTableColumnFlags_WidthFixed, 100.F);
              ImGui::TableSetupColumn("choice2",
                                      ImGuiTableColumnFlags_WidthStretch, 1.F);
              ImGui::TableNextRow();
              ImGui::TableSetColumnIndex(0);
              ImGui::TextUnformatted("Capture device");
              ImGui::TableSetColumnIndex(1);
              if (ImGui::BeginCombo(
                      "##capture_device",
                      selected_device >= 0 &&
                              selected_device < static_cast<int>(devices.size())
                          ? devices[selected_device].display_name.c_str()
                          : "Select device")) {
                for (int i = 0; i < static_cast<int>(devices.size()); ++i) {
                  if (ImGui::Selectable(devices[i].display_name.c_str(),
                                        i == selected_device)) {
                    selected_device = i;
                    modes.clear();
                    selected_mode = -1;
                    modes_future =
                        session->enumerate_capture_modes(devices[i].stable_id);
                  }
                }
                ImGui::EndCombo();
              }
              ImGui::TableSetColumnIndex(2);
              ImGui::TextUnformatted("Serial port");
              ImGui::TableSetColumnIndex(3);
              if (ImGui::BeginCombo(
                      "##serial_port",
                      selected_port >= 0 &&
                              selected_port < static_cast<int>(ports.size())
                          ? ports[selected_port].name.c_str()
                          : "Select port")) {
                for (int i = 0; i < static_cast<int>(ports.size()); ++i)
                  if (ImGui::Selectable(ports[i].name.c_str(),
                                        i == selected_port))
                    selected_port = i;
                ImGui::EndCombo();
              }
              ImGui::TableNextRow();
              ImGui::TableSetColumnIndex(0);
              ImGui::TextUnformatted("Capture mode");
              ImGui::TableSetColumnIndex(1);
              if (ImGui::BeginCombo(
                      "##capture_mode",
                      selected_mode >= 0 &&
                              selected_mode < static_cast<int>(modes.size())
                          ? mode_text(modes[selected_mode]).c_str()
                          : "Select mode")) {
                for (int i = 0; i < static_cast<int>(modes.size()); ++i)
                  if (ImGui::Selectable(mode_text(modes[i]).c_str(),
                                        i == selected_mode))
                    selected_mode = i;
                ImGui::EndCombo();
              }
              ImGui::TableSetColumnIndex(2);
              ImGui::TextUnformatted("Baud rate");
              ImGui::TableSetColumnIndex(3);
              ImGui::InputInt("##baud", &config.serial_baud_rate);
              config.serial_baud_rate = std::max(1200, config.serial_baud_rate);
              ImGui::EndTable();
            }
            if (ImGui::Button("Start preview") && selected_device >= 0 &&
                selected_mode >= 0) {
              close_connections_when_ready = true;
              (void)session->select_capture(devices[selected_device],
                                            modes[selected_mode]);
              current_frame.reset();
              renderer.destroy();
            }
            ImGui::SameLine();
            if (ImGui::Button("Connect") && selected_port >= 0) {
              close_connections_when_ready = true;
              (void)session->connect_control(ports[selected_port].name,
                                             config.serial_baud_rate,
                                             config.serial_address);
            }
            ImGui::SameLine();
            if (ImGui::Button("Disconnect"))
              (void)session->disconnect_control();
          }
          if (ImGui::RadioButton("Absolute",
                                 config.mouse_mode == MouseMode::absolute)) {
            config.mouse_mode = MouseMode::absolute;
            (void)session->set_mouse_mode(config.mouse_mode);
          }
          ImGui::SameLine();
          if (ImGui::RadioButton("Relative",
                                 config.mouse_mode == MouseMode::relative)) {
            config.mouse_mode = MouseMode::relative;
            (void)session->set_mouse_mode(config.mouse_mode);
          }
          ImGui::SameLine();
          ImGui::SetNextItemWidth(180.F);
          float sensitivity = static_cast<float>(config.sensitivity);
          if (ImGui::SliderFloat("Sensitivity", &sensitivity, 0.1F, 4.F))
            config.sensitivity = sensitivity;
          session->set_relative_gain(config.sensitivity);
          ImGui::SameLine();
          if (ImGui::Button("Send Ctrl+Alt+Del"))
            (void)session->send_special(SpecialKeys::control_alt_delete);
          ImGui::SameLine();
          if (ImGui::Button("Send Alt+Tab"))
            (void)session->send_special(SpecialKeys::alt_tab);
          ImGui::BeginDisabled(snapshot.input_state != InputState::preview);
          ImGui::SetNextItemWidth(150.F);
          const char* aspect_names[] = {"Full frame", "16:9", "16:10", "4:3"};
          int aspect = static_cast<int>(config.target_aspect);
          if (ImGui::Combo("Target aspect", &aspect, aspect_names, 4))
            config.target_aspect = static_cast<TargetAspect>(aspect);
          ImGui::EndDisabled();
          ImGui::Checkbox("VSync", &config.vsync);
          ImGui::SameLine();
          ImGui::SetNextItemWidth(180.F);
          const char* color_names[] = {
              "Color: automatic", "Color: BT.601 limited", "Color: BT.601 full",
              "Color: BT.709 limited", "Color: BT.709 full"};
          int color = static_cast<int>(config.color_override);
          if (ImGui::Combo("##color", &color, color_names, 5))
            config.color_override = static_cast<ColorOverride>(color);
          ImGui::SameLine();
          ImGui::SetNextItemWidth(190.F);
#if defined(__APPLE__)
          const char* host_names[] = {"Host: Right Command",
                                      "Host: Right Control"};
          int host = config.host_scancode == 228 ? 1 : 0;
          if (ImGui::Combo("##host", &host, host_names, 2))
            config.host_scancode = host == 0 ? 231 : 228;
#else
          const char* host_names[] = {"Host: Right Control", "Host: Right GUI"};
          int host = config.host_scancode == 231 ? 1 : 0;
          if (ImGui::Combo("##host", &host, host_names, 2))
            config.host_scancode = host == 0 ? 228 : 231;
#endif
          session->set_host_key(config.host_scancode);
          ImGui::EndDisabled();
          ImGui::End();
        }
        ImGui::EndMenuBar();
      }
      ImGui::End();
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.F, 0.F});
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::Begin("KVMuxRoot", nullptr,
                 overlay_flags | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    SDL_GL_SetSwapInterval(config.vsync ? 1 : 0);
    // Chrome overlays never change the video fit or the remote input
    // mapping.
    const ImVec2 video_size = viewport->Size;
    const ImVec2 start = viewport->Pos;
    ImGui::GetWindowDrawList()->AddRectFilled(
        start, {start.x + video_size.x, start.y + video_size.y},
        IM_COL32(0, 0, 0, 255));
    if (renderer.texture_id() && renderer.width() > 0) {
      const auto fit =
          fit_video_rect({start.x, start.y, video_size.x, video_size.y},
                         renderer.width(), renderer.height());
      displayed_video_rect = fit;
      session->set_video_rect(target_input_rect(fit, config.target_aspect));
      ImGui::GetWindowDrawList()->AddImage(
          static_cast<ImTextureID>(renderer.texture_id()),
          {static_cast<float>(fit.x), static_cast<float>(fit.y)},
          {static_cast<float>(fit.x + fit.width),
           static_cast<float>(fit.y + fit.height)},
          {0, 0}, {1, 1});
    } else {
      displayed_video_rect = {};
      session->set_video_rect({});
    }
    if (region_selecting && (region_dragging || region_ready)) {
      const ImVec2 lo{std::min(region_start.x, region_end.x),
                      std::min(region_start.y, region_end.y)};
      const ImVec2 hi{std::max(region_start.x, region_end.x),
                      std::max(region_start.y, region_end.y)};
      auto* draw = ImGui::GetWindowDrawList();
      draw->AddRectFilled(lo, hi, IM_COL32(196, 95, 60, 45));
      draw->AddRect(lo, hi, IM_COL32(237, 148, 100, 255), 0.F, 0, 2.F);
    }
    if (!snapshot.video_fresh) {
      const ImVec2 warning_pos{start.x + 16.F, start.y + 16.F};
      ImGui::GetWindowDrawList()->AddText(
          warning_pos, IM_COL32(255, 100, 80, 255),
          "VIDEO UNAVAILABLE - control disabled");
    }
    if (captured) {
      const float alpha =
          static_cast<float>(std::clamp(3.0 - (now - captured_at), 0.0, 1.0));
      const char* hint = config.host_scancode == 231
                             ? "Captured - Right GUI/Command releases control"
                             : "Captured - Right Control releases control";
      const auto size = ImGui::CalcTextSize(hint);
      const ImVec2 pos{start.x + (video_size.x - size.x) * .5F, start.y + 36.F};
      auto* draw = ImGui::GetWindowDrawList();
      draw->AddRectFilled({pos.x - 8.F, pos.y - 4.F},
                          {pos.x + size.x + 8.F, pos.y + size.y + 4.F},
                          IM_COL32(11, 24, 35, static_cast<int>(225.F * alpha)),
                          4.F);
      draw->AddText(
          pos, IM_COL32(243, 241, 235, static_cast<int>(255.F * alpha)), hint);
    }
    ImGui::End();
    ImGui::PopStyleVar();
    const auto d = diagnostics.snapshot();
    char rates[64];
    if (video_bytes_per_second && control_bytes_per_second)
      std::snprintf(rates, sizeof(rates), "V:%.2fM/s C:%.2fK/s",
                    *video_bytes_per_second / (1024.0 * 1024.0),
                    *control_bytes_per_second / 1024.0);
    else
      std::snprintf(rates, sizeof(rates), "V:-- C:--");
    // Layout may have cleared event-time diagnostics after the earlier
    // session snapshot.
    const auto pointer_snapshot = session->snapshot().pointer;
    char pointer[96];
    if (pointer_snapshot.video_local)
      std::snprintf(pointer, sizeof(pointer), "P:%.0f,%.0f",
                    pointer_snapshot.video_local->first,
                    pointer_snapshot.video_local->second);
    else
      std::snprintf(pointer, sizeof(pointer), "P:--");
    char submitted[64];
    if (pointer_snapshot.submitted_absolute)
      std::snprintf(
          submitted, sizeof(submitted), "H:%u,%u",
          static_cast<unsigned>(pointer_snapshot.submitted_absolute->first),
          static_cast<unsigned>(pointer_snapshot.submitted_absolute->second));
    else if (pointer_snapshot.submitted_relative)
      std::snprintf(submitted, sizeof(submitted), "d:%d,%d",
                    pointer_snapshot.submitted_relative->first,
                    pointer_snapshot.submitted_relative->second);
    else
      std::snprintf(submitted, sizeof(submitted), "H:--");
    char line[320];
    const auto recording_status = recording.status();
    std::snprintf(line, sizeof(line),
                  "%s | %s | %.1f/%.1f fps | %s | %s | %s > %s | Rec:%s",
                  remote ? "LAN" : "Local", resolution.c_str(), d.decode_fps,
                  d.unique_present_fps, rates,
                  input_state(snapshot.input_state), pointer, submitted,
                  recording_state(recording_status.state));
    std::string status_line = line;
    if (show_status) {
      const float status_height = ImGui::GetTextLineHeight() + 8.F;
      ImGui::SetNextWindowPos(
          {viewport->Pos.x,
           viewport->Pos.y + viewport->Size.y - status_height});
      ImGui::SetNextWindowSize({viewport->Size.x, status_height});
      ImGui::SetNextWindowBgAlpha(.88F);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {6.F, 4.F});
      ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, {0.F, 0.F});
      ImGui::Begin("Status", nullptr,
                   overlay_flags | ImGuiWindowFlags_NoFocusOnAppearing |
                       ImGuiWindowFlags_NoScrollbar |
                       ImGuiWindowFlags_NoScrollWithMouse |
                       (remote_input ? ImGuiWindowFlags_NoInputs : 0));
      ImGui::PopStyleVar();
      if (!remote_input) record_local_region();
      if (!media_message.empty()) {
        ImGui::TextWrapped("%s", media_message.c_str());
        ImGui::Separator();
      }
      if (!snapshot.session_error.empty())
        ImGui::TextWrapped("Session error: %s", snapshot.session_error.c_str());
      if (!snapshot.capture.error.empty())
        ImGui::TextWrapped("Capture error: %s", snapshot.capture.error.c_str());
      if (!snapshot.control.error.empty())
        ImGui::TextWrapped("Control error: %s", snapshot.control.error.c_str());
      const auto origin = ImGui::GetCursorScreenPos();
      const float line_height = ImGui::GetTextLineHeight();
      const bool connected =
          snapshot.video_fresh &&
          snapshot.control.state == ControlConnectionState::ready;
      const ImU32 state_color =
          connected ? IM_COL32(80, 210, 120, 255) : IM_COL32(230, 155, 70, 255);
      auto* draw = ImGui::GetWindowDrawList();
      draw->AddCircleFilled({origin.x + 4.F, origin.y + line_height * .5F}, 3.F,
                            state_color);
      const float text_width = ImGui::CalcTextSize(status_line.c_str()).x;
      const float room = std::max(1.F, ImGui::GetContentRegionAvail().x - 14.F);
      const float font_size = ImGui::GetFontSize() *
                              std::min(1.F, room / std::max(1.F, text_width));
      draw->AddText(
          ImGui::GetFont(), font_size,
          {origin.x + 14.F, origin.y + (line_height - font_size) * .5F},
          ImGui::GetColorU32(ImGuiCol_Text), status_line.c_str());
      ImGui::Dummy({ImGui::GetContentRegionAvail().x, line_height});
      ImGui::End();
      ImGui::PopStyleVar();
    }
    ImGui::PopStyleVar(2);
    if (diagnostics_open && !remote_input) {
      const auto d = diagnostics.snapshot();
      const ImVec2 max_size{std::max(1.F, viewport->WorkSize.x - 16.F),
                            std::max(1.F, viewport->WorkSize.y - 16.F)};
      // A stable initial width prevents wrapped text from inflating the
      // auto-fit height.
      ImGui::SetNextWindowSize(
          {std::min(620.F, max_size.x), std::min(360.F, max_size.y)},
          ImGuiCond_FirstUseEver);
      ImGui::SetNextWindowSizeConstraints({0.F, 0.F}, max_size);
      ImGui::Begin("Diagnostics", &diagnostics_open);
      record_local_region();
      ImGui::Text("Decoded video resolution: %s", resolution.c_str());
      if (remote_client) {
        const auto video = remote_client->video_snapshot();
        ImGui::TextUnformatted("Relay transport: UDP v3 / KCP control");
        ImGui::Text("Relay codec: %s", video.codec == VideoCodec::hevc ? "H.265"
                                       : video.codec == VideoCodec::h264
                                           ? "H.264"
                                           : "MJPEG");
        ImGui::Text("Decoder backend: %s",
                    video.decoder_backend
                        ? decoder_backend_label(*video.decoder_backend)
                        : "-- (not active)");
        ImGui::Text("Hardware decoding: %s",
                    !video.hardware_verified ? "unverified"
                    : video.hardware_active  ? "active"
                                             : "not active");
        if (!video.decoder_diagnostic.empty())
          ImGui::TextWrapped("Decoder detail: %s",
                             video.decoder_diagnostic.c_str());
        ImGui::Text("Decoder recoveries: %llu",
                    static_cast<unsigned long long>(video.recoveries));
        const auto& media = video.media;
        ImGui::Text(
            "Media completed: %llu  lost: %llu  XOR fragments/frames: "
            "%llu/%llu",
            static_cast<unsigned long long>(media.received_frames),
            static_cast<unsigned long long>(media.lost_frames),
            static_cast<unsigned long long>(media.recovered_fragments),
            static_cast<unsigned long long>(media.recovered_frames));
        ImGui::Text("Media age/capacity/gap: %llu/%llu/%llu  waiting IDR: %s",
                    static_cast<unsigned long long>(media.age_losses),
                    static_cast<unsigned long long>(media.capacity_losses),
                    static_cast<unsigned long long>(media.gap_losses),
                    media.waiting_idr ? "yes" : "no");
        ImGui::Text("Media recovery reason: %s",
                    relay::media_reason_name(video.last_recovery_reason));
        if (!video.error.empty())
          ImGui::TextWrapped("Decoder error: %s", video.error.c_str());
      }
      int logical_width{}, logical_height{}, pixel_width{}, pixel_height{};
      SDL_GetWindowSize(window, &logical_width, &logical_height);
      SDL_GetWindowSizeInPixels(window, &pixel_width, &pixel_height);
      ImGui::Text("Window logical: %dx%d  Framebuffer: %dx%d", logical_width,
                  logical_height, pixel_width, pixel_height);
      if (pointer_snapshot.video_rect && pointer_snapshot.video_local) {
        const auto& rect = *pointer_snapshot.video_rect;
        const auto [x, y] = *pointer_snapshot.video_local;
        ImGui::Text("Event active rect: x=%.2f y=%.2f w=%.2f h=%.2f", rect.x,
                    rect.y, rect.width, rect.height);
        ImGui::Text("Event window: %.2f,%.2f  Desktop local: %.2f,%.2f",
                    x + rect.x, y + rect.y, x, y);
      } else {
        ImGui::TextUnformatted(
            "Event geometry: -- (move pointer while captured)");
      }
      ImGui::Text("Submitted (queue accepted, not ACK): %s", submitted);
      ImGui::TextWrapped("Capture mode: %s", d.capture_mode.c_str());
      ImGui::Text("Decode %.1f fps  Present %.1f fps", d.decode_fps,
                  d.unique_present_fps);
      ImGui::Text("Sample/frame overwrites: %llu / %llu",
                  static_cast<unsigned long long>(d.sample_mailbox_overwrites),
                  static_cast<unsigned long long>(d.frame_mailbox_overwrites));
      ImGui::Text("ACK %.2f ms  timeouts %llu", d.ack_rtt.latest_ms,
                  static_cast<unsigned long long>(d.timeouts));
      ImGui::Text("Pixel path: %s", d.pixel_path.c_str());
      ImGui::TextWrapped("%s", d.recent_error.c_str());
      ImGui::End();
    }
    menu_icon.clamp(viewport->Size.x, viewport->Size.y);
    if (!relative_capture &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
      const ImVec2 pos{viewport->Pos.x + menu_icon.pos.x,
                       viewport->Pos.y + menu_icon.pos.y};
      auto* draw = ImGui::GetForegroundDrawList();
      const ImVec2 end{pos.x + FloatingMenuIcon::size,
                       pos.y + FloatingMenuIcon::size};
      const ImVec2 mouse = ImGui::GetMousePos();
      const bool hovered = menu_icon.contains(mouse.x - viewport->Pos.x,
                                              mouse.y - viewport->Pos.y);
      draw->AddRectFilled(
          pos, end,
          ImGui::GetColorU32(menu_icon.buttons ? ImGuiCol_ButtonActive
                             : hovered         ? ImGuiCol_ButtonHovered
                                               : ImGuiCol_WindowBg),
          8.F);
      draw->AddRect(
          pos, end,
          ImGui::GetColorU32(hovered || menu_icon.buttons ? ImGuiCol_NavCursor
                                                          : ImGuiCol_Border),
          8.F);
      if (brand_texture) {
        draw->AddImage(static_cast<ImTextureID>(brand_texture),
                       {pos.x + 3.F, pos.y + 3.F}, {end.x - 3.F, end.y - 3.F});
      } else {
        // Keep the menu discoverable if an installation is missing its
        // icon.
        for (float y : {10.F, 16.F, 22.F})
          draw->AddLine({pos.x + 8.F, pos.y + y}, {pos.x + 24.F, pos.y + y},
                        ImGui::GetColorU32(ImGuiCol_Text), 2.F);
      }
    }
    popup_open = !remote_input &&
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
    if (!config.vsync) {
      constexpr auto frame_budget = std::chrono::milliseconds(1000 / 120);
      const auto elapsed = std::chrono::steady_clock::now() - frame_started;
      if (elapsed < frame_budget)
        SDL_Delay(static_cast<Uint32>(
            std::chrono::duration_cast<std::chrono::milliseconds>(frame_budget -
                                                                  elapsed)
                .count()));
    }
    diagnostics.record_present_blocking(std::chrono::steady_clock::now() -
                                        before);
  }
  recording.shutdown();
  session->shutdown();
  if (pref) {
    int w{}, h{};
    SDL_GetWindowSize(window, &w, &h);
    config.window.width = w;
    config.window.height = h;
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
