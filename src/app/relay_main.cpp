#include "control/control_sink.hpp"
#include "video/capture/capture_source.hpp"

#include <algorithm>
#include <exception>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void print_help(std::ostream& out) {
    out << "KVMux headless relay device tools\n"
           "Usage: kvmux-relay COMMAND\n\n"
           "  --help                 Show this help\n"
           "  --list-devices         List capture device stable IDs and names\n"
           "  --list-modes DEVICE    List native modes for a capture stable ID\n"
           "  --list-serial          List serial port names and descriptions\n\n"
           "Quote DEVICE if it contains spaces. Mode indices are zero-based.\n"
           "This build provides device discovery; TCP serving is not yet implemented.\n";
}

int run(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        print_help(std::cout);
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--list-serial") {
        const auto ports = kvmux::enumerate_serial_ports();
        std::cout << "PORT\tDESCRIPTION\n";
        for (const auto& port : ports) {
            std::cout << std::quoted(port.name) << '\t' << std::quoted(port.description) << '\n';
        }
        if (ports.empty()) std::cout << "No serial ports found.\n";
        return 0;
    }
    const bool list_devices = argc == 2 && std::string_view(argv[1]) == "--list-devices";
    const bool list_modes = argc == 3 && std::string_view(argv[1]) == "--list-modes";
    if (!list_devices && !list_modes) {
        std::cerr << "Expected one command with its required arguments.\n";
        print_help(std::cerr);
        return 2;
    }

    auto capture = kvmux::create_platform_capture_source();
    const auto devices = capture->enumerate_devices();
    if (list_devices) {
        std::cout << "DEVICE\tNAME\n";
        for (const auto& device : devices) {
            std::cout << std::quoted(device.stable_id) << '\t'
                      << std::quoted(device.display_name) << '\n';
        }
        if (devices.empty()) std::cout << "No capture devices found.\n";
        return 0;
    }

    const std::string device_id = argv[2];
    if (std::none_of(devices.begin(), devices.end(), [&](const auto& device) {
            return device.stable_id == device_id;
        })) {
        std::cerr << "Capture device not found: " << std::quoted(device_id) << '\n';
        return 1;
    }
    const auto modes = capture->enumerate_modes(device_id);
    std::cout << "MODE\tSIZE\tFPS\tNATIVE FORMAT\n";
    for (std::size_t index = 0; index < modes.size(); ++index) {
        const auto& mode = modes[index];
        std::cout << index << '\t' << mode.width << 'x' << mode.height << '\t'
                  << mode.frame_rate.numerator << '/' << mode.frame_rate.denominator << '\t'
                  << std::quoted(mode.device_format_name) << '\n';
    }
    if (modes.empty()) {
        std::cerr << "No capture modes found for " << std::quoted(device_id) << '\n';
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "kvmux-relay: " << error.what() << '\n';
        return 1;
    }
}
