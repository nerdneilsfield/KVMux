#pragma once

#include "app/ui_state.hpp"

namespace kvmux {

void sync_media_feedback(UiState& ui, const RecordingStatus& status);
void sync_paste_feedback(UiState& ui, const TextPasteSnapshot& paste);
void log_debug_session(UiState& ui, const SessionView& view,
                       const TextPasteSnapshot& paste);
void prepare_overlay_frame(UiState& ui, const SessionView& view);

[[nodiscard]] UiActions draw_chrome(UiState& ui, const SessionView& view);
[[nodiscard]] UiActions draw_video(UiState& ui, const SessionView& view);
[[nodiscard]] UiActions draw_status(UiState& ui, const SessionView& view);
void draw_banners(UiState& ui, const SessionView& view);
[[nodiscard]] UiActions draw_diagnostics(UiState& ui, const SessionView& view);
void draw_menu_icon(UiState& ui, const SessionView& view);

}  // namespace kvmux
