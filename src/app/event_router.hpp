#pragma once

#include <SDL3/SDL.h>

#include "app/ui_state.hpp"

namespace kvmux {

// Button masks and focus live here for the life of the frame loop. The
// session pointer is refreshed each frame before polling; routing calls the
// session immediately so edges stay in poll order, ahead of tick().
struct EventHost {
  KvmSession* session{nullptr};
  Diagnostics* diagnostics{nullptr};
  SDL_Window* window{nullptr};
  SDL_Window* window2{nullptr};
  SDL_WindowID main_window_id{};
  SDL_WindowID second_window_id{};
  bool running{true};
  bool close_second_requested{false};
  bool second_valid{false};
  const VideoFrame* current_frame{nullptr};
  int renderer_width{0};
  int renderer_height{0};
  Uint32 local_buttons{0};
  Uint32 remote_buttons{0};
  bool main_focused{true};
  bool second_focused{false};
  bool focus_release_pending{false};
};

void route_sdl_event(const SDL_Event& event, EventHost& host, UiState& ui);
void settle_window_focus(EventHost& host);

}  // namespace kvmux
