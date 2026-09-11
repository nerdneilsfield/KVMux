#include "control/control_sink.hpp"
#include "control/serial_worker.hpp"
#include "network/relay_server.hpp"
#include <charconv>
#include <csignal>
#include <thread>
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
           "Serve: --serve --device ID --mode-index N --serial PORT [--baud 57600]\n"
           "       [--bind 0.0.0.0] [--control-port 17000] [--video-port 17001]\n"
           "Native MJPEG only. Unauthenticated LAN TCP: trusted networks only.\n";
}

volatile std::sig_atomic_t interrupted=0;
void interrupt(int) { interrupted=1; }
int serve(int argc,char** argv) {
    kvmux::relay::ServerOptions options;
    std::string device,serial; int mode=-1,baud=57600;
    for(int i=2;i<argc;i+=2) {
        if(i+1>=argc)throw std::runtime_error("Missing option value");
        const std::string_view key=argv[i],value=argv[i+1];
        if(key=="--device")device=value;
        else if(key=="--serial")serial=value;
        else if(key=="--bind")options.bind_address=value;
        else {
            int number{};const auto result=std::from_chars(value.data(),value.data()+value.size(),number);
            if(result.ec!=std::errc{}||result.ptr!=value.data()+value.size()||number<0)throw std::runtime_error("Invalid numeric option");
            if(key=="--mode-index")mode=number;
            else if(key=="--baud"&&number>0)baud=number;
            else if(key=="--control-port"&&number>0&&number<=65535)options.control_port=static_cast<std::uint16_t>(number);
            else if(key=="--video-port"&&number>0&&number<=65535)options.video_port=static_cast<std::uint16_t>(number);
            else throw std::runtime_error("Unknown or invalid serve option");
        }
    }
    if(device.empty()||serial.empty()||mode<0)throw std::runtime_error("--serve requires --device, --mode-index and --serial");
    auto capture=kvmux::create_platform_capture_source();auto modes=capture->enumerate_modes(device);
    if(static_cast<std::size_t>(mode)>=modes.size())throw std::runtime_error("Capture mode index out of range");
    const auto& selected=modes[static_cast<std::size_t>(mode)];
    if(selected.device_format!=kvmux::PixelFormat::mjpeg||selected.delivered_format!=kvmux::PixelFormat::mjpeg)
        throw std::runtime_error("LAN relay requires native MJPEG; raw-only capture modes are unsupported");
    capture->start(selected);
    if(capture->snapshot().state==kvmux::CaptureState::fault)throw std::runtime_error(capture->snapshot().error);
    kvmux::Ch9329ControlSink sink;sink.connect(serial,baud);
    kvmux::relay::RelayServer server(*capture,sink);std::string error;
    if(!server.start(options,error))throw std::runtime_error(error);
    std::signal(SIGINT,interrupt);std::signal(SIGTERM,interrupt);
    std::cout<<"Listening on "<<options.bind_address<<":"<<server.control_port()<<" (control), "<<server.video_port()<<" (video). Trusted LAN only.\n";
    while(!interrupted)std::this_thread::sleep_for(std::chrono::milliseconds(50));
    server.stop();capture->stop();sink.disconnect();return 0;
}
int run(int argc, char** argv) {
    if(argc>=2&&std::string_view(argv[1])=="--serve")return serve(argc,argv);
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
