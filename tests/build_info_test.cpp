#include "support/build_info.hpp"
#include "support/config.hpp"
#include "support/diagnostics.hpp"
#include <spdlog/spdlog.h>

#include <iostream>

int main() {
    if (kvmux::Config{}.serial_baud_rate != 9600) return 1;
    kvmux::configure_console_logging(false);
    if (spdlog::should_log(spdlog::level::debug) || spdlog::should_log(spdlog::level::info) ||
        !spdlog::should_log(spdlog::level::warn)) return 1;
    kvmux::configure_console_logging(true);
    if (!spdlog::should_log(spdlog::level::debug)) return 1;
    spdlog::debug("console logging smoke test");
    if (kvmux::application_name() != "KVMux") {
        std::cerr << "unexpected application name\n";
        return 1;
    }
    if (kvmux::application_version().empty()) {
        std::cerr << "application version must not be empty\n";
        return 1;
    }
    return 0;
}
