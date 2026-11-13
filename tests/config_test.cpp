#include "support/config.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
}  // namespace

int main() {
  using namespace kvmux;
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("kvmux-config-test-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto path = directory / "config.json";
  try {
    require(load_config(path).target_aspect == TargetAspect::full_frame,
            "missing config defaults to full frame");
    Config config;
    config.serial_port = "test-port";
    for (const auto aspect :
         {TargetAspect::full_frame, TargetAspect::ratio_16_9,
          TargetAspect::ratio_16_10, TargetAspect::ratio_4_3}) {
      config.target_aspect = aspect;
      save_config(path, config);
      const auto loaded = load_config(path);
      require(
          loaded.target_aspect == aspect && loaded.serial_port == "test-port",
          "target aspect round trip preserves config");
    }
    require(load_config(directory / "missing.json").decoder_backend ==
                CodecBackend::automatic,
            "missing config defaults to automatic decoding");
    require(!config.keep_alive, "keep-alive defaults to disabled");
    config.keep_alive = true;
    save_config(path, config);
    require(load_config(path).keep_alive &&
                load_config(path).serial_port == "test-port",
            "keep-alive round trip preserves config");
    config.keep_alive = false;
    save_config(path, config);
    for (const auto backend :
         {CodecBackend::automatic, CodecBackend::videotoolbox,
          CodecBackend::ffmpeg_software}) {
      config.decoder_backend = backend;
      save_config(path, config);
      const auto loaded = load_config(path);
      require(loaded.decoder_backend == backend &&
                  loaded.serial_port == "test-port",
              "decoder backend round trip preserves config");
    }
    nlohmann::json root;
    {
      std::ifstream input(path);
      input >> root;
    }
    root["control"].erase("target_aspect");
    root["control"].erase("keep_alive");
    {
      std::ofstream output(path);
      output << root;
    }
    auto loaded = load_config(path);
    require(loaded.target_aspect == TargetAspect::full_frame &&
                !loaded.keep_alive && loaded.serial_port == "test-port",
            "existing schema without optional field retains settings and "
            "defaults to full frame");
    for (const auto& invalid : {nlohmann::json("21:9"), nlohmann::json(42)}) {
      root["control"]["target_aspect"] = invalid;
      {
        std::ofstream output(path);
        output << root;
      }
      loaded = load_config(path);
      require(loaded.target_aspect == TargetAspect::full_frame &&
                  loaded.serial_port.empty() &&
                  std::filesystem::exists(path.string() + ".broken"),
              "invalid aspect follows existing broken config policy");
    }
    root["control"].erase("target_aspect");
    root.erase("decoder_backend");
    {
      std::ofstream output(path);
      output << root;
    }
    loaded = load_config(path);
    require(
        loaded.decoder_backend == CodecBackend::automatic &&
            loaded.serial_port == "test-port",
        "absent decoder setting defaults to automatic and preserves settings");
    for (const auto& invalid :
         {nlohmann::json("jetson_gstreamer"), nlohmann::json("unknown"),
          nlohmann::json(42)}) {
      root["decoder_backend"] = invalid;
      {
        std::ofstream output(path);
        output << root;
      }
      loaded = load_config(path);
      require(loaded.decoder_backend == CodecBackend::automatic &&
                  loaded.serial_port.empty() &&
                  std::filesystem::exists(path.string() + ".broken"),
              "invalid decoder follows existing broken config policy");
    }
    config = Config{};
    config.serial_port = "test-port";
    config.serial_baud_rate = 115200;
    config.host_scancode = 231;
    config.capture_stable_id = "cam-1";
    config.capture_mode.device_id = "cam-1";
    config.capture_mode.width = 1920;
    config.capture_mode.height = 1080;
    config.capture_mode.frame_rate = {30, 1};
    config.capture_mode.device_format = PixelFormat::yuy2;
    config.capture_mode.delivered_format = PixelFormat::bgra;
    config.capture_mode.device_format_name = "YUY2";
    config.video_source = VideoSource::relay;
    config.control_target = ControlTarget::none;
    config.video_host = "10.0.0.8";
    config.control_host = "10.0.0.9";
    config.video_control_port = 18000;
    config.video_port = 18001;
    config.control_relay_port = 18002;
    config.auto_connect_serial = true;
    save_config(path, config);
    loaded = load_config(path);
    require(loaded.video_source == VideoSource::relay &&
                loaded.control_target == ControlTarget::none &&
                loaded.video_host == "10.0.0.8" &&
                loaded.control_host == "10.0.0.9" &&
                loaded.video_control_port == 18000 &&
                loaded.video_port == 18001 &&
                loaded.control_relay_port == 18002 &&
                loaded.auto_connect_serial && loaded.host_scancode == 231 &&
                loaded.capture_stable_id == "cam-1" &&
                loaded.capture_mode.device_id == "cam-1" &&
                loaded.capture_mode.width == 1920 &&
                loaded.capture_mode.height == 1080 &&
                loaded.capture_mode.frame_rate.numerator == 30 &&
                loaded.capture_mode.device_format == PixelFormat::yuy2 &&
                loaded.capture_mode.delivered_format == PixelFormat::bgra &&
                loaded.serial_port == "test-port" &&
                loaded.serial_baud_rate == 115200,
            "connection settings round trip preserves config");
    {
      std::ifstream input(path);
      input >> root;
    }
    root.erase("connection");
    {
      std::ofstream output(path);
      output << root;
    }
    loaded = load_config(path);
    require(
        loaded.video_source == VideoSource::local &&
            loaded.control_target == ControlTarget::local_serial &&
            loaded.video_host == "127.0.0.1" &&
            loaded.control_host == "127.0.0.1" &&
            loaded.video_control_port == 17000 && loaded.video_port == 17001 &&
            loaded.control_relay_port == 17000 && !loaded.auto_connect_serial &&
            loaded.serial_port == "test-port" && loaded.host_scancode == 231,
        "missing connection object keeps settings and uses defaults");
    root["connection"] = nlohmann::json::object();
    {
      std::ofstream output(path);
      output << root;
    }
    loaded = load_config(path);
    require(loaded.video_source == VideoSource::local &&
                loaded.control_target == ControlTarget::local_serial &&
                !loaded.auto_connect_serial &&
                loaded.serial_port == "test-port",
            "empty connection object uses defaults and keeps settings");
    root["connection"] = {{"video_source", "satellite"}};
    {
      std::ofstream output(path);
      output << root;
    }
    loaded = load_config(path);
    require(loaded.serial_port.empty() &&
                std::filesystem::exists(path.string() + ".broken"),
            "invalid video source follows existing broken config policy");
    root["schema_version"] = kConfigSchemaVersion + 1;
    {
      std::ofstream output(path);
      output << root;
    }
    require(
        load_config(path).serial_port.empty() && std::filesystem::exists(path),
        "unknown schema still returns defaults without migration");
  } catch (...) {
    std::filesystem::remove_all(directory);
    throw;
  }
  std::filesystem::remove_all(directory);
}
