#include "app/ui_panels.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "input/us_ascii_text.hpp"

namespace kvmux {
namespace {

UiAction action(UiActionKind kind) {
  UiAction out;
  out.kind = kind;
  return out;
}

void record_local_region(UiState& ui) {
  const auto pos = ImGui::GetWindowPos();
  const auto size = ImGui::GetWindowSize();
  ui.overlay.local_regions.push_back(
      {pos.x, pos.y, pos.x + size.x, pos.y + size.y});
}

void format_pointer(const InputPointerSnapshot& pointer, char* local,
                    std::size_t local_n, char* submitted,
                    std::size_t submitted_n) {
  if (pointer.video_local)
    std::snprintf(local, local_n, "P:%.0f,%.0f", pointer.video_local->first,
                  pointer.video_local->second);
  else
    std::snprintf(local, local_n, "P:--");
  if (pointer.submitted_absolute)
    std::snprintf(submitted, submitted_n, "H:%u,%u",
                  static_cast<unsigned>(pointer.submitted_absolute->first),
                  static_cast<unsigned>(pointer.submitted_absolute->second));
  else if (pointer.submitted_relative)
    std::snprintf(submitted, submitted_n, "d:%d,%d",
                  pointer.submitted_relative->first,
                  pointer.submitted_relative->second);
  else
    std::snprintf(submitted, submitted_n, "H:--");
}

bool relay_port_ok(int port) { return port > 0 && port <= 65535; }

bool action_button(const char* label, bool enabled, const char* reason) {
  ImGui::BeginDisabled(!enabled);
  const bool pressed = ImGui::Button(label);
  ImGui::EndDisabled();
  if (!enabled && reason != nullptr &&
      ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("%s", reason);
  }
  return enabled && pressed;
}

void show_port_error(int port, const char* label) {
  if (!relay_port_ok(port)) ImGui::Text("%s must be from 1 to 65535.", label);
}

void baud_combo(int& baud) {
  // Datasheet rates. docs/ch9329-baud.md. A saved custom rate stays visible
  // until the user picks one of these.
  constexpr int kRates[] = {1200,  2400,  4800,  9600,  14400,
                            19200, 38400, 57600, 115200};
  char preview[16];
  std::snprintf(preview, sizeof(preview), "%d", baud);
  if (!ImGui::BeginCombo("##baud", preview)) return;
  for (const int rate : kRates) {
    char item[16];
    std::snprintf(item, sizeof(item), "%d", rate);
    if (ImGui::Selectable(item, baud == rate)) baud = rate;
  }
  ImGui::EndCombo();
}

std::string link_status_text(const LinkStatus& link) {
  if (link.ready) return "Ready";
  if (!link.error.empty()) return "Failed: " + link.error;
  if (link.state == "Fault" || link.state == "Permission denied" ||
      link.state == "Stalled") {
    return "Failed: " + link.state;
  }
  if (link.state == "Stopped" || link.state == "Disconnected")
    return link.state;
  return "Connecting";
}

void note_rebuild_ui(ConnectionForm& form) {
  form.capture_only = form.control_target == ControlTarget::none;
  form.screen2_open = false;
  form.second_device_id.clear();
  form.modes2.clear();
  form.selected_mode2 = -1;
  form.devices.clear();
  form.modes.clear();
  form.selected_device = form.selected_mode = -1;
}

void append_text_paste(UiState& ui, const SessionView& view,
                       UiActions& actions) {
  auto& paste_ui = ui.paste;
  const auto& paste = view.paste;
  if (view.snapshot.text_paste_active) {
    ImGui::Text("Typing ASCII: %zu/%zu scheduled", paste.scheduled_gestures,
                paste.planned_gestures);
    if (ImGui::Button("Cancel typing")) {
      paste_ui.cancelled = true;
      actions.add(action(UiActionKind::cancel_text_paste));
      paste_ui.buffer.fill('\0');
      paste_ui.removed = 0;
    }
    return;
  }
  ImGui::TextUnformatted("US ASCII only. Target keyboard layout must be US.");
  ImGui::InputTextMultiline("##ascii-text", paste_ui.buffer.data(),
                            paste_ui.buffer.size(),
                            {-1.F, ImGui::GetTextLineHeight() * 6.F});
  if (paste_ui.removed)
    ImGui::Text("Removed unsupported characters: %zu", paste_ui.removed);
  if (paste_ui.error != TextPasteError::none) {
    ImGui::TextWrapped("%s", paste_ui.error == TextPasteError::canceled
                                 ? paste_cancel_message(paste_ui.cancel_reason)
                                 : text_paste_error_label(paste_ui.error));
    if (paste_ui.error == TextPasteError::bare_carriage_return ||
        paste_ui.error == TextPasteError::non_ascii ||
        paste_ui.error == TextPasteError::unsupported) {
      if (ImGui::Button("Remove unsupported")) {
        const auto filtered = filter_us_ascii_text(paste_ui.buffer.data());
        paste_ui.removed = filtered.removed;
        std::snprintf(paste_ui.buffer.data(), paste_ui.buffer.size(), "%s",
                      filtered.text.c_str());
        paste_ui.error = TextPasteError::none;
        paste_ui.cancel_reason = PasteCancelReason::none;
      }
      ImGui::SameLine();
    }
  }
  ImGui::BeginDisabled(ui.connection.capture_only ||
                       view.snapshot.input_state != InputState::preview);
  const auto prepared = map_us_ascii_text(paste_ui.buffer.data());
  ImGui::Text("Prepared: %zu characters", prepared.normalized_characters);
  if (ImGui::Button("Type ASCII")) {
    auto start = action(UiActionKind::start_text_paste);
    start.text = paste_ui.buffer.data();
    actions.add(std::move(start));
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) {
    paste_ui.buffer.fill('\0');
    paste_ui.removed = 0;
    paste_ui.error = TextPasteError::none;
    paste_ui.cancel_reason = PasteCancelReason::none;
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (ImGui::Button("Type host clipboard")) {
    char* clipboard = SDL_GetClipboardText();
    if (!clipboard) {
      paste_ui.error = TextPasteError::unsupported;
    } else {
      paste_ui.clipboard_loaded_bytes = std::strlen(clipboard);
      const auto mapped = map_us_ascii_text(clipboard);
      paste_ui.clipboard_loaded_characters = mapped.normalized_characters;
      if (ui.debug)
        spdlog::debug(
            "Clipboard text loaded: bytes={} characters={} mapping={}",
            paste_ui.clipboard_loaded_bytes,
            paste_ui.clipboard_loaded_characters,
            debug_text_paste_error(mapped.error));
      if (paste_ui.clipboard_loaded_bytes < paste_ui.buffer.size()) {
        std::snprintf(paste_ui.buffer.data(), paste_ui.buffer.size(), "%s",
                      clipboard);
        paste_ui.removed = 0;
        paste_ui.error = mapped.error;
      } else {
        paste_ui.clipboard_loaded_bytes = 0;
        paste_ui.clipboard_loaded_characters = 0;
        paste_ui.error = TextPasteError::too_long;
      }
      SDL_free(clipboard);
    }
  }
  if (paste_ui.clipboard_loaded_bytes) {
    ImGui::Text("Clipboard loaded: %zu bytes, %zu characters",
                paste_ui.clipboard_loaded_bytes,
                paste_ui.clipboard_loaded_characters);
  }
}

void append_media_menu(UiState& ui, const SessionView& view,
                       UiActions& actions) {
  auto& media = ui.media;
  auto& recording = *ui.recording;
  const auto& status = view.recording;
  if (media.region_selecting) {
    if (media.region_confirm_requested ||
        ImGui::MenuItem("Confirm region", nullptr, false, media.region_ready))
      commit_region(media, view.renderer_width, view.renderer_height);
    if (ImGui::MenuItem("Cancel region selection")) {
      media.region_selecting = media.region_dragging = media.region_ready =
          false;
      media.region_confirm_requested = false;
    }
    ImGui::Separator();
  }
  ImGui::BeginDisabled(!view.valid_visible_cpu_frame);
  if (ImGui::MenuItem("Save full screenshot")) {
    if (recording.snapshot(*view.frame)) {
      media.snapshot_queued = true;
      set_media_notice(media, "Screenshot queued.", false);
      ImGui::CloseCurrentPopup();
    } else
      set_media_notice(media, "Could not queue screenshot.", true);
  }
  ImGui::EndDisabled();
  ImGui::BeginDisabled(!view.valid_visible_cpu_frame || !media.saved_region);
  if (ImGui::MenuItem("Save region screenshot")) {
    if (recording.snapshot(*view.frame, media.saved_region)) {
      media.snapshot_queued = true;
      set_media_notice(media, "Screenshot queued.", false);
      ImGui::CloseCurrentPopup();
    } else
      set_media_notice(media, "Could not queue screenshot.", true);
  }
  if (ImGui::MenuItem("Start scrolling", nullptr, false,
                      !ui.connection.capture_only)) {
    if (recording.start_scroll(*view.frame, *media.saved_region)) {
      media.scroll_active = true;
      actions.add(action(UiActionKind::activate_control));
      set_media_notice(media, "Scrolling capture started.", false);
      ImGui::CloseCurrentPopup();
    } else
      set_media_notice(media, "Could not start scrolling capture.", true);
  }
  ImGui::EndDisabled();
  ImGui::BeginDisabled(!view.valid_visible_cpu_frame || !media.saved_region ||
                       status.state != RecordingState::idle);
  if (ImGui::MenuItem("Start recording")) {
    const bool started = recording.start(*view.frame, *media.saved_region);
    set_media_notice(
        media, started ? "Recording started." : "Could not start recording.",
        !started);
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndDisabled();
  if (ImGui::MenuItem("Select region")) {
    media.region_selecting = true;
    media.region_dragging = media.region_ready = false;
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
  if (media.scroll_active && ImGui::MenuItem("Finish scrolling screenshot")) {
    (void)recording.finish_scroll();
    media.scroll_active = false;
  }
}

void append_connections(UiState& ui, const SessionView& view,
                        UiActions& actions) {
  auto& form = ui.connection;
  auto& config = *ui.config;
  const auto* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(
      {viewport->Pos.x + 8.F, viewport->Pos.y + ImGui::GetFrameHeight()});
  ImGui::SetNextWindowSize({std::min(900.F, viewport->Size.x - 16.F), 0.F});
  if (!(form.open_connections &&
        ImGui::Begin("Connections", &form.open_connections,
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_AlwaysAutoResize)))
    return;
  record_local_region(ui);
  const bool require_control = form.control_target != ControlTarget::none;
  if (form.close_connections_when_ready &&
      links_ready(view.video, view.control, require_control)) {
    form.open_connections = false;
    form.close_connections_when_ready = false;
  }
  ImGui::Text("Video: %s", link_status_text(view.video).c_str());
  if (require_control)
    ImGui::Text("Control: %s", link_status_text(view.control).c_str());
  ImGui::Separator();
  const bool source_pending = form.draft_video != form.video_source ||
                              form.draft_control != form.control_target;
  const VideoSource shown_video = form.draft_video;
  const ControlTarget shown_control = form.draft_control;
  const bool controls_enabled = !view.captured;
  ImGui::BeginDisabled(!controls_enabled);
  if (view.retiring) ImGui::TextUnformatted("Stopping previous session...");
  ImGui::BeginDisabled(view.retiring);
  ImGui::TextUnformatted("Video source");
  ImGui::SameLine();
  if (ImGui::RadioButton("Local device", shown_video == VideoSource::local))
    form.draft_video = VideoSource::local;
  ImGui::SameLine();
  if (ImGui::RadioButton("Relay##video", shown_video == VideoSource::relay))
    form.draft_video = VideoSource::relay;
  ImGui::TextUnformatted("Control target");
  ImGui::SameLine();
  if (ImGui::RadioButton("Local serial",
                         shown_control == ControlTarget::local_serial))
    form.draft_control = ControlTarget::local_serial;
  ImGui::SameLine();
  if (ImGui::RadioButton("Relay##control",
                         shown_control == ControlTarget::relay))
    form.draft_control = ControlTarget::relay;
  ImGui::SameLine();
  if (ImGui::RadioButton("None", shown_control == ControlTarget::none))
    form.draft_control = ControlTarget::none;
  ImGui::SetItemTooltip(
      "Video and recording only; no serial "
      "keyboard, mouse or keep-alive.");
  const char* apply_reason =
      view.retiring ? "Stopping previous session..."
                    : "Video source and control target are already applied.";
  if (action_button("Apply", source_pending && !view.retiring, apply_reason)) {
    form.video_source = form.draft_video;
    form.control_target = form.draft_control;
    note_rebuild_ui(form);
    actions.add(action(UiActionKind::rebuild_local));
  }
  if (shown_video == VideoSource::relay) {
    ImGui::InputText("Video host", form.video_host, sizeof(form.video_host));
    ImGui::InputInt("Video control port", &form.video_control_port);
    show_port_error(form.video_control_port, "Video control port");
    ImGui::InputInt("Video port", &form.video_port);
    show_port_error(form.video_port, "Video port");
    if (ImGui::BeginCombo("Decode",
                          decoder_backend_label(config.decoder_backend))) {
      for (const auto backend :
           {CodecBackend::automatic, CodecBackend::videotoolbox,
            CodecBackend::ffmpeg_software}) {
        if (ImGui::Selectable(decoder_backend_label(backend),
                              config.decoder_backend == backend))
          config.decoder_backend = backend;
      }
      ImGui::EndCombo();
    }
  }
  if (shown_control == ControlTarget::relay) {
    ImGui::InputText("Control host", form.control_host,
                     sizeof(form.control_host));
    ImGui::InputInt("Control port", &form.control_relay_port);
    show_port_error(form.control_relay_port, "Control port");
  }
  if (shown_video == VideoSource::relay ||
      shown_control == ControlTarget::relay) {
    const bool video_ports_ok = shown_video != VideoSource::relay ||
                                (relay_port_ok(form.video_control_port) &&
                                 relay_port_ok(form.video_port));
    const bool control_port_ok = shown_control != ControlTarget::relay ||
                                 relay_port_ok(form.control_relay_port);
    const char* relay_reason =
        source_pending  ? "Apply the video source and control target first."
        : view.retiring ? "Stopping previous session..."
        : !video_ports_ok || !control_port_ok ? "Enter a port from 1 to 65535."
                                              : nullptr;
    if (action_button(
            "Connect relay",
            relay_reason == nullptr && !view.retiring && !source_pending,
            relay_reason))
      actions.add(action(UiActionKind::connect_relay));
    ImGui::SameLine();
    const bool relay_open = view.relay_video_open || view.relay_control_open;
    if (action_button("Disconnect relay", relay_open,
                      "Relay is not connected."))
      actions.add(action(UiActionKind::disconnect_relay));
  }
  ImGui::EndDisabled();
  if (shown_video == VideoSource::local ||
      shown_control == ControlTarget::local_serial) {
    if (ImGui::BeginTable("connections", 4,
                          ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("setting", ImGuiTableColumnFlags_WidthFixed,
                              120.F);
      ImGui::TableSetupColumn("choice", ImGuiTableColumnFlags_WidthStretch,
                              2.F);
      ImGui::TableSetupColumn("setting2", ImGuiTableColumnFlags_WidthFixed,
                              100.F);
      ImGui::TableSetupColumn("choice2", ImGuiTableColumnFlags_WidthStretch,
                              1.F);
      ImGui::TableNextRow();
      if (shown_video == VideoSource::local) {
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("Capture device");
        ImGui::TableSetColumnIndex(1);
        if (ImGui::BeginCombo("##capture_device",
                              form.selected_device >= 0 &&
                                      form.selected_device <
                                          static_cast<int>(form.devices.size())
                                  ? form.devices[static_cast<std::size_t>(
                                                     form.selected_device)]
                                        .display_name.c_str()
                                  : "Select device")) {
          for (int i = 0; i < static_cast<int>(form.devices.size()); ++i) {
            if (ImGui::Selectable(form.devices[static_cast<std::size_t>(i)]
                                      .display_name.c_str(),
                                  i == form.selected_device)) {
              form.selected_device = i;
              form.modes.clear();
              form.selected_mode = -1;
              auto enumerate = action(UiActionKind::enumerate_modes);
              enumerate.text =
                  form.devices[static_cast<std::size_t>(i)].stable_id;
              actions.add(std::move(enumerate));
            }
          }
          ImGui::EndCombo();
        }
      }
      if (shown_control == ControlTarget::local_serial) {
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted("Serial port");
        ImGui::TableSetColumnIndex(3);
        if (ImGui::BeginCombo(
                "##serial_port",
                form.selected_port >= 0 &&
                        form.selected_port < static_cast<int>(form.ports.size())
                    ? form.ports[static_cast<std::size_t>(form.selected_port)]
                          .name.c_str()
                    : "Select port")) {
          for (int i = 0; i < static_cast<int>(form.ports.size()); ++i) {
            const auto& port = form.ports[static_cast<std::size_t>(i)];
            const std::string label =
                port.description.empty()
                    ? port.name
                    : port.name + " (" + port.description + ")";
            if (ImGui::Selectable(label.c_str(), i == form.selected_port))
              form.selected_port = i;
          }
          ImGui::EndCombo();
        }
      }
      ImGui::TableNextRow();
      if (shown_video == VideoSource::local) {
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("Capture mode");
        ImGui::TableSetColumnIndex(1);
        if (ImGui::BeginCombo(
                "##capture_mode",
                form.selected_mode >= 0 &&
                        form.selected_mode < static_cast<int>(form.modes.size())
                    ? mode_text(form.modes[static_cast<std::size_t>(
                                    form.selected_mode)])
                          .c_str()
                    : "Select mode")) {
          for (int i = 0; i < static_cast<int>(form.modes.size()); ++i)
            if (ImGui::Selectable(
                    mode_text(form.modes[static_cast<std::size_t>(i)]).c_str(),
                    i == form.selected_mode))
              form.selected_mode = i;
          ImGui::EndCombo();
        }
      }
      if (shown_control == ControlTarget::local_serial) {
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted("Baud rate");
        ImGui::TableSetColumnIndex(3);
        baud_combo(config.serial_baud_rate);
      }
      ImGui::EndTable();
    }
    const bool preview_ready =
        !source_pending && !view.retiring && form.selected_device >= 0 &&
        form.selected_device < static_cast<int>(form.devices.size()) &&
        form.selected_mode >= 0 &&
        form.selected_mode < static_cast<int>(form.modes.size());
    const char* preview_reason =
        source_pending  ? "Apply the video source and control target first."
        : view.retiring ? "Stopping previous session..."
                        : "Select a capture device and mode.";
    if (shown_video == VideoSource::local &&
        action_button("Start preview", preview_ready, preview_reason)) {
      const auto& device =
          form.devices[static_cast<std::size_t>(form.selected_device)];
      if (form.screen2_open && device.stable_id == form.second_device_id) {
        set_media_notice(ui.media, "That device is in use by screen 2.", true);
      } else {
        form.close_connections_when_ready = true;
        form.primary_device_id = device.stable_id;
        actions.add(action(UiActionKind::start_preview));
      }
    }
    if (shown_control == ControlTarget::local_serial) {
      if (shown_video == VideoSource::local) ImGui::SameLine();
      const bool port_ready =
          !source_pending && form.selected_port >= 0 &&
          form.selected_port < static_cast<int>(form.ports.size());
      const char* serial_reason =
          source_pending ? "Apply the video source and control target first."
                         : "Select a serial port.";
      if (action_button("Connect", port_ready, serial_reason)) {
        form.close_connections_when_ready = true;
        actions.add(action(UiActionKind::connect_serial));
      }
      ImGui::SameLine();
      if (ImGui::Button("Disconnect"))
        actions.add(action(UiActionKind::disconnect_serial));
      ImGui::Checkbox("Connect serial on startup", &config.auto_connect_serial);
    }
    if (shown_video == VideoSource::local) {
      ImGui::SeparatorText("Second screen (Relative mouse only)");
      if (!form.screen2_open) {
        if (ImGui::BeginCombo("Screen 2 device",
                              form.selected_device2 >= 0 &&
                                      form.selected_device2 <
                                          static_cast<int>(form.devices.size())
                                  ? form.devices[static_cast<std::size_t>(
                                                     form.selected_device2)]
                                        .display_name.c_str()
                                  : "Select device")) {
          for (int i = 0; i < static_cast<int>(form.devices.size()); ++i) {
            if (form.devices[static_cast<std::size_t>(i)].stable_id ==
                form.primary_device_id)
              continue;
            if (ImGui::Selectable(form.devices[static_cast<std::size_t>(i)]
                                      .display_name.c_str(),
                                  i == form.selected_device2)) {
              form.selected_device2 = i;
              form.modes2.clear();
              form.selected_mode2 = -1;
              auto enumerate = action(UiActionKind::enumerate_screen2_modes);
              enumerate.text =
                  form.devices[static_cast<std::size_t>(i)].stable_id;
              actions.add(std::move(enumerate));
            }
          }
          ImGui::EndCombo();
        }
        if (ImGui::BeginCombo(
                "Screen 2 mode",
                form.selected_mode2 >= 0 &&
                        form.selected_mode2 <
                            static_cast<int>(form.modes2.size())
                    ? mode_text(form.modes2[static_cast<std::size_t>(
                                    form.selected_mode2)])
                          .c_str()
                    : "Select mode")) {
          for (int i = 0; i < static_cast<int>(form.modes2.size()); ++i)
            if (ImGui::Selectable(
                    mode_text(form.modes2[static_cast<std::size_t>(i)]).c_str(),
                    i == form.selected_mode2))
              form.selected_mode2 = i;
          ImGui::EndCombo();
        }
        const bool screen2_ready =
            !source_pending && !view.retiring && form.selected_device2 >= 0 &&
            form.selected_device2 < static_cast<int>(form.devices.size()) &&
            form.selected_mode2 >= 0 &&
            form.selected_mode2 < static_cast<int>(form.modes2.size());
        const char* screen2_reason =
            source_pending  ? "Apply the video source and control target first."
            : view.retiring ? "Stopping previous session..."
                            : "Select a screen 2 device and mode.";
        if (action_button("Open screen 2", screen2_ready, screen2_reason)) {
          if (form.devices[static_cast<std::size_t>(form.selected_device2)]
                  .stable_id == form.primary_device_id) {
            set_media_notice(ui.media,
                             "Screen 2 must use a different capture device.",
                             true);
          } else {
            actions.add(action(UiActionKind::open_screen2));
          }
        }
      } else if (ImGui::Button("Close screen 2")) {
        actions.add(action(UiActionKind::close_screen2));
      }
    }
  }
  if (ImGui::RadioButton("Absolute",
                         config.mouse_mode == MouseMode::absolute)) {
    config.mouse_mode = MouseMode::absolute;
    auto mode = action(UiActionKind::set_mouse_mode);
    mode.mouse = config.mouse_mode;
    actions.add(std::move(mode));
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("Relative",
                         config.mouse_mode == MouseMode::relative)) {
    config.mouse_mode = MouseMode::relative;
    auto mode = action(UiActionKind::set_mouse_mode);
    mode.mouse = config.mouse_mode;
    actions.add(std::move(mode));
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(180.F);
  float sensitivity = static_cast<float>(config.sensitivity);
  if (ImGui::SliderFloat("Sensitivity", &sensitivity, 0.1F, 4.F))
    config.sensitivity = sensitivity;
  auto gain = action(UiActionKind::set_relative_gain);
  gain.gain = config.sensitivity;
  actions.add(std::move(gain));
  ImGui::SameLine();
  ImGui::BeginDisabled(form.capture_only);
  bool send_ctrl_alt_del = ImGui::Button("Send Ctrl+Alt+Del");
  ImGui::SameLine();
  bool send_alt_tab = ImGui::Button("Send Alt+Tab");
  ImGui::EndDisabled();
  if (send_ctrl_alt_del) {
    auto special = action(UiActionKind::send_special);
    special.special = SpecialKeys::control_alt_delete;
    actions.add(std::move(special));
  }
  if (send_alt_tab) {
    auto special = action(UiActionKind::send_special);
    special.special = SpecialKeys::alt_tab;
    actions.add(std::move(special));
  }
  ImGui::SetNextItemWidth(190.F);
#if defined(__APPLE__)
  const char* host_names[] = {"Host: Right Command", "Host: Right Control"};
  int host = config.host_scancode == 228 ? 1 : 0;
  if (ImGui::Combo("##host", &host, host_names, 2))
    config.host_scancode = host == 0 ? 231 : 228;
#else
  const char* host_names[] = {"Host: Right Control", "Host: Right GUI"};
  int host = config.host_scancode == 231 ? 1 : 0;
  if (ImGui::Combo("##host", &host, host_names, 2))
    config.host_scancode = host == 0 ? 228 : 231;
#endif
  auto host_key = action(UiActionKind::set_host_key);
  host_key.usage = config.host_scancode;
  actions.add(std::move(host_key));
  ImGui::EndDisabled();
  ImGui::End();
}

const char* host_key_name(std::uint16_t code) {
#if defined(__APPLE__)
  if (code == 231) return "Right Command";
  if (code == 228) return "Right Control";
#else
  if (code == 228) return "Right Control";
  if (code == 231) return "Right GUI";
#endif
  return "Host";
}

void queue_special(UiActions& actions, SpecialKeys keys) {
  auto special = action(UiActionKind::send_special);
  special.special = keys;
  actions.add(std::move(special));
}

void append_command_menus(UiState& ui, const SessionView& view,
                          UiActions& actions) {
  auto& config = *ui.config;
  if (ImGui::BeginMenu("连接")) {
    if (ImGui::MenuItem("Connections")) ui.connection.open_connections = true;
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("显示")) {
    ImGui::MenuItem("VSync", nullptr, &config.vsync);
    if (ImGui::BeginMenu("Color")) {
      const std::pair<const char*, ColorOverride> colors[] = {
          {"Automatic", ColorOverride::automatic},
          {"BT.601 limited", ColorOverride::bt601_limited},
          {"BT.601 full", ColorOverride::bt601_full},
          {"BT.709 limited", ColorOverride::bt709_limited},
          {"BT.709 full", ColorOverride::bt709_full},
      };
      for (const auto& [label, value] : colors) {
        if (ImGui::MenuItem(label, nullptr, config.color_override == value))
          config.color_override = value;
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Target aspect")) {
      ImGui::BeginDisabled(view.snapshot.input_state != InputState::preview);
      const std::pair<const char*, TargetAspect> aspects[] = {
          {"Full frame", TargetAspect::full_frame},
          {"16:9", TargetAspect::ratio_16_9},
          {"16:10", TargetAspect::ratio_16_10},
          {"4:3", TargetAspect::ratio_4_3},
      };
      for (const auto& [label, value] : aspects) {
        if (ImGui::MenuItem(label, nullptr, config.target_aspect == value))
          config.target_aspect = value;
      }
      ImGui::EndDisabled();
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem(ui.overlay.fullscreen ? "Exit fullscreen"
                                              : "Fullscreen")) {
      ui.overlay.fullscreen = !ui.overlay.fullscreen;
      SDL_SetWindowFullscreen(ui.window, ui.overlay.fullscreen);
    }
    ImGui::MenuItem("Status overlay", nullptr, &ui.overlay.show_status);
    if (ImGui::MenuItem("Diagnostics", nullptr, ui.overlay.diagnostics_open))
      ui.overlay.diagnostics_open = !ui.overlay.diagnostics_open;
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("输入")) {
    if (ImGui::MenuItem("Type ASCII", nullptr, false,
                        !ui.connection.capture_only))
      ui.paste.open = true;
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("动作")) {
    append_media_menu(ui, view, actions);
    ImGui::Separator();
    const bool control = !ui.connection.capture_only;
    if (ImGui::MenuItem("Send Ctrl+Alt+Del", nullptr, false, control))
      queue_special(actions, SpecialKeys::control_alt_delete);
    if (ImGui::MenuItem("Send Alt+Tab", nullptr, false, control))
      queue_special(actions, SpecialKeys::alt_tab);
    if (control &&
        ImGui::MenuItem("Keep target awake", nullptr, &config.keep_alive))
      actions.add(action(UiActionKind::set_keep_alive));
    ImGui::EndMenu();
  }
}

}  // namespace

void sync_media_feedback(UiState& ui, const RecordingStatus& media_status) {
  auto& media = ui.media;
  if (media_status.snapshot_error != media.displayed_snapshot_error) {
    media.displayed_snapshot_error = media_status.snapshot_error;
    if (!media.displayed_snapshot_error.empty()) {
      media.snapshot_queued = false;
      set_media_notice(
          media, "Screenshot error: " + media.displayed_snapshot_error, true);
    }
  }
  if (media_status.last_snapshot_path != media.displayed_snapshot_path) {
    media.displayed_snapshot_path = media_status.last_snapshot_path;
    if (!media.displayed_snapshot_path.empty()) media.snapshot_queued = false;
  }
  if (media_status.scroll_error != media.displayed_scroll_error) {
    media.displayed_scroll_error = media_status.scroll_error;
    if (!media.displayed_scroll_error.empty()) {
      set_media_notice(
          media, "Scrolling screenshot error: " + media.displayed_scroll_error,
          true);
      media.scroll_active = false;
      media.scroll_sample_after.reset();
      ui.recording->cancel_scroll();
    }
  }
  if (media_status.last_scroll_path != media.displayed_scroll_path) {
    media.displayed_scroll_path = media_status.last_scroll_path;
    if (!media.displayed_scroll_path.empty()) {
      media.scroll_active = false;
      media.scroll_sample_after.reset();
    }
  }
}

void sync_paste_feedback(UiState& ui, const TextPasteSnapshot& paste) {
  if (paste.active) {
    ui.paste.seen_active = true;
  } else if (ui.paste.seen_active) {
    ui.paste.seen_active = false;
    ui.paste.error = paste.error;
    ui.paste.cancel_reason = paste.cancel_reason;
  }
}

void log_debug_session(UiState& ui, const SessionView& view,
                       const TextPasteSnapshot& paste) {
  const char* paste_status = debug_text_paste_error(paste.error);
  if (view.snapshot.text_paste_active) {
    paste_status = "active";
    ui.paste.was_active = true;
  } else if (ui.paste.was_active) {
    paste_status = ui.paste.cancelled                    ? "cancelled"
                   : paste.error != TextPasteError::none ? "failed"
                                                         : "scheduled";
    ui.paste.was_active = false;
    ui.paste.cancelled = false;
  }
  const char* const snapshot_status =
      !view.recording.snapshot_error.empty()       ? "failed"
      : ui.media.snapshot_queued                   ? "queued"
      : !view.recording.last_snapshot_path.empty() ? "saved"
                                                   : "idle";
  const auto status =
      std::string("capture=") + view.video.state +
      " capture-error=" + link_error_token(view.video) +
      " control=" + view.control.state +
      " control-error=" + link_error_token(view.control) +
      " input=" + input_state_label(view.snapshot.input_state) +
      " paste=" + paste_status +
      " paste-reason=" + paste_cancel_reason_name(paste.cancel_reason) +
      " paste-bytes=" + std::to_string(paste.source_bytes) +
      " paste-chars=" + std::to_string(paste.normalized_characters) +
      " paste-scheduled=" + std::to_string(paste.scheduled_gestures) + "/" +
      std::to_string(paste.planned_gestures) + " media=snapshot-" +
      snapshot_status +
      " recording=" + debug_recording_state(view.recording.state) +
      " session-error=" +
      (view.snapshot.session_error.empty() ? "none" : "present");
  if (status != ui.last_debug_status) {
    spdlog::debug("Session {}", status);
    ui.last_debug_status = status;
  }
}

void prepare_overlay_frame(UiState& ui, const SessionView& view) {
  const double now = ImGui::GetTime();
  if (view.captured && !ui.overlay.was_captured) ui.overlay.captured_at = now;
  if (view.remote_input && !ui.overlay.local_ui_input_seen) {
    ImGui::GetIO().ClearInputKeys();
    ImGui::GetIO().ClearEventsQueue();
    ui.overlay.chrome_until = 0.0;
  }
  ui.overlay.local_ui_input_seen = false;
  ui.overlay.was_captured = view.captured;
}

UiActions draw_chrome(UiState& ui, const SessionView& view) {
  UiActions actions;
  const auto* viewport = ImGui::GetMainViewport();
  const double now = ImGui::GetTime();
  const auto mouse = ImGui::GetIO().MousePos;
  const bool at_top = mouse.x >= viewport->Pos.x &&
                      mouse.x < viewport->Pos.x + viewport->Size.x &&
                      mouse.y >= viewport->Pos.y &&
                      mouse.y < viewport->Pos.y + 8.F;
  if (!view.remote_input && at_top) ui.overlay.chrome_until = now + 1.0;
  ui.overlay.chrome_visible =
      !view.remote_input &&
      (!ui.overlay.fullscreen || ui.overlay.open_floating_menu ||
       ui.connection.open_connections || now < ui.overlay.chrome_until ||
       ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId));
  const auto overlay_flags =
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar |
      ImGuiWindowFlags_NoSavedSettings;
  if (ui.overlay.chrome_visible) {
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize({viewport->Size.x, ImGui::GetFrameHeight()});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, {0.F, 0.F});
    ImGui::Begin("Chrome", nullptr,
                 overlay_flags | ImGuiWindowFlags_MenuBar |
                     ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    record_local_region(ui);
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
      ui.overlay.chrome_until = now + 1.0;
    if (ImGui::BeginMenuBar()) {
      if (ui.overlay.open_floating_menu) {
        ImGui::OpenPopup("Floating menu");
        ui.overlay.open_floating_menu = false;
      }
      append_command_menus(ui, view, actions);
      ImGui::SetNextWindowPos({viewport->Pos.x + ui.overlay.menu_icon.pos.x,
                               viewport->Pos.y + ui.overlay.menu_icon.pos.y +
                                   FloatingMenuIcon::size},
                              ImGuiCond_Appearing);
      if (ImGui::BeginPopup("Floating menu")) {
        record_local_region(ui);
        append_command_menus(ui, view, actions);
        ImGui::EndPopup();
      }
      if (ui.paste.open) {
        ImGui::OpenPopup("Type ASCII");
        ui.paste.open = false;
      }
      ImGui::SetNextWindowPos(
          {viewport->Pos.x + 8.F, viewport->Pos.y + ImGui::GetFrameHeight()});
      ImGui::SetNextWindowSize({std::min(600.F, viewport->Size.x - 16.F), 0.F});
      if (ImGui::BeginPopup("Type ASCII")) {
        record_local_region(ui);
        append_text_paste(ui, view, actions);
        ImGui::EndPopup();
      }
      append_connections(ui, view, actions);
      ImGui::EndMenuBar();
    }
    ImGui::End();
  }
  return actions;
}

UiActions draw_video(UiState& ui, const SessionView& view) {
  UiActions actions;
  const auto* viewport = ImGui::GetMainViewport();
  const auto overlay_flags =
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar |
      ImGuiWindowFlags_NoSavedSettings;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.F, 0.F});
  ImGui::SetNextWindowPos(viewport->Pos);
  ImGui::SetNextWindowSize(viewport->Size);
  ImGui::Begin("KVMuxRoot", nullptr,
               overlay_flags | ImGuiWindowFlags_NoInputs |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);
  const ImVec2 video_size = viewport->Size;
  const ImVec2 start = viewport->Pos;
  ImGui::GetWindowDrawList()->AddRectFilled(
      start, {start.x + video_size.x, start.y + video_size.y},
      IM_COL32(0, 0, 0, 255));
  if (view.has_texture) {
    const auto fit =
        fit_video_rect({start.x, start.y, video_size.x, video_size.y},
                       view.renderer_width, view.renderer_height);
    ui.media.displayed_video_rect = fit;
    auto rect = action(UiActionKind::set_video_rect);
    rect.video_rect = target_input_rect(fit, ui.config->target_aspect);
    actions.add(std::move(rect));
    ImGui::GetWindowDrawList()->AddImage(
        static_cast<ImTextureID>(view.texture),
        {static_cast<float>(fit.x), static_cast<float>(fit.y)},
        {static_cast<float>(fit.x + fit.width),
         static_cast<float>(fit.y + fit.height)},
        {0, 0}, {1, 1});
  } else {
    ui.media.displayed_video_rect = {};
    actions.add(action(UiActionKind::set_video_rect));
    constexpr const char* guide =
        "选择视频源 → 开始预览 → 连接控制 → 点击画面开始控制";
    constexpr float logo = 96.F;
    const ImVec2 text_size = ImGui::CalcTextSize(guide);
    const float block =
        (ui.brand_texture != 0 ? logo + 16.F : 0.F) + text_size.y;
    float text_y = start.y + (video_size.y - block) * 0.5F;
    auto* empty = ImGui::GetWindowDrawList();
    if (ui.brand_texture != 0) {
      const float x = start.x + (video_size.x - logo) * 0.5F;
      empty->AddImage(static_cast<ImTextureID>(ui.brand_texture), {x, text_y},
                      {x + logo, text_y + logo});
      text_y += logo + 16.F;
    }
    empty->AddText({start.x + (video_size.x - text_size.x) * 0.5F, text_y},
                   ImGui::GetColorU32(ImGuiCol_Text), guide);
  }
  if (ui.media.region_selecting &&
      (ui.media.region_dragging || ui.media.region_ready)) {
    const ImVec2 lo{std::min(ui.media.region_start.x, ui.media.region_end.x),
                    std::min(ui.media.region_start.y, ui.media.region_end.y)};
    const ImVec2 hi{std::max(ui.media.region_start.x, ui.media.region_end.x),
                    std::max(ui.media.region_start.y, ui.media.region_end.y)};
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(lo, hi, IM_COL32(196, 95, 60, 45));
    draw->AddRect(lo, hi, IM_COL32(237, 148, 100, 255), 0.F, 0, 2.F);
  }
  if (view.captured) {
    const double now = ImGui::GetTime();
    const float alpha = static_cast<float>(
        std::clamp(3.0 - (now - ui.overlay.captured_at), 0.0, 1.0));
    const char* hint = ui.config->host_scancode == 231
                           ? "Captured - Right GUI/Command releases control"
                           : "Captured - Right Control releases control";
    const auto size = ImGui::CalcTextSize(hint);
    const ImVec2 pos{start.x + (video_size.x - size.x) * .5F, start.y + 36.F};
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled({pos.x - 8.F, pos.y - 4.F},
                        {pos.x + size.x + 8.F, pos.y + size.y + 4.F},
                        IM_COL32(11, 24, 35, static_cast<int>(225.F * alpha)),
                        4.F);
    draw->AddText(pos, IM_COL32(243, 241, 235, static_cast<int>(255.F * alpha)),
                  hint);
  } else if (view.snapshot.input_state == InputState::preview &&
             view.has_texture) {
    char hint[128];
    std::snprintf(hint, sizeof(hint), "点击画面开始控制，Host 键：%s",
                  host_key_name(ui.config->host_scancode));
    const auto size = ImGui::CalcTextSize(hint);
    const ImVec2 pos{start.x + (video_size.x - size.x) * .5F, start.y + 36.F};
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled({pos.x - 8.F, pos.y - 4.F},
                        {pos.x + size.x + 8.F, pos.y + size.y + 4.F},
                        ImGui::GetColorU32(ImGuiCol_WindowBg), 4.F);
    draw->AddText(pos, ImGui::GetColorU32(ImGuiCol_Text), hint);
  }
  ImGui::End();
  ImGui::PopStyleVar();
  return actions;
}

UiActions draw_status(UiState& ui, const SessionView& view) {
  UiActions actions;
  const auto* viewport = ImGui::GetMainViewport();
  const auto& d = view.diagnostics;
  const auto recording_state = ui.recording->status().state;
  const char* place = "Local";
  if (ui.connection.video_source == VideoSource::relay &&
      ui.connection.control_target == ControlTarget::relay)
    place = "LAN";
  else if (ui.connection.video_source == VideoSource::relay)
    place = "LAN video";
  else if (ui.connection.control_target == ControlTarget::relay)
    place = "LAN control";
  char left[160];
  if (recording_state == RecordingState::idle) {
    std::snprintf(left, sizeof(left), "%s · %s", place,
                  input_state_label(view.snapshot.input_state));
  } else {
    std::snprintf(left, sizeof(left), "%s · %s · Rec:%s", place,
                  input_state_label(view.snapshot.input_state),
                  recording_state_label(recording_state));
  }
  char right[80];
  std::snprintf(right, sizeof(right), "%s  %.1f/%.1f fps",
                view.resolution.c_str(), d.decode_fps, d.unique_present_fps);
  const bool pasting = view.snapshot.text_paste_active;
  if (!ui.overlay.show_status) return actions;
  const auto overlay_flags =
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar |
      ImGuiWindowFlags_NoSavedSettings;
  const float text_height = ImGui::GetTextLineHeight();
  float status_height = text_height + 8.F;
  if (pasting) status_height += ImGui::GetFrameHeightWithSpacing();
  ImGui::SetNextWindowPos(
      {viewport->Pos.x, viewport->Pos.y + viewport->Size.y - status_height});
  ImGui::SetNextWindowSize({viewport->Size.x, status_height});
  ImGui::SetNextWindowBgAlpha(.88F);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {6.F, 4.F});
  ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, {0.F, 0.F});
  ImGui::Begin("Status", nullptr,
               overlay_flags | ImGuiWindowFlags_NoFocusOnAppearing |
                   ImGuiWindowFlags_NoScrollbar |
                   ImGuiWindowFlags_NoScrollWithMouse |
                   (view.remote_input ? ImGuiWindowFlags_NoInputs : 0));
  ImGui::PopStyleVar();
  if (!view.remote_input) record_local_region(ui);
  const auto origin = ImGui::GetCursorScreenPos();
  const float line_height = ImGui::GetTextLineHeight();
  const ImU32 state_color =
      links_ready(view.video, view.control,
                  ui.connection.control_target != ControlTarget::none)
          ? IM_COL32(80, 210, 120, 255)
          : IM_COL32(230, 155, 70, 255);
  auto* draw = ImGui::GetWindowDrawList();
  draw->AddCircleFilled({origin.x + 4.F, origin.y + line_height * .5F}, 3.F,
                        state_color);
  const float right_width = ImGui::CalcTextSize(right).x;
  const float left_room =
      std::max(1.F, ImGui::GetContentRegionAvail().x - right_width - 22.F);
  const float left_width = ImGui::CalcTextSize(left).x;
  const float font_size = ImGui::GetFontSize() *
                          std::min(1.F, left_room / std::max(1.F, left_width));
  draw->AddText(ImGui::GetFont(), font_size,
                {origin.x + 14.F, origin.y + (line_height - font_size) * .5F},
                ImGui::GetColorU32(ImGuiCol_Text), left);
  draw->AddText(
      {origin.x + ImGui::GetContentRegionAvail().x - right_width, origin.y},
      ImGui::GetColorU32(ImGuiCol_Text), right);
  ImGui::Dummy({ImGui::GetContentRegionAvail().x, line_height});
  if (pasting) {
    ImGui::Text("Typing ASCII: %zu/%zu scheduled",
                view.paste.scheduled_gestures, view.paste.planned_gestures);
    ImGui::SameLine();
    if (ImGui::Button("Cancel typing")) {
      ui.paste.cancelled = true;
      ui.paste.buffer.fill('\0');
      ui.paste.removed = 0;
      actions.add(action(UiActionKind::cancel_text_paste));
    }
  }
  ImGui::End();
  ImGui::PopStyleVar();
  return actions;
}

void append_banner_line(std::string& text, const std::string& line) {
  if (line.empty()) return;
  if (!text.empty()) text.push_back('\n');
  text += line;
}

void draw_one_banner(UiState& ui, const char* title, const std::string& text,
                     std::string& dismissed, bool remote, float& y) {
  if (text.empty() || text == dismissed) return;
  const auto* viewport = ImGui::GetMainViewport();
  const float width = std::max(1.F, viewport->Size.x - 16.F);
  ImGui::SetNextWindowPos({viewport->Pos.x + 8.F, y});
  ImGui::SetNextWindowSize({width, 0.F});
  const auto flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoFocusOnAppearing |
                     (remote ? ImGuiWindowFlags_NoInputs : 0);
  if (!ImGui::Begin(title, nullptr, flags)) {
    ImGui::End();
    return;
  }
  if (!remote) record_local_region(ui);
  const float close_width =
      ImGui::CalcTextSize("Close").x + ImGui::GetStyle().FramePadding.x * 2.F;
  ImGui::PushTextWrapPos(ImGui::GetCursorPos().x +
                         ImGui::GetContentRegionAvail().x - close_width - 8.F);
  ImGui::TextUnformatted(text.c_str());
  ImGui::PopTextWrapPos();
  ImGui::SameLine();
  if (ImGui::Button("Close")) dismissed = text;
  y += ImGui::GetWindowSize().y + 8.F;
  ImGui::End();
}

void draw_banners(UiState& ui, const SessionView& view) {
  std::string errors;
  if (!view.snapshot.session_error.empty()) {
    append_banner_line(errors, "Session error: " + view.snapshot.session_error);
  }
  if (!view.video.error.empty()) {
    append_banner_line(errors, "Capture error: " + view.video.error);
  }
  if (!view.control.error.empty()) {
    append_banner_line(errors, "Control error: " + view.control.error);
  }
  if (link_has_hint(view.video)) append_banner_line(errors, view.video.hint);
  if (link_has_hint(view.control))
    append_banner_line(errors, view.control.hint);
  if (ui.media.media_is_error)
    append_banner_line(errors, ui.media.media_message);
  const auto* viewport = ImGui::GetMainViewport();
  const float chrome =
      ui.overlay.chrome_visible ? ImGui::GetFrameHeight() : 0.F;
  float y = viewport->Pos.y + chrome + 8.F;
  const bool remote = view.remote_input;
  draw_one_banner(ui, "Error banner", errors, ui.overlay.dismissed_errors,
                  remote, y);
  if (!ui.media.media_is_error) {
    draw_one_banner(ui, "Notice", ui.media.media_message,
                    ui.overlay.dismissed_notice, remote, y);
  }
}

UiActions draw_diagnostics(UiState& ui, const SessionView& view) {
  UiActions actions;
  if (!(ui.overlay.diagnostics_open && !view.remote_input)) return actions;
  const auto* viewport = ImGui::GetMainViewport();
  const auto& d = view.diagnostics;
  const ImVec2 max_size{std::max(1.F, viewport->WorkSize.x - 16.F),
                        std::max(1.F, viewport->WorkSize.y - 16.F)};
  ImGui::SetNextWindowSize(
      {std::min(620.F, max_size.x), std::min(360.F, max_size.y)},
      ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints({0.F, 0.F}, max_size);
  ImGui::Begin("Diagnostics", &ui.overlay.diagnostics_open);
  record_local_region(ui);
  ImGui::Text("Decoded video resolution: %s", view.resolution.c_str());
  if (view.relay_video) {
    const auto& video = *view.relay_video;
    ImGui::TextUnformatted("Relay transport: UDP v3 / KCP control");
    ImGui::Text("Relay codec: %s", video.codec == VideoCodec::hevc   ? "H.265"
                                   : video.codec == VideoCodec::h264 ? "H.264"
                                                                     : "MJPEG");
    ImGui::Text("Decoder backend: %s",
                video.decoder_backend
                    ? decoder_backend_label(*video.decoder_backend)
                    : "-- (not active)");
    ImGui::Text("Hardware decoding: %s", !video.hardware_verified ? "unverified"
                                         : video.hardware_active
                                             ? "active"
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
  SDL_GetWindowSize(ui.window, &logical_width, &logical_height);
  SDL_GetWindowSizeInPixels(ui.window, &pixel_width, &pixel_height);
  ImGui::Text("Window logical: %dx%d  Framebuffer: %dx%d", logical_width,
              logical_height, pixel_width, pixel_height);
  char pointer[96];
  char submitted[64];
  format_pointer(view.pointer, pointer, sizeof(pointer), submitted,
                 sizeof(submitted));
  char video_rate[32] = "V:--";
  char control_rate[32] = "C:--";
  if (view.video_bytes_per_second)
    std::snprintf(video_rate, sizeof(video_rate), "V:%.2fM/s",
                  *view.video_bytes_per_second / (1024.0 * 1024.0));
  if (view.control_bytes_per_second)
    std::snprintf(control_rate, sizeof(control_rate), "C:%.2fK/s",
                  *view.control_bytes_per_second / 1024.0);
  ImGui::Text("Pointer: %s", pointer);
  ImGui::Text("Rates: %s %s", video_rate, control_rate);
  if (view.pointer.video_rect && view.pointer.video_local) {
    const auto& rect = *view.pointer.video_rect;
    const auto [x, y] = *view.pointer.video_local;
    ImGui::Text("Event active rect: x=%.2f y=%.2f w=%.2f h=%.2f", rect.x,
                rect.y, rect.width, rect.height);
    ImGui::Text("Event window: %.2f,%.2f  Desktop local: %.2f,%.2f", x + rect.x,
                y + rect.y, x, y);
  } else {
    ImGui::TextUnformatted("Event geometry: -- (move pointer while captured)");
  }
  ImGui::Text("Submitted (queue accepted, not ACK): %s", submitted);
  ImGui::TextWrapped("Capture mode: %s", d.capture_mode.c_str());
  ImGui::Text("Decode %.1f fps  Present %.1f fps", d.decode_fps,
              d.unique_present_fps);
  ImGui::Text("Sample/frame overwrites: %llu / %llu",
              static_cast<unsigned long long>(d.sample_mailbox_overwrites),
              static_cast<unsigned long long>(d.frame_mailbox_overwrites));
  const auto latency_row = [](const char* label, const LatencySnapshot& l) {
    ImGui::Text("%s: %.2f ms (p95 %.2f, n=%zu)", label, l.latest_ms, l.p95_ms,
                l.samples);
  };
  ImGui::SeparatorText("Pointer path (UI thread -> serial ACK)");
  ImGui::Text("Mouse motion: %.0f ev/s  total %llu  folded %llu",
              d.mouse_events_per_second,
              static_cast<unsigned long long>(d.mouse_motion_events),
              static_cast<unsigned long long>(d.mouse_motion_folded));
  latency_row("SDL stamp -> UI poll", d.sdl_event_wait);
  latency_row("UI receipt -> input handler done (incl. lock wait)",
              d.input_processing);
  if (ui.connection.control_target == ControlTarget::relay) {
    ImGui::TextUnformatted(
        "Serial dequeue / queue age / ACK RTT / queue-merged / rejected / "
        "timeouts: N/A (measured on the relay server's serial worker)");
    ImGui::Text("Ordinary pending: %s",
                view.snapshot.control.ordinary_input_pending ? "yes" : "no");
  } else if (ui.connection.control_target == ControlTarget::local_serial) {
    latency_row("UI receipt -> serial dequeue", d.sdl_to_control_dequeue);
    latency_row("Queue age (submit -> dequeue)", d.input_queue_age);
    latency_row("Ordinary ACK RTT (begin -> ACK, not target response)",
                d.ack_rtt);
    ImGui::Text(
        "Queue-merged motion: %llu  ordinary pending: %s  rejected: %llu  "
        "ACK timeouts: %llu",
        static_cast<unsigned long long>(
            view.snapshot.control.merged_motion_events),
        view.snapshot.control.ordinary_input_pending ? "yes" : "no",
        static_cast<unsigned long long>(view.snapshot.control.rejected_events),
        static_cast<unsigned long long>(d.timeouts));
  }
  ImGui::SeparatorText("Video");
  latency_row("Capture -> GPU submit", d.sample_to_gpu_submit);
  latency_row("SwapWindow blocking (excl. limiter)", d.present_blocking);
  ImGui::Text("Pixel path: %s", d.pixel_path.c_str());
  ImGui::TextWrapped("%s", d.recent_error.c_str());
  ImGui::End();
  return actions;
}

void draw_menu_icon(UiState& ui, const SessionView& view) {
  const auto* viewport = ImGui::GetMainViewport();
  auto& icon = ui.overlay.menu_icon;
  icon.clamp(viewport->Size.x, viewport->Size.y);
  if (view.relative_capture ||
      ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
    return;
  const ImVec2 pos{viewport->Pos.x + icon.pos.x, viewport->Pos.y + icon.pos.y};
  auto* draw = ImGui::GetForegroundDrawList();
  const ImVec2 end{pos.x + FloatingMenuIcon::size,
                   pos.y + FloatingMenuIcon::size};
  const ImVec2 mouse = ImGui::GetMousePos();
  const bool hovered =
      icon.contains(mouse.x - viewport->Pos.x, mouse.y - viewport->Pos.y);
  draw->AddRectFilled(pos, end,
                      ImGui::GetColorU32(icon.buttons ? ImGuiCol_ButtonActive
                                         : hovered    ? ImGuiCol_ButtonHovered
                                                      : ImGuiCol_WindowBg),
                      8.F);
  draw->AddRect(pos, end,
                ImGui::GetColorU32(hovered || icon.buttons ? ImGuiCol_NavCursor
                                                           : ImGuiCol_Border),
                8.F);
  if (ui.brand_texture) {
    draw->AddImage(static_cast<ImTextureID>(ui.brand_texture),
                   {pos.x + 3.F, pos.y + 3.F}, {end.x - 3.F, end.y - 3.F});
  } else {
    for (float y : {10.F, 16.F, 22.F})
      draw->AddLine({pos.x + 8.F, pos.y + y}, {pos.x + 24.F, pos.y + y},
                    ImGui::GetColorU32(ImGuiCol_Text), 2.F);
  }
}

}  // namespace kvmux
