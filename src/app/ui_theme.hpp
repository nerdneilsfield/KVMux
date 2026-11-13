#pragma once

#include <imgui.h>

#include <cstdint>

namespace kvmux::theme {

// Channels are the brand palette plus the interface extensions recorded in
// assets/branding/README.md. Connection orange is not a button fill: warm
// white on #C45F3C is below 4.5:1. accent_fill_active matches accent_fill.
struct Rgb {
  std::uint8_t r;
  std::uint8_t g;
  std::uint8_t b;
};

inline constexpr Rgb bg{0x0B, 0x18, 0x23};
inline constexpr Rgb surface{0x20, 0x2A, 0x33};
inline constexpr Rgb surface_raised{0x2C, 0x38, 0x42};
inline constexpr Rgb border{0x57, 0x65, 0x70};
inline constexpr Rgb text{0xF3, 0xF1, 0xEB};
inline constexpr Rgb text_muted{0xBF, 0xC5, 0xC9};
inline constexpr Rgb accent{0xC4, 0x5F, 0x3C};
inline constexpr Rgb accent_fill{0x63, 0x3A, 0x2E};
inline constexpr Rgb accent_fill_hover{0x84, 0x44, 0x2E};
inline constexpr Rgb accent_fill_active{0x63, 0x3A, 0x2E};
inline constexpr Rgb on_accent{0xF3, 0xF1, 0xEB};
inline constexpr Rgb highlight{0xED, 0x94, 0x64};
inline constexpr Rgb video_well{0x08, 0x12, 0x1B};
inline constexpr Rgb ok{0x3D, 0xDC, 0x97};
inline constexpr Rgb warn{0xE6, 0xC8, 0x4A};
inline constexpr Rgb error{0xFF, 0x6B, 0x6B};

inline constexpr float k_border_alpha = 0.40F;
inline constexpr float k_resize_alpha = 0.30F;

inline constexpr float unit(std::uint8_t channel) {
  return static_cast<float>(channel) / 255.F;
}

inline ImVec4 rgba(Rgb color, float alpha = 1.F) {
  return {unit(color.r), unit(color.g), unit(color.b), alpha};
}

inline ImU32 packed(Rgb color, std::uint8_t alpha = 255) {
  return IM_COL32(color.r, color.g, color.b, alpha);
}

inline void push_primary_button() {
  ImGui::PushStyleColor(ImGuiCol_Button, rgba(accent_fill));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rgba(accent_fill_hover));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, rgba(accent_fill_active));
  ImGui::PushStyleColor(ImGuiCol_Text, rgba(on_accent));
}

inline void pop_primary_button() { ImGui::PopStyleColor(4); }

inline void apply() {
  ImGui::StyleColorsDark();
  auto& style = ImGui::GetStyle();
  style.WindowRounding = 8.F;
  style.PopupRounding = 8.F;
  style.FrameRounding = 4.F;
  style.GrabRounding = 4.F;
  style.WindowBorderSize = 1.F;
  style.PopupBorderSize = 1.F;
  style.FrameBorderSize = 1.F;
  style.WindowPadding = {8.F, 8.F};
  style.FramePadding = {8.F, 8.F};
  style.ItemSpacing = {8.F, 8.F};
  style.ItemInnerSpacing = {8.F, 8.F};
  style.CellPadding = {8.F, 8.F};
  auto& c = style.Colors;
  const auto base = rgba(bg);
  const auto panel = rgba(surface);
  const auto raised = rgba(surface_raised);
  const auto line = rgba(border);
  const auto silver = rgba(text_muted);
  const auto white = rgba(text);
  const auto copper = rgba(accent);
  const auto mark = rgba(highlight);
  c[ImGuiCol_Text] = white;
  c[ImGuiCol_TextDisabled] = silver;
  c[ImGuiCol_WindowBg] = panel;
  c[ImGuiCol_ChildBg] = panel;
  c[ImGuiCol_PopupBg] = panel;
  c[ImGuiCol_MenuBarBg] = panel;
  c[ImGuiCol_Border] = rgba(text_muted, k_border_alpha);
  c[ImGuiCol_BorderShadow] = rgba(bg, 0.F);
  c[ImGuiCol_FrameBg] = raised;
  c[ImGuiCol_FrameBgHovered] = raised;
  c[ImGuiCol_FrameBgActive] = raised;
  c[ImGuiCol_TitleBg] = panel;
  c[ImGuiCol_TitleBgActive] = raised;
  c[ImGuiCol_TitleBgCollapsed] = panel;
  c[ImGuiCol_Button] = panel;
  c[ImGuiCol_ButtonHovered] = raised;
  c[ImGuiCol_ButtonActive] = raised;
  c[ImGuiCol_Header] = raised;
  c[ImGuiCol_HeaderHovered] = raised;
  c[ImGuiCol_HeaderActive] = line;
  c[ImGuiCol_CheckMark] = mark;
  c[ImGuiCol_SliderGrab] = copper;
  c[ImGuiCol_SliderGrabActive] = mark;
  c[ImGuiCol_ScrollbarBg] = panel;
  c[ImGuiCol_ScrollbarGrab] = line;
  c[ImGuiCol_ScrollbarGrabHovered] = silver;
  c[ImGuiCol_ScrollbarGrabActive] = copper;
  c[ImGuiCol_Separator] = line;
  c[ImGuiCol_SeparatorHovered] = copper;
  c[ImGuiCol_SeparatorActive] = mark;
  c[ImGuiCol_ResizeGrip] = rgba(text_muted, k_resize_alpha);
  c[ImGuiCol_ResizeGripHovered] = copper;
  c[ImGuiCol_ResizeGripActive] = mark;
  c[ImGuiCol_Tab] = panel;
  c[ImGuiCol_TabHovered] = raised;
  c[ImGuiCol_TabSelected] = raised;
  c[ImGuiCol_TabSelectedOverline] = copper;
  c[ImGuiCol_TabDimmed] = base;
  c[ImGuiCol_TabDimmedSelected] = panel;
  c[ImGuiCol_TabDimmedSelectedOverline] = silver;
  c[ImGuiCol_TextSelectedBg] = raised;
  c[ImGuiCol_NavCursor] = mark;
  c[ImGuiCol_PlotLines] = silver;
  c[ImGuiCol_PlotLinesHovered] = mark;
  c[ImGuiCol_PlotHistogram] = copper;
  c[ImGuiCol_PlotHistogramHovered] = mark;
  c[ImGuiCol_TableHeaderBg] = panel;
  c[ImGuiCol_TableBorderStrong] = line;
  c[ImGuiCol_TableBorderLight] = raised;
}

}  // namespace kvmux::theme
