#include "support/config.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}

int main() {
    using namespace kvmux;
    const auto directory = std::filesystem::temp_directory_path() /
        ("kvmux-config-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto path = directory / "config.json";
    try {
        require(load_config(path).target_aspect == TargetAspect::full_frame, "missing config defaults to full frame");
        Config config;
        config.serial_port = "test-port";
        for (const auto aspect : {TargetAspect::full_frame, TargetAspect::ratio_16_9,
                                  TargetAspect::ratio_16_10, TargetAspect::ratio_4_3}) {
            config.target_aspect = aspect;
            save_config(path, config);
            const auto loaded = load_config(path);
            require(loaded.target_aspect == aspect && loaded.serial_port == "test-port",
                    "target aspect round trip preserves config");
        }
        nlohmann::json root;
        { std::ifstream input(path); input >> root; }
        root["control"].erase("target_aspect");
        { std::ofstream output(path); output << root; }
        auto loaded = load_config(path);
        require(loaded.target_aspect == TargetAspect::full_frame && loaded.serial_port == "test-port",
                "existing schema without optional field retains settings and defaults to full frame");
        for (const auto& invalid : {nlohmann::json("21:9"), nlohmann::json(42)}) {
            root["control"]["target_aspect"] = invalid;
            { std::ofstream output(path); output << root; }
            loaded = load_config(path);
            require(loaded.target_aspect == TargetAspect::full_frame && loaded.serial_port.empty() &&
                        std::filesystem::exists(path.string() + ".broken"),
                    "invalid aspect follows existing broken config policy");
        }
        root["schema_version"] = kConfigSchemaVersion + 1;
        { std::ofstream output(path); output << root; }
        require(load_config(path).serial_port.empty() && std::filesystem::exists(path),
                "unknown schema still returns defaults without migration");
    } catch (...) {
        std::filesystem::remove_all(directory);
        throw;
    }
    std::filesystem::remove_all(directory);
}
