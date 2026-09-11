#pragma once

#include "control/control_sink.hpp"
#include "render/video_renderer.hpp"
#include "video/capture/capture_source.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace kvmux {

inline constexpr int kConfigSchemaVersion = 1;

struct WindowConfig {
    int x{100};
    int y{100};
    int width{1280};
    int height{720};
    bool maximized{};
};

struct Config {
    std::string capture_stable_id;
    CaptureMode capture_mode;
    std::string serial_port;
    int serial_baud_rate{9600};
    std::uint8_t serial_address{};
    MouseMode mouse_mode{MouseMode::absolute};
#if defined(__APPLE__)
    std::uint16_t host_scancode{231};  // SDL_SCANCODE_RGUI
#else
    std::uint16_t host_scancode{228};  // SDL_SCANCODE_RCTRL
#endif
    double sensitivity{1.0};
    bool vsync{};
    ColorOverride color_override{ColorOverride::automatic};
    WindowConfig window;
};

// The caller supplies the complete config file path, normally below the
// directory returned by SDL_GetPrefPath. Missing files return defaults.
[[nodiscard]] Config load_config(const std::filesystem::path& path);
void save_config(const std::filesystem::path& path, const Config& config);

}  // namespace kvmux
