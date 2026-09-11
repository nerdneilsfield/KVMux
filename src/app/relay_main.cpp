#include "control/control_sink.hpp"
#include "support/diagnostics.hpp"
#include <spdlog/spdlog.h>
#include <vector>
#include "control/serial_worker.hpp"
#include "network/relay_server.hpp"
#include "network/relay_selection.hpp"
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
           "Usage: kvmux-relay [--debug] COMMAND\n\n"
           "  --debug                Write diagnostic logs to stderr (time, thread, level)\n"
           "  --help                 Show this help\n"
           "  --list-devices         List capture device stable IDs and names\n"
           "  --list-modes DEVICE    List native modes for a capture stable ID\n"
           "  --list-serial          List serial port names and descriptions\n\n"
           "Quote DEVICE if it contains spaces. Mode indices are zero-based.\n"
           "Serve: --serve [--device ID] [--mode-index N] [--serial PORT] [--baud 9600]\n"
           "       [--bind 0.0.0.0] [--control-port 17000] [--video-port 17001]\n"
           "       [--codec mjpeg|hevc] [--encoder auto|jetson|software] [--bitrate 8000000]\n"
           "Omitted device: require one capture device. Omitted serial: require one USB\n"
           "CH340/CH341/CH343 VID/PID match (not proof of CH9329 identity).\n"
           "Auto MJPEG: 1080p60, 720p60, 1080p30, 720p30 (including 59.94/29.97),\n"
           "then descending pixel area, width, height and fps; ties use first index.\n"
           "HEVC requires native and delivered raw video. Auto tries hardware, then CPU.\n"
           "Explicit choices never fall back. Bitrate is in bits/s.\n"
           "Unauthenticated LAN TCP: trusted networks only.\n";
}

volatile std::sig_atomic_t interrupted=0;
void interrupt(int) { interrupted=1; }
int serve(int argc,char** argv) {
    kvmux::relay::ServerOptions options;
    std::optional<std::string> device_request,serial_request;
    std::optional<std::size_t> mode_request; int baud=9600;
    for(int i=2;i<argc;i+=2) {
        if(i+1>=argc)throw std::runtime_error("Missing option value");
        const std::string_view key=argv[i],value=argv[i+1];
        if(key=="--device")device_request=value;
        else if(key=="--serial")serial_request=value;
        else if(key=="--bind")options.bind_address=value;
        else if(key=="--codec") {
            if(value=="mjpeg")options.codec=kvmux::VideoCodec::mjpeg;
            else if(value=="hevc")options.codec=kvmux::VideoCodec::hevc;
            else throw std::runtime_error("--codec must be mjpeg or hevc");
        } else if(key=="--encoder") {
            if(value=="auto")options.encoder_backend=kvmux::CodecBackend::automatic;
            else if(value=="jetson")options.encoder_backend=kvmux::CodecBackend::jetson_gstreamer;
            else if(value=="software")options.encoder_backend=kvmux::CodecBackend::ffmpeg_software;
            else throw std::runtime_error("--encoder must be auto, jetson, or software");
        } else {
            int number{};const auto result=std::from_chars(value.data(),value.data()+value.size(),number);
            if(result.ec!=std::errc{}||result.ptr!=value.data()+value.size()||number<0)throw std::runtime_error("Invalid numeric option");
            if(key=="--mode-index")mode_request=static_cast<std::size_t>(number);
            else if(key=="--bitrate"&&number>0&&number<=100000000)options.bitrate=static_cast<std::uint32_t>(number);
            else if(key=="--baud"&&number>0)baud=number;
            else if(key=="--control-port"&&number>0&&number<=65535)options.control_port=static_cast<std::uint16_t>(number);
            else if(key=="--video-port"&&number>0&&number<=65535)options.video_port=static_cast<std::uint16_t>(number);
            else throw std::runtime_error("Unknown or invalid serve option");
        }
    }
    auto capture=kvmux::create_platform_capture_source();
    const auto device=kvmux::relay::select_device(capture->enumerate_devices(),device_request);
    const auto modes=capture->enumerate_modes(device);
    const auto mode=kvmux::relay::select_mode(modes,mode_request,options.codec);
    const auto serial=kvmux::relay::select_serial(
        serial_request ? std::vector<kvmux::SerialPortInfo>{} : kvmux::enumerate_serial_ports(),serial_request);
    const auto& selected=modes[mode];
    std::cout << "Selected device=" << std::quoted(device) << " mode-index=" << mode
              << " size=" << selected.width << 'x' << selected.height
              << " fps=" << selected.frame_rate.numerator << '/' << selected.frame_rate.denominator
              << " native-format=" << selected.device_format_name
              << " delivered-format=" << static_cast<int>(selected.delivered_format)
              << " codec=" << (options.codec==kvmux::VideoCodec::hevc ? "hevc" : "mjpeg")
              << " serial=" << std::quoted(serial) << " baud=" << baud << '\n';
    if (!serial_request)
        std::cout << "USB adapter VID/PID match only; CH9329 handshake not yet verified.\n";
    std::cout.flush();
    capture->start(selected);
    if(capture->snapshot().state==kvmux::CaptureState::fault)throw std::runtime_error(capture->snapshot().error);
    kvmux::Ch9329ControlSink sink;sink.connect(serial,baud);
    kvmux::relay::RelayServer server(*capture,sink);std::string error;
    if(!server.start(options,error))throw std::runtime_error(error);
    std::signal(SIGINT,interrupt);std::signal(SIGTERM,interrupt);
    std::cout<<"Listening on "<<options.bind_address<<":"<<server.control_port()<<" (control), "<<server.video_port()<<" (video). Trusted LAN only.\n";
    std::string last_status;
    while(!interrupted) {
        if (spdlog::should_log(spdlog::level::debug)) {
            const auto capture_status = capture->snapshot();
            const auto control_status = sink.snapshot();
            const auto status = "capture-state=" + std::to_string(static_cast<int>(capture_status.state)) +
                " error=" + capture_status.error + " control-state=" +
                std::to_string(static_cast<int>(control_status.state)) + " error=" + control_status.error;
            if (status != last_status) { spdlog::debug("Relay {}", status); last_status = status; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    spdlog::debug("Relay stopping: termination signal");
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
        bool debug = false;
        std::vector<char*> arguments{argv[0]};
        for (int i = 1; i < argc; ++i) {
            if (std::string_view(argv[i]) == "--debug") debug = true;
            else arguments.push_back(argv[i]);
        }
        kvmux::configure_console_logging(debug);
        return run(static_cast<int>(arguments.size()), arguments.data());
    } catch (const std::exception& error) {
        std::cerr << "kvmux-relay: " << error.what() << '\n';
        return 1;
    }
}
