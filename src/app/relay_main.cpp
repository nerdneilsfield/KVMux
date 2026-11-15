#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "app/probe.hpp"
#include "control/control_sink.hpp"
#include "control/serial_worker.hpp"
#include "input/windows_hex_text.hpp"
#include "network/relay_client.hpp"
#include "network/relay_selection.hpp"
#include "network/relay_server.hpp"
#include "support/diagnostics.hpp"
#include "video/capture/capture_source.hpp"

namespace {

void print_help(std::ostream& out) {
  out << "KVMux headless relay device tools\n"
         "Usage: kvmux-relay [--debug] [--log-file PATH] COMMAND\n\n"
         "  --debug                Write diagnostic logs to stderr (time, "
         "thread, level)\n"
         "  --log-file PATH        Append the same log to PATH (10 MiB, 3 "
         "rotations)\n"
         "  --help                 Show this help\n"
         "  --list-devices         List capture device stable IDs and names\n"
         "  --list-modes DEVICE    List native modes for a capture stable ID\n"
         "  --list-serial          List serial port names and descriptions\n"
         "  --probe-control        Open CH9329, optionally send input, then "
         "release\n"
         "  --probe-video HOST     Count frames from a video relay\n\n"
         "Quote DEVICE if it contains spaces. Mode indices are zero-based.\n"
         "Probe control: --probe-control (--serial PORT [--baud 9600] |\n"
         "       --relay HOST [--control-port 17000]) [--keys] [--mouse]\n"
         "       [--paste] [--hex-probe LIST] [--hex-gap-ms 300]\n"
         "       [--timeout-ms 5000]\n"
         "--hex-probe LIST is up to 16 comma-separated hex code points\n"
         "(example 4e2d,7684). It requires --serial, reads NumLock from\n"
         "GET_INFO, and sends each point as a Windows hex Alt sequence.\n"
         "--hex-gap-ms pauses between points (0..60000) and needs "
         "--hex-probe.\n"
         "Probe video: --probe-video HOST [--control-port 17000]\n"
         "       [--video-port 17001] [--seconds 5]\n"
         "Serve: --serve [--device ID] [--mode-index N] [--serial PORT] "
         "[--baud 9600]\n"
         "       [--no-capture | --no-serial]\n"
         "       [--bind 0.0.0.0] [--control-port 17000] [--video-port 17001]\n"
         "       [--codec mjpeg | --encoding "
         "h264-quality|h264-size|h265-quality|h265-size]\n"
         "       [--encoder auto|jetson|software] [--bitrate 40000000]\n"
         "       [--transport-rate 12000000] (UDP envelope+payload+FEC "
         "bytes/s; excludes IP/UDP headers)\n"
         "Omitted device: require one capture device. Omitted serial: require "
         "one USB\n"
         "CH340/CH341/CH343 VID/PID match (not proof of CH9329 identity).\n"
         "--no-capture does not open a capture device (control only). "
         "--no-serial does not open a serial port (video only). "
         "The two flags cannot be combined.\n"
         "Auto MJPEG: 1080p60, 720p60, 1080p30, 720p30 (including "
         "59.94/29.97),\n"
         "then descending pixel area, width, height and fps; ties use first "
         "index.\n"
         "The four --encoding choices require raw video. Auto tries Jetson, "
         "then software.\n"
         "Use --codec mjpeg for MJPEG pass-through; do not combine it with "
         "--encoding.\n"
         "Explicit choices never fall back. --bitrate is the quality target in "
         "bits/s; size uses half (minimum 250000).\n"
         "Diagnostics show Mbit/s and MB/s (decimal).\n"
         "Transport rate: 1..1000000000 bytes/s. Encoder bitrate: bits/s (not "
         "transport cap).\n"
         "Unauthenticated LAN UDP v3 / KCP control: trusted networks only.\n";
}

volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) { interrupted = 1; }
int serve(int argc, char** argv) {
  kvmux::relay::ServerOptions options;
  std::optional<std::string> device_request, serial_request;
  std::optional<std::size_t> mode_request;
  bool codec_seen = false, encoding_seen = false;
  bool no_capture = false, no_serial = false;
  int baud = 9600;
  for (int i = 2; i < argc; ++i) {
    const std::string_view key = argv[i];
    if (key == "--no-capture") {
      no_capture = true;
      continue;
    }
    if (key == "--no-serial") {
      no_serial = true;
      continue;
    }
    if (i + 1 >= argc) throw std::runtime_error("Missing option value");
    const std::string_view value = argv[++i];
    if (key == "--device")
      device_request = value;
    else if (key == "--serial")
      serial_request = value;
    else if (key == "--bind")
      options.bind_address = value;
    else if (key == "--codec") {
      if (value != "mjpeg")
        throw std::runtime_error(
            "--codec only accepts mjpeg; use --encoding for raw video");
      if (encoding_seen)
        throw std::runtime_error("--codec and --encoding cannot be combined");
      codec_seen = true;
      options.codec = kvmux::VideoCodec::mjpeg;
    } else if (key == "--encoding") {
      if (codec_seen)
        throw std::runtime_error("--codec and --encoding cannot be combined");
      encoding_seen = true;
      if (value == "h264-quality") {
        options.codec = kvmux::VideoCodec::h264;
        options.priority = kvmux::EncodingPriority::quality;
      } else if (value == "h264-size") {
        options.codec = kvmux::VideoCodec::h264;
        options.priority = kvmux::EncodingPriority::size;
      } else if (value == "h265-quality") {
        options.codec = kvmux::VideoCodec::hevc;
        options.priority = kvmux::EncodingPriority::quality;
      } else if (value == "h265-size") {
        options.codec = kvmux::VideoCodec::hevc;
        options.priority = kvmux::EncodingPriority::size;
      } else
        throw std::runtime_error(
            "--encoding must be h264-quality, h264-size, h265-quality, or "
            "h265-size");
    } else if (key == "--encoder") {
      if (value == "auto")
        options.encoder_backend = kvmux::CodecBackend::automatic;
      else if (value == "jetson")
        options.encoder_backend = kvmux::CodecBackend::jetson_gstreamer;
      else if (value == "software")
        options.encoder_backend = kvmux::CodecBackend::ffmpeg_software;
      else
        throw std::runtime_error("--encoder must be auto, jetson, or software");
    } else if (key == "--transport-rate") {
      std::uint64_t number{};
      const auto result =
          std::from_chars(value.data(), value.data() + value.size(), number);
      if (result.ec != std::errc{} ||
          result.ptr != value.data() + value.size() || !number ||
          number > 1'000'000'000)
        throw std::runtime_error(
            "--transport-rate must be 1..1000000000 bytes/s");
      options.transport_bytes_per_second = number;
    } else {
      int number{};
      const auto result =
          std::from_chars(value.data(), value.data() + value.size(), number);
      if (result.ec != std::errc{} ||
          result.ptr != value.data() + value.size() || number < 0)
        throw std::runtime_error("Invalid numeric option");
      if (key == "--mode-index")
        mode_request = static_cast<std::size_t>(number);
      else if (key == "--bitrate" && number > 0 && number <= 100000000)
        options.bitrate = static_cast<std::uint32_t>(number);
      else if (key == "--baud" && number > 0)
        baud = number;
      else if (key == "--control-port" && number > 0 && number <= 65535)
        options.control_port = static_cast<std::uint16_t>(number);
      else if (key == "--video-port" && number > 0 && number <= 65535)
        options.video_port = static_cast<std::uint16_t>(number);
      else
        throw std::runtime_error("Unknown or invalid serve option");
    }
  }
  if (no_capture && no_serial)
    throw std::runtime_error("--no-capture and --no-serial cannot be combined");
  std::unique_ptr<kvmux::CaptureSource> capture;
  std::optional<kvmux::CaptureMode> selected;
  std::size_t mode = 0;
  std::string device;
  if (!no_capture) {
    capture = kvmux::create_platform_capture_source();
    device = kvmux::relay::select_device(capture->enumerate_devices(),
                                         device_request);
    const auto modes = capture->enumerate_modes(device);
    mode = kvmux::relay::select_mode(modes, mode_request, options.codec);
    selected = modes[mode];
  }
  std::string serial;
  if (!no_serial)
    serial = kvmux::relay::select_serial(
        serial_request ? std::vector<kvmux::SerialPortInfo>{}
                       : kvmux::enumerate_serial_ports(),
        serial_request);
  if (selected) {
    const auto& mode_info = *selected;
    std::cout << "Selected device=" << std::quoted(device)
              << " mode-index=" << mode << " size=" << mode_info.width << 'x'
              << mode_info.height << " fps=" << mode_info.frame_rate.numerator
              << '/' << mode_info.frame_rate.denominator
              << " native-format=" << mode_info.device_format_name
              << " delivered-format="
              << static_cast<int>(mode_info.delivered_format) << " codec="
              << (options.codec == kvmux::VideoCodec::hevc   ? "h265"
                  : options.codec == kvmux::VideoCodec::h264 ? "h264"
                                                             : "mjpeg")
              << " priority="
              << (options.codec == kvmux::VideoCodec::mjpeg ? "pass-through"
                  : options.priority == kvmux::EncodingPriority::quality
                      ? "quality"
                      : "size")
              << " selected-bitrate=" << options.bitrate / 1'000'000.0
              << " Mbit/s (" << options.bitrate / 8'000'000.0 << " MB/s)";
    const auto effective_bitrate =
        options.codec == kvmux::VideoCodec::mjpeg
            ? 0U
            : kvmux::encoding_profile(options.priority, options.bitrate)
                  .bitrate;
    std::cout << " effective-bitrate=" << effective_bitrate / 1'000'000.0
              << " Mbit/s (" << effective_bitrate / 8'000'000.0 << " MB/s)"
              << " UDP-media-cap="
              << options.transport_bytes_per_second / 1'000'000.0 << " MB/s";
  }
  if (!no_serial) {
    std::cout << (selected ? " serial=" : "Selected serial=")
              << std::quoted(serial) << " baud=" << baud;
  }
  std::cout << '\n';
  if (!no_serial && !serial_request)
    std::cout << "USB adapter VID/PID match only; CH9329 handshake not yet "
                 "verified.\n";
  std::cout.flush();
  if (capture) {
    capture->start(*selected);
    if (capture->snapshot().state == kvmux::CaptureState::fault)
      throw std::runtime_error(capture->snapshot().error);
  }
  std::unique_ptr<kvmux::Ch9329ControlSink> sink;
  if (!no_serial) {
    sink = std::make_unique<kvmux::Ch9329ControlSink>();
    sink->connect(serial, baud);
  }
  kvmux::relay::RelayServer server(capture.get(), sink.get());
  std::string error;
  if (!server.start(options, error)) throw std::runtime_error(error);
  std::signal(SIGINT, interrupt);
  std::signal(SIGTERM, interrupt);
  std::cout << "Listening on " << options.bind_address << ":"
            << server.control_port() << " (control)";
  if (server.video_port())
    std::cout << ", " << server.video_port() << " (video)";
  std::cout << ", UDP v3 / KCP. Cap includes envelope+payload+FEC, "
               "excludes IP/UDP headers. Trusted LAN only.\n";
  std::string last_status;
  auto media_log_at = std::chrono::steady_clock::time_point{};
  while (!interrupted) {
    if (spdlog::should_log(spdlog::level::debug)) {
      std::string status;
      if (capture) {
        const auto capture_status = capture->snapshot();
        status += "capture-state=" +
                  std::to_string(static_cast<int>(capture_status.state)) +
                  " error=" + capture_status.error;
      }
      if (sink) {
        const auto control_status = sink->snapshot();
        if (!status.empty()) status += ' ';
        status += "control-state=" +
                  std::to_string(static_cast<int>(control_status.state)) +
                  " error=" + control_status.error;
      }
      if (status != last_status) {
        spdlog::debug("Relay {}", status);
        last_status = status;
      }
    }
    const auto now = std::chrono::steady_clock::now();
    if (capture && now - media_log_at >= std::chrono::seconds(2)) {
      const auto media = server.snapshot();
      spdlog::info(
          "UDP media cap={} bytes/s source-interval={} us feedback={} "
          "completed={} lost={} age={} capacity={} gap={} paced-bytes={} "
          "reason={}",
          media.transport_bytes_per_second, media.admission_interval.count(),
          media.feedback_samples, media.feedback.received_frames,
          media.feedback.lost_frames, media.feedback.age_losses,
          media.feedback.capacity_losses, media.feedback.gap_losses,
          media.pacer.sent_bytes,
          kvmux::relay::media_reason_name(media.last_reason));
      if (media.last_reason ==
          kvmux::relay::MediaReason::frame_exceeds_rate_budget)
        spdlog::warn(
            "Video blocked: frame_exceeds_rate_budget. Lower --bitrate "
            "(H.264/H.265) or raise --transport-rate, then start a new "
            "session.");
      media_log_at = now;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  spdlog::debug("Relay stopping: termination signal");
  server.stop();
  if (capture) capture->stop();
  if (sink) sink->disconnect();
  return 0;
}

bool parse_bounded_int(std::string_view text, int min, int max, int& output) {
  int number = 0;
  const auto* begin = text.data();
  const auto* end = begin + text.size();
  const auto result = std::from_chars(begin, end, number);
  if (result.ec != std::errc{} || result.ptr != end || number < min ||
      number > max)
    return false;
  output = number;
  return true;
}

constexpr int kMaxHexProbePoints = 16;

bool parse_hex_probe_list(std::string_view text,
                          std::vector<std::uint32_t>& points) {
  if (text.empty()) return false;
  points.clear();
  std::size_t begin = 0;
  while (begin <= text.size()) {
    const auto comma = text.find(',', begin);
    const auto token = text.substr(begin, comma == std::string_view::npos
                                              ? std::string_view::npos
                                              : comma - begin);
    if (token.empty() || points.size() == kMaxHexProbePoints) return false;
    std::uint32_t scalar = 0;
    const auto* first = token.data();
    const auto* last = first + token.size();
    const auto parsed = std::from_chars(first, last, scalar, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != last) return false;
    if (kvmux::windows_hex_scalar_error(scalar) != kvmux::WindowsHexError::none)
      return false;
    points.push_back(scalar);
    if (comma == std::string_view::npos) return true;
    begin = comma + 1;
  }
  return false;
}

bool next_value(int argc, char** argv, int& index, std::string_view& value) {
  if (index + 1 >= argc ||
      std::string_view(argv[index + 1]).starts_with("--")) {
    std::cerr << "Missing option value\n";
    return false;
  }
  value = argv[++index];
  return true;
}

int probe_control_command(int argc, char** argv) {
  std::optional<std::string> serial;
  std::optional<std::string> relay;
  int baud = 9600;
  int control_port = 17000;
  int timeout_ms = 5000;
  bool keys = false;
  bool mouse = false;
  bool paste = false;
  bool baud_seen = false;
  bool port_seen = false;
  bool hex_gap_seen = false;
  std::vector<std::uint32_t> hex_points;
  int hex_gap_ms = 300;
  for (int i = 2; i < argc; ++i) {
    const std::string_view key = argv[i];
    if (key == "--keys") {
      keys = true;
      continue;
    }
    if (key == "--mouse") {
      mouse = true;
      continue;
    }
    if (key == "--paste") {
      paste = true;
      continue;
    }
    std::string_view value;
    if (!next_value(argc, argv, i, value)) return 2;
    if (key == "--serial") {
      if (serial) {
        std::cerr << "Duplicate --serial\n";
        return 2;
      }
      serial = std::string(value);
    } else if (key == "--relay") {
      if (relay) {
        std::cerr << "Duplicate --relay\n";
        return 2;
      }
      relay = std::string(value);
    } else if (key == "--baud") {
      baud_seen = true;
      if (!parse_bounded_int(value, 1, 4000000, baud)) {
        std::cerr << "Invalid --baud\n";
        return 2;
      }
    } else if (key == "--control-port") {
      port_seen = true;
      if (!parse_bounded_int(value, 1, 65535, control_port)) {
        std::cerr << "Invalid --control-port\n";
        return 2;
      }
    } else if (key == "--timeout-ms") {
      if (!parse_bounded_int(value, 1, 600000, timeout_ms)) {
        std::cerr << "Invalid --timeout-ms\n";
        return 2;
      }
    } else if (key == "--hex-probe") {
      if (!hex_points.empty()) {
        std::cerr << "Duplicate --hex-probe\n";
        return 2;
      }
      if (!parse_hex_probe_list(value, hex_points)) {
        std::cerr << "Invalid --hex-probe\n";
        return 2;
      }
    } else if (key == "--hex-gap-ms") {
      hex_gap_seen = true;
      if (!parse_bounded_int(value, 0, 60000, hex_gap_ms)) {
        std::cerr << "Invalid --hex-gap-ms\n";
        return 2;
      }
    } else {
      std::cerr << "Unknown probe-control option\n";
      return 2;
    }
  }
  if (serial.has_value() == relay.has_value()) {
    std::cerr
        << "--probe-control requires exactly one of --serial or --relay\n";
    return 2;
  }
  if (relay && baud_seen) {
    std::cerr << "--baud applies only to --serial\n";
    return 2;
  }
  if (serial && port_seen) {
    std::cerr << "--control-port applies only to --relay\n";
    return 2;
  }
  if (relay && !hex_points.empty()) {
    std::cerr << "--hex-probe requires --serial\n";
    return 2;
  }
  if (hex_gap_seen && hex_points.empty()) {
    std::cerr << "--hex-gap-ms requires --hex-probe\n";
    return 2;
  }

  kvmux::ProbeControlOptions options;
  options.keys = keys;
  options.mouse = mouse;
  options.paste = paste;
  options.hex_points = std::move(hex_points);
  options.hex_gap_ms = hex_gap_ms;
  options.timeout_ms = timeout_ms;
  options.baud = baud;
  options.control_port = control_port;
  if (serial) {
    options.serial = true;
    options.declare_freshness = false;
    options.target = *serial;
    kvmux::Ch9329ControlSink sink;
    return kvmux::probe_control(std::cout, sink, options);
  }
  options.serial = false;
  options.declare_freshness = true;
  options.target = *relay;
  kvmux::relay::ClientOptions client;
  client.host = *relay;
  client.control_port = static_cast<std::uint16_t>(control_port);
  // Control role still resolves a video port and does not open media.
  client.video_port = 17001;
  client.role = kvmux::relay::wire::RelayRole::control;
  auto shared = std::make_shared<kvmux::relay::RelayClient>(std::move(client));
  kvmux::relay::NetworkControlSink sink(std::move(shared));
  return kvmux::probe_control(std::cout, sink, options);
}

int probe_video_command(int argc, char** argv) {
  std::optional<std::string> host;
  int control_port = 17000;
  int video_port = 17001;
  int seconds = 5;
  for (int i = 2; i < argc; ++i) {
    const std::string_view key = argv[i];
    if (!key.starts_with("--")) {
      if (host) {
        std::cerr << "Unexpected argument\n";
        return 2;
      }
      host = std::string(key);
      continue;
    }
    std::string_view value;
    if (!next_value(argc, argv, i, value)) return 2;
    if (key == "--control-port") {
      if (!parse_bounded_int(value, 1, 65535, control_port)) {
        std::cerr << "Invalid --control-port\n";
        return 2;
      }
    } else if (key == "--video-port") {
      if (!parse_bounded_int(value, 1, 65535, video_port)) {
        std::cerr << "Invalid --video-port\n";
        return 2;
      }
    } else if (key == "--seconds") {
      if (!parse_bounded_int(value, 1, 86400, seconds)) {
        std::cerr << "Invalid --seconds\n";
        return 2;
      }
    } else {
      std::cerr << "Unknown probe-video option\n";
      return 2;
    }
  }
  if (!host || host->empty()) {
    std::cerr << "--probe-video requires a host\n";
    return 2;
  }
  kvmux::relay::ClientOptions client;
  client.host = *host;
  client.control_port = static_cast<std::uint16_t>(control_port);
  client.video_port = static_cast<std::uint16_t>(video_port);
  client.role = kvmux::relay::wire::RelayRole::video;
  auto shared = std::make_shared<kvmux::relay::RelayClient>(std::move(client));
  kvmux::relay::NetworkCaptureSource source(std::move(shared));
  return kvmux::probe_video(std::cout, source, seconds);
}

int run(int argc, char** argv) {
  if (argc >= 2 && std::string_view(argv[1]) == "--serve")
    return serve(argc, argv);
  if (argc >= 2 && std::string_view(argv[1]) == "--probe-control")
    return probe_control_command(argc, argv);
  if (argc >= 2 && std::string_view(argv[1]) == "--probe-video")
    return probe_video_command(argc, argv);
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    print_help(std::cout);
    return 0;
  }
  if (argc == 2 && std::string_view(argv[1]) == "--list-serial") {
    const auto ports = kvmux::enumerate_serial_ports();
    std::cout << "PORT\tDESCRIPTION\n";
    for (const auto& port : ports) {
      std::cout << std::quoted(port.name) << '\t'
                << std::quoted(port.description) << '\n';
    }
    if (ports.empty()) std::cout << "No serial ports found.\n";
    return 0;
  }
  const bool list_devices =
      argc == 2 && std::string_view(argv[1]) == "--list-devices";
  const bool list_modes =
      argc == 3 && std::string_view(argv[1]) == "--list-modes";
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
              << mode.frame_rate.numerator << '/' << mode.frame_rate.denominator
              << '\t' << std::quoted(mode.device_format_name) << '\n';
  }
  if (modes.empty()) {
    std::cerr << "No capture modes found for " << std::quoted(device_id)
              << '\n';
    return 1;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    bool debug = false;
    std::string log_file;
    std::string serial_port;
    std::string baud;
    std::vector<char*> arguments{argv[0]};
    for (int i = 1; i < argc; ++i) {
      const std::string_view key = argv[i];
      if (key == "--debug") {
        debug = true;
        continue;
      }
      if (key == "--log-file") {
        if (i + 1 >= argc || std::string_view(argv[i + 1]).starts_with("--")) {
          std::cerr << "--log-file requires a path\n";
          return 2;
        }
        if (!log_file.empty()) {
          std::cerr << "Duplicate --log-file\n";
          return 2;
        }
        log_file = argv[++i];
        if (log_file.empty()) {
          std::cerr << "--log-file requires a path\n";
          return 2;
        }
        continue;
      }
      if (key == "--serial" && i + 1 < argc &&
          !std::string_view(argv[i + 1]).starts_with("--")) {
        serial_port = argv[i + 1];
      } else if (key == "--baud" && i + 1 < argc &&
                 !std::string_view(argv[i + 1]).starts_with("--")) {
        baud = argv[i + 1];
      }
      arguments.push_back(argv[i]);
    }
    if (!kvmux::configure_console_logging(debug, log_file, "kvmux-relay",
                                          serial_port, baud)) {
      return 2;
    }
    const kvmux::ConsoleLoggingSession logging_session;
    return run(static_cast<int>(arguments.size()), arguments.data());
  } catch (const std::exception& error) {
    std::cerr << "kvmux-relay: " << error.what() << '\n';
    return 1;
  }
}
