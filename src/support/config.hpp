#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "control/control_sink.hpp"
#include "input/input_router.hpp"
#include "render/video_renderer.hpp"
#include "video/capture/capture_source.hpp"
#include "video/codec/video_codec.hpp"

namespace kvmux {

inline constexpr int kConfigSchemaVersion = 1;

struct WindowConfig {
  int x{100};
  int y{100};
  int width{1280};
  int height{720};
  bool maximized{};
};

enum class VideoSource { local, relay };
enum class ControlTarget { local_serial, relay, none };

struct Config {
  std::string capture_stable_id;
  CaptureMode capture_mode;
  std::string serial_port;
  int serial_baud_rate{9600};
  std::uint8_t serial_address{};
  MouseMode mouse_mode{MouseMode::absolute};
  TargetAspect target_aspect{TargetAspect::full_frame};
  VideoSource video_source{VideoSource::local};
  ControlTarget control_target{ControlTarget::local_serial};
  std::string video_host{"127.0.0.1"};
  std::string control_host{"127.0.0.1"};
  int video_control_port{17000};
  int video_port{17001};
  int control_relay_port{17000};
  bool auto_connect_serial{false};
#if defined(__APPLE__)
  std::uint16_t host_scancode{231};  // SDL_SCANCODE_RGUI
#else
  std::uint16_t host_scancode{228};  // SDL_SCANCODE_RCTRL
#endif
  double sensitivity{1.0};
  bool vsync{};
  // Wiggle the target pointer while the session idles so an unattended
  // target does not sleep.
  bool keep_alive{};
  ColorOverride color_override{ColorOverride::automatic};
  CodecBackend decoder_backend{CodecBackend::automatic};
  WindowConfig window;
};

// The caller supplies the complete config file path, normally below the
// directory returned by SDL_GetPrefPath. Missing files return defaults.
[[nodiscard]] Config load_config(const std::filesystem::path& path);
void save_config(const std::filesystem::path& path, const Config& config);

}  // namespace kvmux
