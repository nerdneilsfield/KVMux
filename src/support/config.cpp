#include "support/config.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace kvmux {
namespace {

using json = nlohmann::json;

std::string_view pixel_format_name(PixelFormat format) {
    switch (format) {
    case PixelFormat::yuy2: return "yuy2";
    case PixelFormat::uyvy: return "uyvy";
    case PixelFormat::nv12: return "nv12";
    case PixelFormat::yuv420p: return "yuv420p";
    case PixelFormat::yuv422p: return "yuv422p";
    case PixelFormat::yuv444p: return "yuv444p";
    case PixelFormat::bgra: return "bgra";
    case PixelFormat::rgba: return "rgba";
    case PixelFormat::mjpeg: return "mjpeg";
    case PixelFormat::unknown: return "unknown";
    }
    return "unknown";
}

PixelFormat parse_pixel_format(const std::string& value) {
    static constexpr std::array formats{
        std::pair{"yuy2", PixelFormat::yuy2},
        std::pair{"uyvy", PixelFormat::uyvy},
        std::pair{"nv12", PixelFormat::nv12},
        std::pair{"yuv420p", PixelFormat::yuv420p},
        std::pair{"yuv422p", PixelFormat::yuv422p},
        std::pair{"yuv444p", PixelFormat::yuv444p},
        std::pair{"bgra", PixelFormat::bgra},
        std::pair{"rgba", PixelFormat::rgba},
        std::pair{"mjpeg", PixelFormat::mjpeg},
        std::pair{"unknown", PixelFormat::unknown},
    };
    for (const auto& [name, format] : formats) {
        if (value == name) return format;
    }
    throw std::runtime_error("invalid capture pixel format");
}

std::string_view mouse_mode_name(MouseMode mode) {
    return mode == MouseMode::relative ? "relative" : "absolute";
}

MouseMode parse_mouse_mode(const std::string& value) {
    if (value == "absolute") return MouseMode::absolute;
    if (value == "relative") return MouseMode::relative;
    throw std::runtime_error("invalid mouse mode");
}

std::string_view target_aspect_name(TargetAspect aspect) {
    switch (aspect) {
    case TargetAspect::full_frame: return "full_frame";
    case TargetAspect::ratio_16_9: return "16:9";
    case TargetAspect::ratio_16_10: return "16:10";
    case TargetAspect::ratio_4_3: return "4:3";
    }
    return "full_frame";
}

TargetAspect parse_target_aspect(const std::string& value) {
    if (value == "full_frame") return TargetAspect::full_frame;
    if (value == "16:9") return TargetAspect::ratio_16_9;
    if (value == "16:10") return TargetAspect::ratio_16_10;
    if (value == "4:3") return TargetAspect::ratio_4_3;
    throw std::runtime_error("invalid target aspect");
}

std::string_view color_override_name(ColorOverride value) {
    switch (value) {
    case ColorOverride::automatic: return "automatic";
    case ColorOverride::bt601_limited: return "bt601_limited";
    case ColorOverride::bt601_full: return "bt601_full";
    case ColorOverride::bt709_limited: return "bt709_limited";
    case ColorOverride::bt709_full: return "bt709_full";
    }
    return "automatic";
}

ColorOverride parse_color_override(const std::string& value) {
    if (value == "automatic") return ColorOverride::automatic;
    if (value == "bt601_limited") return ColorOverride::bt601_limited;
    if (value == "bt601_full") return ColorOverride::bt601_full;
    if (value == "bt709_limited") return ColorOverride::bt709_limited;
    if (value == "bt709_full") return ColorOverride::bt709_full;
    throw std::runtime_error("invalid color override");
}

void replace_file(const std::filesystem::path& temporary,
                  const std::filesystem::path& destination) {
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (!error) return;
#if defined(_WIN32)
    if (::MoveFileExW(temporary.c_str(), destination.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
        return;
    }
    error = std::error_code(static_cast<int>(::GetLastError()),
                            std::system_category());
#endif
    throw std::filesystem::filesystem_error(
        "could not replace configuration file", temporary, destination, error);
}

void back_up_broken(const std::filesystem::path& path) noexcept {
    auto broken = path;
    broken += ".broken";
    std::error_code ignored;
    std::filesystem::remove(broken, ignored);
    ignored.clear();
    std::filesystem::rename(path, broken, ignored);
}

Config parse_config(const json& root) {
    Config result;
    const auto& capture = root.at("capture");
    const auto& mode = capture.at("mode");
    const auto& rate = mode.at("frame_rate");
    const auto& serial = root.at("serial");
    const auto& control = root.at("control");
    const auto& window = root.at("window");

    result.capture_stable_id = capture.at("stable_id").get<std::string>();
    result.capture_mode.device_id = result.capture_stable_id;
    result.capture_mode.width = mode.at("width").get<std::uint32_t>();
    result.capture_mode.height = mode.at("height").get<std::uint32_t>();
    result.capture_mode.frame_rate.numerator = rate.at("numerator").get<std::int32_t>();
    result.capture_mode.frame_rate.denominator = rate.at("denominator").get<std::int32_t>();
    result.capture_mode.device_format =
        parse_pixel_format(mode.at("device_format").get<std::string>());
    result.capture_mode.delivered_format =
        parse_pixel_format(mode.at("delivered_format").get<std::string>());
    result.capture_mode.device_format_name =
        mode.at("device_format_name").get<std::string>();

    result.serial_port = serial.at("port").get<std::string>();
    result.serial_baud_rate = serial.at("baud_rate").get<int>();
    const auto address = serial.at("address").get<unsigned int>();
    if (address > 255U) throw std::runtime_error("invalid serial address");
    result.serial_address = static_cast<std::uint8_t>(address);

    result.mouse_mode = parse_mouse_mode(control.at("mouse_mode").get<std::string>());
    result.target_aspect = parse_target_aspect(control.value("target_aspect", std::string("full_frame")));
    result.host_scancode = control.at("host_scancode").get<std::uint16_t>();
    result.sensitivity = control.at("sensitivity").get<double>();
    result.vsync = control.at("vsync").get<bool>();
    result.color_override =
        parse_color_override(control.at("color_override").get<std::string>());

    result.window.x = window.at("x").get<int>();
    result.window.y = window.at("y").get<int>();
    result.window.width = window.at("width").get<int>();
    result.window.height = window.at("height").get<int>();
    result.window.maximized = window.at("maximized").get<bool>();
    if (result.window.width < 960 || result.window.height < 640 ||
        result.window.width > 7680 || result.window.height > 4320 ||
        result.serial_baud_rate <= 0 || !std::isfinite(result.sensitivity) ||
        result.sensitivity <= 0.0) {
        throw std::runtime_error("configuration values are outside supported bounds");
    }
    return result;
}

json serialize_config(const Config& config) {
    return {
        {"schema_version", kConfigSchemaVersion},
        {"capture", {
            {"stable_id", config.capture_stable_id},
            {"mode", {
                {"width", config.capture_mode.width},
                {"height", config.capture_mode.height},
                {"frame_rate", {
                    {"numerator", config.capture_mode.frame_rate.numerator},
                    {"denominator", config.capture_mode.frame_rate.denominator},
                }},
                {"device_format", pixel_format_name(config.capture_mode.device_format)},
                {"delivered_format", pixel_format_name(config.capture_mode.delivered_format)},
                {"device_format_name", config.capture_mode.device_format_name},
            }},
        }},
        {"serial", {
            {"port", config.serial_port},
            {"baud_rate", config.serial_baud_rate},
            {"address", config.serial_address},
        }},
        {"control", {
            {"mouse_mode", mouse_mode_name(config.mouse_mode)},
            {"target_aspect", target_aspect_name(config.target_aspect)},
            {"host_scancode", config.host_scancode},
            {"sensitivity", config.sensitivity},
            {"vsync", config.vsync},
            {"color_override", color_override_name(config.color_override)},
        }},
        {"window", {
            {"x", config.window.x},
            {"y", config.window.y},
            {"width", config.window.width},
            {"height", config.window.height},
            {"maximized", config.window.maximized},
        }},
    };
}

}  // namespace

Config load_config(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) return {};

    try {
        const auto root = json::parse(input);
        if (root.at("schema_version").get<int>() != kConfigSchemaVersion) {
            return {};
        }
        return parse_config(root);
    } catch (const std::exception&) {
        input.close();
        back_up_broken(path);
        return {};
    }
}

void save_config(const std::filesystem::path& path, const Config& config) {
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    auto temporary = path;
    temporary += ".tmp";

    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("could not open temporary configuration file");
        output << serialize_config(config).dump(2) << '\n';
        output.close();
        if (!output) throw std::runtime_error("could not write temporary configuration file");
        replace_file(temporary, path);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

}  // namespace kvmux
