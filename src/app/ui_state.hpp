#pragma once

#include <SDL3/SDL.h>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app/kvm_session.hpp"
#include "app/recording.hpp"
#include "network/relay_client.hpp"
#include "support/config.hpp"
#include "support/diagnostics.hpp"

namespace kvmux {

// One link's display text. `ready` is the predicate the status dot and the
// connections auto-close already shared: video uses video_fresh, control
// uses ControlConnectionState::ready. `hint` stays empty until a caller has
// a sentence that is neither the state label nor the raw error.
struct LinkStatus {
  std::string state;
  std::string error;
  std::string hint;
  bool ready{false};
};

inline const char* capture_state_label(CaptureState state) {
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
inline const char* control_state_label(ControlConnectionState state) {
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
inline const char* recording_state_label(RecordingState state) {
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
inline const char* input_state_label(InputState state) {
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
inline const char* text_paste_error_label(TextPasteError error) {
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
    case TextPasteError::invalid_utf8:
      return "非法 UTF-8，整段未发送。";
    case TextPasteError::surrogate:
      return "有代理项，整段未发送。";
    case TextPasteError::above_bmp:
      return "有 BMP 以外的码点，整段未发送。";
    case TextPasteError::control:
      return "有控制字符，整段未发送。";
    case TextPasteError::num_lock:
      return "目标机 NumLock 关闭或尚未读到，未发送按键。";
    case TextPasteError::too_long:
      return "Text is limited to 65536 characters.";
    case TextPasteError::canceled:
      return "粘贴已取消。";
  }
  return "Could not prepare text.";
}
inline const char* paste_cancel_message(PasteCancelReason reason) {
  switch (reason) {
    case PasteCancelReason::none:
    case PasteCancelReason::user:
      return "粘贴已取消。";
    case PasteCancelReason::key_press:
      return "检测到按键，粘贴已取消。";
    case PasteCancelReason::focus_lost:
      return "窗口失去焦点，粘贴已取消。";
    case PasteCancelReason::video_stale:
      return "画面中断，粘贴已取消。";
    case PasteCancelReason::ui_stall:
      return "界面无响应，粘贴已取消。";
    case PasteCancelReason::link_lost:
      return "控制链路中断，粘贴已取消。";
    case PasteCancelReason::ack_timeout:
      return "设备应答超时，粘贴已取消。";
    case PasteCancelReason::device_fault:
      return "控制设备故障，粘贴已取消。";
    case PasteCancelReason::shutdown:
      return "正在退出，粘贴已取消。";
    case PasteCancelReason::not_ready:
      return "当前无法开始粘贴。";
    case PasteCancelReason::unknown:
      return "粘贴已停止（原因未知）";
  }
  return "粘贴已取消。";
}
inline const char* debug_text_paste_error(TextPasteError error) {
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
    case TextPasteError::invalid_utf8:
      return "error-invalid-utf8";
    case TextPasteError::surrogate:
      return "error-surrogate";
    case TextPasteError::above_bmp:
      return "error-above-bmp";
    case TextPasteError::control:
      return "error-control";
    case TextPasteError::num_lock:
      return "error-num-lock";
    case TextPasteError::too_long:
      return "error-too-long";
    case TextPasteError::canceled:
      return "canceled";
  }
  return "error-unknown";
}
inline const char* debug_recording_state(RecordingState state) {
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
inline const char* decoder_backend_label(CodecBackend backend) {
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
inline std::string mode_text(const CaptureMode& mode) {
  return std::to_string(mode.width) + "x" + std::to_string(mode.height) +
         " @ " + std::to_string(mode.frame_rate.numerator) + "/" +
         std::to_string(mode.frame_rate.denominator) + " " +
         mode.device_format_name;
}
inline const char* link_error_token(const LinkStatus& link) {
  return link.error.empty() ? "none" : "present";
}
inline bool link_has_hint(const LinkStatus& link) { return !link.hint.empty(); }

inline LinkStatus make_video_link(const KvmSessionSnapshot& snapshot) {
  LinkStatus link;
  link.state = capture_state_label(snapshot.capture.state);
  link.error = snapshot.capture.error;
  link.ready = snapshot.video_fresh;
  return link;
}
inline LinkStatus make_control_link(const KvmSessionSnapshot& snapshot) {
  LinkStatus link;
  link.state = control_state_label(snapshot.control.state);
  link.error = snapshot.control.error;
  link.ready = snapshot.control.state == ControlConnectionState::ready;
  return link;
}
inline bool links_ready(const LinkStatus& video, const LinkStatus& control,
                        bool require_control) {
  return video.ready && (!require_control || control.ready);
}

// Fixed-icon pointer gestures, owned before ImGui and remote routing.
// Keyboard and lifecycle events must still reach the session while the icon
// owns a click.
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

struct ConnectionForm {
  VideoSource video_source{VideoSource::local};
  ControlTarget control_target{ControlTarget::local_serial};
  VideoSource draft_video{VideoSource::local};
  ControlTarget draft_control{ControlTarget::local_serial};
  char video_host[64]{"127.0.0.1"};
  char control_host[64]{"127.0.0.1"};
  int video_control_port{17000};
  int video_port{17001};
  int control_relay_port{17000};
  std::vector<DeviceInfo> devices;
  std::vector<CaptureMode> modes;
  std::vector<SerialPortInfo> ports;
  std::vector<CaptureMode> modes2;
  int selected_device{-1};
  int selected_mode{-1};
  int selected_port{-1};
  int selected_device2{-1};
  int selected_mode2{-1};
  std::string primary_device_id;
  std::string second_device_id;
  bool close_connections_when_ready{false};
  bool open_connections{false};
  bool capture_only{false};
  bool screen2_open{false};
  // Last drawn Connections window, viewport-relative. Zero means it was not
  // drawn, so the floating icon keeps its stored position.
  bool panel_shown{false};
  float panel_x{};
  float panel_y{};
  float panel_w{};
  float panel_h{};
  std::chrono::steady_clock::time_point next_serial_scan{};
};

struct MediaUi {
  std::string media_message;
  bool media_is_error{false};
  std::filesystem::path displayed_snapshot_path;
  std::filesystem::path displayed_scroll_path;
  std::string displayed_snapshot_error;
  std::string displayed_scroll_error;
  bool snapshot_queued{false};
  std::optional<FrameCrop> saved_region;
  bool region_selecting{false};
  bool region_dragging{false};
  bool region_ready{false};
  bool region_confirm_requested{false};
  bool region_buttons{false};
  Rect region_confirm_hit{};
  Rect region_cancel_hit{};
  bool scroll_active{false};
  std::optional<std::pair<std::uint64_t, std::uint64_t>> scroll_sample_after;
  ImVec2 region_start{};
  ImVec2 region_end{};
  Rect displayed_video_rect{};
};

struct PasteUi {
  std::array<char, 65537> buffer{};
  std::size_t removed{0};
  TextPasteError error{TextPasteError::none};
  PasteCancelReason cancel_reason{PasteCancelReason::none};
  std::size_t clipboard_loaded_bytes{0};
  std::size_t clipboard_loaded_characters{0};
  bool open{false};
  bool hex_open{false};
  bool was_active{false};
  bool cancelled{false};
  bool seen_active{false};
};

struct OverlayFlags {
  bool show_status{true};
  bool fullscreen{false};
  bool diagnostics_open{false};
  bool chrome_visible{true};
  double chrome_until{0.0};
  double captured_at{0.0};
  bool was_captured{false};
  bool open_floating_menu{false};
  bool popup_open{false};
  bool local_ui_input_seen{false};
  std::string dismissed_errors;
  std::string dismissed_notice;
  FloatingMenuIcon menu_icon{};
  std::vector<ImVec4> local_regions;
};

// Drawn and hit-tested position. Does not write menu_icon.pos.
inline ImVec2 menu_icon_shown_pos(const OverlayFlags& overlay,
                                  const ConnectionForm& form, float width,
                                  float height) {
  const float size = FloatingMenuIcon::size;
  const auto fits = [&](ImVec2 candidate) {
    candidate.x = std::clamp(candidate.x, 0.F, std::max(0.F, width - size));
    candidate.y = std::clamp(candidate.y, 0.F, std::max(0.F, height - size));
    return candidate;
  };
  const ImVec2 stored = fits(overlay.menu_icon.pos);
  if (!form.panel_shown) return stored;
  if (form.panel_w <= 0.F || form.panel_h <= 0.F)
    return fits({width - size - 16.F, height - size - 48.F});
  const auto overlaps = [&](ImVec2 candidate) {
    return candidate.x < form.panel_x + form.panel_w &&
           candidate.x + size > form.panel_x &&
           candidate.y < form.panel_y + form.panel_h &&
           candidate.y + size > form.panel_y;
  };
  if (!overlaps(stored)) return stored;
  const ImVec2 candidates[] = {
      {form.panel_x + form.panel_w + 8.F, stored.y},
      {stored.x, form.panel_y + form.panel_h + 8.F},
      {width - size - 16.F, 16.F},
      {16.F, height - size - 48.F},
  };
  for (ImVec2 candidate : candidates) {
    candidate = fits(candidate);
    if (!overlaps(candidate)) return candidate;
  }
  return fits(candidates[3]);
}

// Panels mutate these groups. config, recording, and the window are the
// objects the draw code already wrote; they are not session calls.
struct UiState {
  ConnectionForm connection;
  MediaUi media;
  PasteUi paste;
  OverlayFlags overlay;
  Config* config{nullptr};
  Recording* recording{nullptr};
  SDL_Window* window{nullptr};
  bool debug{false};
  unsigned brand_texture{0};
  std::string last_debug_status;
};

// Read-only session picture for one frame. Link text is only `video` and
// `control`. Pointer and relay fields are filled after the calls that change
// them, matching the previous read points in the frame.
struct SessionView {
  KvmSessionSnapshot snapshot{};
  LinkStatus video{};
  LinkStatus control{};
  RecordingStatus recording{};
  TextPasteSnapshot paste{};
  bool retiring{false};
  bool captured{false};
  bool remote_input{false};
  bool relative_capture{false};
  bool valid_visible_cpu_frame{false};
  const VideoFrame* frame{nullptr};
  std::string resolution{"--"};
  DiagnosticsSnapshot diagnostics{};
  bool relay_video_open{false};
  bool relay_control_open{false};
  std::optional<double> video_bytes_per_second;
  std::optional<double> control_bytes_per_second;
  std::optional<relay::ClientVideoSnapshot> relay_video;
  InputPointerSnapshot pointer{};
  int renderer_width{0};
  int renderer_height{0};
  std::uintptr_t texture{0};
  bool has_texture{false};
};

enum class UiActionKind {
  set_keep_alive,
  rebuild_local,
  connect_relay,
  disconnect_relay,
  enumerate_modes,
  enumerate_screen2_modes,
  start_preview,
  connect_serial,
  disconnect_serial,
  open_screen2,
  close_screen2,
  set_mouse_mode,
  set_relative_gain,
  send_special,
  set_host_key,
  cancel_text_paste,
  start_text_paste,
  start_windows_hex_paste,
  activate_control,
  set_video_rect,
};

// Drawn panels append these. The frame loop runs each panel's list before
// the next panel uses the session, in the order the widgets produced them.
struct UiAction {
  UiActionKind kind{};
  bool flag{false};
  double gain{};
  std::uint16_t usage{};
  SpecialKeys special{SpecialKeys::control_alt_delete};
  MouseMode mouse{MouseMode::absolute};
  Rect video_rect{};
  std::string text;
};

struct UiActions {
  std::vector<UiAction> items;
  void add(UiAction action) { items.push_back(std::move(action)); }
};

// Saves the dragged rectangle into MediaUi. A failed confirm stays in
// selection so the user can drag again.
inline void commit_region(MediaUi& media, int width, int height) {
  const auto mapped =
      width > 0 && height > 0
          ? crop_from_display(
                media.displayed_video_rect.x, media.displayed_video_rect.y,
                media.displayed_video_rect.width,
                media.displayed_video_rect.height, media.region_start.x,
                media.region_start.y, media.region_end.x, media.region_end.y,
                static_cast<unsigned>(width), static_cast<unsigned>(height))
          : std::nullopt;
  const FrameCrop raw = mapped.value_or(FrameCrop{});
  const auto crop = mapped ? align_even_crop(raw, static_cast<unsigned>(width),
                                             static_cast<unsigned>(height))
                           : std::nullopt;
  if (mapped && crop) {
    spdlog::debug("region align: raw={}x{}+{},{} aligned={}x{}+{},{}",
                  mapped->width, mapped->height, mapped->x, mapped->y,
                  crop->width, crop->height, crop->x, crop->y);
    media.saved_region = crop;
    media.media_message = "Region saved.";
    media.media_is_error = false;
    media.region_selecting = false;
    media.region_dragging = false;
    media.region_ready = false;
    media.region_confirm_requested = false;
    media.region_buttons = false;
  } else {
    media.media_message = mapped ? "Region must be at least 2x2 pixels."
                                 : "Select a larger region.";
    media.media_is_error = true;
  }
}

inline void begin_region_selection(MediaUi& media) {
  media.region_selecting = true;
  media.region_dragging = false;
  media.region_ready = false;
  media.region_confirm_requested = false;
  media.region_buttons = false;
  spdlog::debug("region select: enter");
}

inline void cancel_region_selection(MediaUi& media, const char* reason) {
  const bool active =
      media.region_selecting || media.region_dragging || media.region_ready;
  media.region_selecting = false;
  media.region_dragging = false;
  media.region_ready = false;
  media.region_confirm_requested = false;
  media.region_buttons = false;
  if (active) {
    spdlog::debug("region cancel: reason={}", reason != nullptr ? reason : "-");
  }
}

inline bool confirm_region_selection(MediaUi& media, int width, int height) {
  if (!media.region_selecting) return false;
  commit_region(media, width, height);
  if (!media.region_selecting && media.saved_region) {
    const FrameCrop& crop = *media.saved_region;
    spdlog::debug("region confirm: x={} y={} width={} height={}", crop.x,
                  crop.y, crop.width, crop.height);
    return true;
  }
  spdlog::debug("region confirm: rejected message={}", media.media_message);
  return false;
}

inline void clear_saved_region(MediaUi& media) {
  if (!media.saved_region) return;
  const FrameCrop crop = *media.saved_region;
  media.saved_region.reset();
  media.media_message = "Region cleared.";
  media.media_is_error = false;
  spdlog::debug("region clear: x={} y={} width={} height={}", crop.x, crop.y,
                crop.width, crop.height);
}

inline void set_media_notice(MediaUi& media, std::string text, bool error) {
  media.media_message = std::move(text);
  media.media_is_error = error;
}

}  // namespace kvmux
