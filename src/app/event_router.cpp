#include "app/event_router.hpp"

#include <imgui_impl_sdl3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <optional>

namespace kvmux {
namespace {

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

void minimize_window(EventHost& host) {
  host.remote_buttons = 0;
  host.session->minimized();
  if (host.session->text_paste_active() &&
      (host.main_focused || host.second_focused))
    host.session->focus_returned();
}

SDL_WindowID event_window_id(const SDL_Event& event) {
  switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
      return event.key.windowID;
    case SDL_EVENT_MOUSE_MOTION:
      return event.motion.windowID;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
      return event.button.windowID;
    case SDL_EVENT_MOUSE_WHEEL:
      return event.wheel.windowID;
    default:
      return event.type >= SDL_EVENT_WINDOW_FIRST &&
                     event.type <= SDL_EVENT_WINDOW_LAST
                 ? event.window.windowID
                 : 0;
  }
}

bool is_injectable_input(const SDL_Event& event) {
  return event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP ||
         event.type == SDL_EVENT_MOUSE_MOTION ||
         event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
         event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
         event.type == SDL_EVENT_MOUSE_WHEEL;
}

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

void route_sdl_event(const SDL_Event& event, EventHost& host, UiState& ui) {
  auto* session = host.session;
  auto* diagnostics = host.diagnostics;
  auto& config = *ui.config;
  auto& media = ui.media;
  auto& overlay = ui.overlay;
  const auto received_at = std::chrono::steady_clock::now();
  if (const auto polled = SDL_GetTicksNS(); is_injectable_input(event) &&
                                            event.common.timestamp != 0 &&
                                            polled >= event.common.timestamp)
    diagnostics->record_sdl_event_wait(
        std::chrono::nanoseconds(polled - event.common.timestamp));
  if (event.type == SDL_EVENT_MOUSE_MOTION)
    diagnostics->record_mouse_motion(received_at);
  const auto event_state = session->input_state();
  const bool remote_input = event_state == InputState::captured ||
                            event_state == InputState::recovering ||
                            event_state == InputState::arming;
  int event_width{}, event_height{};
  SDL_GetWindowSize(host.window, &event_width, &event_height);
  const bool icon_enabled =
      !overlay.popup_open && !host.remote_buttons &&
      !(remote_input && config.mouse_mode == MouseMode::relative);
  const bool preview = event_state == InputState::preview;
  if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED ||
      event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
    const bool gained = event.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
    const SDL_WindowID wid = event_window_id(event);
    if (wid == host.main_window_id)
      host.main_focused = gained;
    else if (wid == host.second_window_id)
      host.second_focused = gained;
    if (gained && (wid == host.main_window_id || wid == host.second_window_id))
      host.session->focus_returned();
    if (!gained) host.focus_release_pending = true;
  }
  const bool paste_cancel_event =
      session->text_paste_active() ||
      (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) &&
          event.key.scancode == config.host_scancode;
  if (const auto wid = event_window_id(event);
      wid != 0 && wid != host.main_window_id) {
    if (wid != host.second_window_id) return;
    if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
      host.close_second_requested = true;
    } else if (event.type == SDL_EVENT_WINDOW_MINIMIZED ||
               event.type == SDL_EVENT_WINDOW_HIDDEN) {
      minimize_window(host);
    } else if (is_injectable_input(event) &&
               config.mouse_mode == MouseMode::relative &&
               (remote_input || paste_cancel_event ||
                !(overlay.popup_open || overlay.open_floating_menu))) {
      if (!host.second_valid && event.type != SDL_EVENT_KEY_DOWN &&
          event.type != SDL_EVENT_KEY_UP)
        return;
      if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && remote_input)
        host.remote_buttons |= SDL_BUTTON_MASK(event.button.button);
      if (event.type == SDL_EVENT_MOUSE_BUTTON_UP)
        host.remote_buttons &= ~SDL_BUTTON_MASK(event.button.button);
      if (event.type == SDL_EVENT_MOUSE_MOTION) {
        session->handle_input(
            {InputRelativeMotion{event.motion.xrel, event.motion.yrel},
             received_at});
      } else {
        const Rect input =
            target_input_rect(media.displayed_video_rect, config.target_aspect);
        const auto cx = static_cast<float>(input.x + input.width / 2);
        const auto cy = static_cast<float>(input.y + input.height / 2);
        SDL_Event routed = event;
        if (routed.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
            routed.type == SDL_EVENT_MOUSE_BUTTON_UP) {
          routed.button.x = cx;
          routed.button.y = cy;
        } else if (routed.type == SDL_EVENT_MOUSE_WHEEL) {
          routed.wheel.mouse_x = cx;
          routed.wheel.mouse_y = cy;
        }
        auto converted = to_input(routed);
        converted.received_at = received_at;
        session->handle_input(converted);
      }
    }
    return;
  }
  if (media.scroll_active && (event.type == SDL_EVENT_WINDOW_FOCUS_LOST ||
                              event.type == SDL_EVENT_WINDOW_MINIMIZED ||
                              event.type == SDL_EVENT_WINDOW_HIDDEN ||
                              (event.type == SDL_EVENT_KEY_DOWN &&
                               event.key.scancode == SDL_SCANCODE_ESCAPE))) {
    const char* reason = "focus";
    if (event.type == SDL_EVENT_KEY_DOWN &&
        event.key.scancode == SDL_SCANCODE_ESCAPE)
      reason = "escape";
    else if (event.type == SDL_EVENT_WINDOW_MINIMIZED)
      reason = "minimized";
    else if (event.type == SDL_EVENT_WINDOW_HIDDEN)
      reason = "hidden";
    ui.recording->cancel_scroll(reason);
    media.scroll_active = false;
    media.scroll_sample_after.reset();
  }
  if (media.region_selecting) {
    const bool escape = event.type == SDL_EVENT_KEY_DOWN &&
                        event.key.scancode == SDL_SCANCODE_ESCAPE;
    const bool lost = event.type == SDL_EVENT_WINDOW_FOCUS_LOST ||
                      event.type == SDL_EVENT_WINDOW_MINIMIZED ||
                      event.type == SDL_EVENT_WINDOW_HIDDEN;
    if (!preview || lost || escape) {
      cancel_region_selection(media, !preview ? "not-preview"
                                     : escape ? "escape"
                                              : "focus");
      if (event.type == SDL_EVENT_KEY_DOWN) return;
    }
    const bool enter = event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                       (event.key.scancode == SDL_SCANCODE_RETURN ||
                        event.key.scancode == SDL_SCANCODE_KP_ENTER);
    if (media.region_selecting && preview && media.region_ready && enter) {
      (void)confirm_region_selection(media, host.renderer_width,
                                     host.renderer_height);
      return;
    }
    if (media.region_selecting && preview && media.region_ready &&
        media.region_buttons && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        event.button.button == SDL_BUTTON_LEFT) {
      if (media.region_confirm_hit.contains(event.button.x, event.button.y)) {
        (void)confirm_region_selection(media, host.renderer_width,
                                       host.renderer_height);
        return;
      }
      if (media.region_cancel_hit.contains(event.button.x, event.button.y)) {
        cancel_region_selection(media, "button");
        return;
      }
    }
    if (media.region_selecting && preview &&
        event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        event.button.button == SDL_BUTTON_LEFT &&
        media.displayed_video_rect.contains(event.button.x, event.button.y)) {
      media.region_start = media.region_end = {event.button.x, event.button.y};
      media.region_dragging = true;
      media.region_ready = false;
      media.region_buttons = false;
      return;
    }
    if (media.region_dragging && event.type == SDL_EVENT_MOUSE_MOTION) {
      media.region_end = {
          static_cast<float>(std::clamp(
              static_cast<double>(event.motion.x), media.displayed_video_rect.x,
              media.displayed_video_rect.x + media.displayed_video_rect.width)),
          static_cast<float>(std::clamp(
              static_cast<double>(event.motion.y), media.displayed_video_rect.y,
              media.displayed_video_rect.y +
                  media.displayed_video_rect.height))};
      return;
    }
    if (media.region_dragging && event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
        event.button.button == SDL_BUTTON_LEFT) {
      media.region_dragging = false;
      media.region_ready = true;
      return;
    }
    if (preview && (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                    event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
                    event.type == SDL_EVENT_MOUSE_MOTION ||
                    event.type == SDL_EVENT_MOUSE_WHEEL))
      return;
  }
  auto& icon = overlay.menu_icon;
  icon.clamp(static_cast<float>(event_width), static_cast<float>(event_height));
  const ImVec2 stored_icon = icon.pos;
  icon.pos = menu_icon_shown_pos(overlay, ui.connection,
                                 static_cast<float>(event_width),
                                 static_cast<float>(event_height));
  const bool icon_hit =
      icon.consume(event, icon_enabled, static_cast<float>(event_width),
                   static_cast<float>(event_height));
  icon.pos = stored_icon;
  if (icon_hit) {
    if (overlay.menu_icon.clicked) {
      if (session->text_paste_active())
        log_unknown_paste_cancel("floating_menu");
      session->release_control(PasteCancelReason::unknown);
      overlay.open_floating_menu = true;
      overlay.menu_icon.clicked = false;
    }
    return;
  }
  if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST ||
      event.type == SDL_EVENT_WINDOW_MINIMIZED ||
      event.type == SDL_EVENT_WINDOW_HIDDEN) {
    host.remote_buttons = 0;
    overlay.open_floating_menu = false;
  }
  bool local_click = false;
  if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
      event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
    const Uint32 bit = SDL_BUTTON_MASK(event.button.button);
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
      local_click =
          (event_state == InputState::preview &&
           (overlay.popup_open || overlay.open_floating_menu ||
            std::any_of(overlay.local_regions.begin(),
                        overlay.local_regions.end(), [&](const ImVec4& r) {
                          return event.button.x >= r.x &&
                                 event.button.x < r.z &&
                                 event.button.y >= r.y && event.button.y < r.w;
                        })));
      if (local_click) host.local_buttons |= bit;
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
               (host.local_buttons & bit)) {
      host.local_buttons &= ~bit;
      local_click = true;
    }
  }
  if (!remote_input || local_click) {
    ImGui_ImplSDL3_ProcessEvent(&event);
    if (local_click) overlay.local_ui_input_seen = true;
  }
  if (event.type == SDL_EVENT_QUIT ||
      event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
    session->release_control(PasteCancelReason::shutdown);
    host.running = false;
    return;
  }
  if (event.type == SDL_EVENT_WINDOW_MINIMIZED ||
      event.type == SDL_EVENT_WINDOW_HIDDEN)
    minimize_window(host);
  const bool injectable_event = event.type == SDL_EVENT_KEY_DOWN ||
                                event.type == SDL_EVENT_KEY_UP ||
                                event.type == SDL_EVENT_MOUSE_MOTION ||
                                event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                                event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
                                event.type == SDL_EVENT_MOUSE_WHEEL;
  if (local_click && injectable_event) return;
  if (overlay.popup_open && !remote_input && injectable_event &&
      !paste_cancel_event)
    return;
  if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && remote_input)
    host.remote_buttons |= SDL_BUTTON_MASK(event.button.button);
  if (event.type == SDL_EVENT_MOUSE_BUTTON_UP)
    host.remote_buttons &= ~SDL_BUTTON_MASK(event.button.button);
  if (media.scroll_active && remote_input &&
      event.type == SDL_EVENT_MOUSE_WHEEL) {
    const double sign =
        event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0 : 1.0;
    if (event.wheel.y * sign < 0.0 && host.current_frame)
      media.scroll_sample_after = std::pair{host.current_frame->generation,
                                            host.current_frame->sequence};
  }
  if (event.type == SDL_EVENT_MOUSE_MOTION &&
      config.mouse_mode == MouseMode::relative) {
    session->handle_input(
        {InputRelativeMotion{event.motion.xrel, event.motion.yrel},
         received_at});
  } else if (injectable_event) {
    auto converted = to_input(event);
    converted.received_at = received_at;
    session->handle_input(converted);
  }
}

void settle_window_focus(EventHost& host) {
  if (!host.focus_release_pending) return;
  host.focus_release_pending = false;
  SDL_Window* const focus = SDL_GetKeyboardFocus();
  if (focus == host.window) host.main_focused = true;
  if (host.window2 && focus == host.window2) host.second_focused = true;
  if (!host.main_focused && !host.second_focused && focus != host.window &&
      (!host.window2 || focus != host.window2)) {
    host.session->focus_lost();
    host.remote_buttons = 0;
  } else {
    host.session->focus_returned();
  }
}

}  // namespace kvmux
