#include <cmath>
#include <cstdint>
#include <stdexcept>

#include "app/ui_theme.hpp"

namespace {
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

double channel(std::uint8_t value) {
  const double encoded = static_cast<double>(value) / 255.0;
  if (encoded <= 0.04045) return encoded / 12.92;
  return std::pow((encoded + 0.055) / 1.055, 2.4);
}

double luminance(kvmux::theme::Rgb color) {
  return 0.2126 * channel(color.r) + 0.7152 * channel(color.g) +
         0.0722 * channel(color.b);
}

double contrast(kvmux::theme::Rgb foreground, kvmux::theme::Rgb background) {
  const double left = luminance(foreground);
  const double right = luminance(background);
  const double lighter = left > right ? left : right;
  const double darker = left > right ? right : left;
  return (lighter + 0.05) / (darker + 0.05);
}

void require_contrast(kvmux::theme::Rgb foreground,
                      kvmux::theme::Rgb background, double minimum,
                      const char* message) {
  require(contrast(foreground, background) >= minimum, message);
}
}  // namespace

int main() {
  namespace theme = kvmux::theme;
  require_contrast(theme::text, theme::bg, 4.5, "text on bg");
  require_contrast(theme::text, theme::surface, 4.5, "text on surface");
  require_contrast(theme::on_accent, theme::accent_fill, 4.5,
                   "button text on accent fill");
  require_contrast(theme::on_accent, theme::accent_fill_hover, 4.5,
                   "button text on accent hover");
  require_contrast(theme::on_accent, theme::accent_fill_active, 4.5,
                   "button text on accent active");
  require_contrast(theme::ok, theme::bg, 4.5, "ok on bg");
  require_contrast(theme::ok, theme::surface, 4.5, "ok on surface");
  require_contrast(theme::warn, theme::bg, 4.5, "warn on bg");
  require_contrast(theme::warn, theme::surface, 4.5, "warn on surface");
  require_contrast(theme::error, theme::bg, 4.5, "error on bg");
  require_contrast(theme::error, theme::surface, 4.5, "error on surface");
  require_contrast(theme::accent, theme::bg, 3.0, "copper on bg");
  require_contrast(theme::accent, theme::surface, 3.0, "copper on surface");
  require(contrast(theme::on_accent, theme::accent) < 4.5,
          "copper is not a text-button fill");
  require(theme::content_scale(2.F, 2.F) == 1.F, "retina content scale is 1");
  require(theme::content_scale(1.5F, 1.F) == 1.5F,
          "pixel-sized window keeps the display scale");
  require(theme::content_scale(1.5F, 1.5F) == 1.F,
          "matching framebuffer scale is not scaled again");
  require(theme::content_scale(0.F, 2.F) == 1.F, "missing display scale");
  return 0;
}
