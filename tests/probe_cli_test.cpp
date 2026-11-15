#include <sys/wait.h>
#include <unistd.h>

#include <charconv>
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <source_location>
#include <string>
#include <thread>
#include <vector>

#include "network/relay_server.hpp"
#include "relay_test_fakes.hpp"

using namespace kvmux;
using namespace kvmux::relay;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

void check(bool ok,
           std::source_location loc = std::source_location::current()) {
  if (!ok) {
    std::cerr << "probe cli check failed at " << loc.line() << '\n';
    std::abort();
  }
}
bool until(const std::function<bool()>& condition,
           std::chrono::milliseconds timeout = 2000ms) {
  const auto deadline = Clock::now() + timeout;
  do {
    if (condition()) return true;
    std::this_thread::sleep_for(2ms);
  } while (Clock::now() < deadline);
  return condition();
}
std::vector<std::uint8_t> load_file(const char* path) {
  std::ifstream input(path, std::ios::binary);
  check(static_cast<bool>(input));
  return {std::istreambuf_iterator<char>(input), {}};
}

struct CommandResult {
  int code{-1};
  std::string output;
};
CommandResult run_relay(const char* binary,
                        const std::vector<std::string>& args) {
  int pipes[2]{};
  check(pipe(pipes) == 0);
  const pid_t pid = fork();
  check(pid >= 0);
  if (pid == 0) {
    if (dup2(pipes[1], STDOUT_FILENO) < 0 || dup2(pipes[1], STDERR_FILENO) < 0)
      _exit(127);
    close(pipes[0]);
    close(pipes[1]);
    std::vector<std::string> storage;
    storage.emplace_back(binary);
    storage.insert(storage.end(), args.begin(), args.end());
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (auto& arg : storage) argv.push_back(arg.data());
    argv.push_back(nullptr);
    execv(argv[0], argv.data());
    _exit(127);
  }
  close(pipes[1]);
  CommandResult result;
  char buffer[512];
  while (true) {
    const auto count = read(pipes[0], buffer, sizeof buffer);
    if (count == 0) break;
    check(count > 0);
    result.output.append(buffer, static_cast<std::size_t>(count));
  }
  close(pipes[0]);
  int status = 0;
  check(waitpid(pid, &status, 0) == pid);
  if (WIFEXITED(status)) result.code = WEXITSTATUS(status);
  return result;
}
void require_code(const CommandResult& result, int code) {
  if (result.code != code) {
    std::cerr << result.output << "\nexit " << result.code << " expected "
              << code << '\n';
    std::abort();
  }
}
void require_contains(const CommandResult& result, const std::string& text) {
  if (result.output.find(text) == std::string::npos) {
    std::cerr << result.output << "\nmissing: " << text << '\n';
    std::abort();
  }
}
int frames_in(const std::string& output) {
  const auto pos = output.find("frames=");
  check(pos != std::string::npos);
  const char* begin = output.c_str() + pos + 7;
  const char* end = output.c_str() + output.size();
  int value = 0;
  const auto parsed = std::from_chars(begin, end, value);
  check(parsed.ec == std::errc{});
  return value;
}
ServerOptions local_options() {
  ServerOptions options;
  options.bind_address = "127.0.0.1";
  options.control_port = 0;
  options.video_port = 0;
  options.codec = VideoCodec::mjpeg;
  return options;
}
void start_server(RelayServer& server) {
  std::string error;
  check(server.start(local_options(), error));
}

void hex_probe_argument_errors(const char* binary) {
  const auto relay = run_relay(
      binary, {"--probe-control", "--relay", "127.0.0.1", "--hex-probe", "41"});
  require_code(relay, 2);
  require_contains(relay, "--hex-probe requires --serial");
  check(relay.output.find("[PASS]") == std::string::npos);

  const auto surrogate = run_relay(
      binary,
      {"--probe-control", "--serial", "/dev/null", "--hex-probe", "d800"});
  require_code(surrogate, 2);
  require_contains(surrogate, "Invalid --hex-probe");
  check(surrogate.output.find("[FAIL]") == std::string::npos);

  const auto range = run_relay(binary, {"--probe-control", "--serial",
                                        "/dev/null", "--hex-probe", "110000"});
  require_code(range, 2);
  require_contains(range, "Invalid --hex-probe");

  std::string many = "1";
  for (int index = 1; index < 17; ++index) many += ",1";
  const auto too_many = run_relay(binary, {"--probe-control", "--serial",
                                           "/dev/null", "--hex-probe", many});
  require_code(too_many, 2);
  require_contains(too_many, "Invalid --hex-probe");

  const auto gap = run_relay(binary, {"--probe-control", "--serial",
                                      "/dev/null", "--hex-gap-ms", "1"});
  require_code(gap, 2);
  require_contains(gap, "--hex-gap-ms requires --hex-probe");

  const auto trace_only = run_relay(
      binary, {"--probe-control", "--serial", "/dev/null", "--hex-trace"});
  require_code(trace_only, 2);
  require_contains(trace_only, "--hex-trace requires --hex-probe");

  const auto single_only = run_relay(
      binary, {"--probe-control", "--serial", "/dev/null", "--hex-single"});
  require_code(single_only, 2);
  require_contains(single_only, "--hex-single requires --hex-probe");

  const auto single_two =
      run_relay(binary, {"--probe-control", "--serial", "/dev/null",
                         "--hex-probe", "4e2d,7684", "--hex-single"});
  require_code(single_two, 2);
  require_contains(single_two, "--hex-single accepts one code point");

  const auto lead_only = run_relay(binary, {"--probe-control", "--serial",
                                            "/dev/null", "--hex-lead-ms", "1"});
  require_code(lead_only, 2);
  require_contains(lead_only, "--hex-lead-ms requires --hex-probe");

  const auto lead_range =
      run_relay(binary, {"--probe-control", "--serial", "/dev/null",
                         "--hex-probe", "4e2d", "--hex-lead-ms", "60001"});
  require_code(lead_range, 2);
  require_contains(lead_range, "Invalid --hex-lead-ms");

  const auto one = run_relay(
      binary, {"--probe-control", "--serial", "/dev/null", "--hex-probe",
               "4e2d", "--hex-single", "--hex-trace", "--hex-lead-ms", "0"});
  require_code(one, 1);
  require_contains(one, "[FAIL] open");
  check(one.output.find("Invalid --hex-probe") == std::string::npos);
}

void argument_errors_exit_2(const char* binary) {
  const auto both = run_relay(binary, {"--probe-control", "--serial",
                                       "/dev/null", "--relay", "127.0.0.1"});
  require_code(both, 2);
  check(both.output.find("[PASS]") == std::string::npos);
  const auto neither = run_relay(binary, {"--probe-control"});
  require_code(neither, 2);
  check(neither.output.find("[PASS]") == std::string::npos);
}

void missing_serial_fails_open(const char* binary) {
  const auto missing =
      run_relay(binary, {"--probe-control", "--serial", "/dev/does-not-exist"});
  require_code(missing, 1);
  require_contains(missing, "[FAIL] open");
  const auto line = missing.output.find("[FAIL] open");
  check(line != std::string::npos);
  const auto end = missing.output.find('\n', line);
  check(end != std::string::npos && end > line + 12);

  const auto absent =
      run_relay(binary, {"--probe-control", "--serial", "ch341-usb:9-9"});
  require_code(absent, 1);
  require_contains(absent, "[FAIL] open");
  require_contains(absent, "设备不存在或已拔出");

  const auto paste_missing = run_relay(
      binary,
      {"--probe-control", "--serial", "/dev/does-not-exist", "--paste"});
  require_code(paste_missing, 1);
  require_contains(paste_missing, "[FAIL] open");
  check(paste_missing.output.find("paste stage=") == std::string::npos);
}

void control_relay_probe_passes(const char* binary) {
  FakeSerial serial;
  Ch9329ControlSink sink(serial.io());
  sink.connect("fake", 115200);
  check(until([&] { return sink.snapshot().release_confirmed; }));
  RelayServer server(nullptr, &sink);
  start_server(server);
  check(server.video_port() == 0);
  const auto result = run_relay(
      binary, {"--probe-control", "--relay", "127.0.0.1", "--control-port",
               std::to_string(server.control_port()), "--keys", "--mouse"});
  require_code(result, 0);
  require_contains(result, "[PASS] open");
  require_contains(result, "[PASS] ready");
  require_contains(result, "[PASS] keys");
  require_contains(result, "[PASS] mouse");
  require_contains(result, "[PASS] release");
  require_contains(result, "无画面，CLI 自行声明新鲜，仅用于自检");
  check(result.output.find("[FAIL]") == std::string::npos);
  check(result.output.find("[SKIP]") == std::string::npos);
  check(result.output.find("paste stage=") == std::string::npos);
  check(result.output.find("[PASS] paste") == std::string::npos);
}

void control_relay_paste_passes(const char* binary) {
  FakeSerial serial;
  Ch9329ControlSink sink(serial.io());
  sink.connect("fake", 115200);
  check(until([&] { return sink.snapshot().release_confirmed; }));
  RelayServer server(nullptr, &sink);
  start_server(server);
  const auto result = run_relay(
      binary, {"--probe-control", "--relay", "127.0.0.1", "--control-port",
               std::to_string(server.control_port()), "--paste"});
  require_code(result, 0);
  require_contains(result, "[PASS] paste");
  require_contains(result, "result=completed");
  require_contains(result, "reason=none");
  require_contains(result, "paste stage=active");
  check(result.output.find("[FAIL]") == std::string::npos);
}

void video_relay_probe_counts_frames(const char* binary, const char* jpeg) {
  FakeCapture capture;
  capture.jpeg = load_file(jpeg);
  RelayServer server(&capture, nullptr);
  start_server(server);
  check(server.video_port() != 0);
  const auto result = run_relay(
      binary, {"--probe-video", "127.0.0.1", "--control-port",
               std::to_string(server.control_port()), "--video-port",
               std::to_string(server.video_port()), "--seconds", "11"});
  require_code(result, 0);
  require_contains(result, "codec=MJPEG");
  require_contains(result, "[PASS] video");
  check(frames_in(result.output) > 0);
}

void video_probe_rejects_control_relay(const char* binary) {
  FakeSerial serial;
  Ch9329ControlSink sink(serial.io());
  sink.connect("fake", 115200);
  check(until([&] { return sink.snapshot().release_confirmed; }));
  RelayServer server(nullptr, &sink);
  start_server(server);
  const auto result = run_relay(
      binary, {"--probe-video", "127.0.0.1", "--control-port",
               std::to_string(server.control_port()), "--seconds", "11"});
  require_code(result, 1);
  require_contains(result, "角色不匹配");
  check(result.output.find("[PASS] video") == std::string::npos);
}

int main(int argc, char** argv) {
  check(argc >= 3);
  argument_errors_exit_2(argv[2]);
  hex_probe_argument_errors(argv[2]);
  missing_serial_fails_open(argv[2]);
  control_relay_probe_passes(argv[2]);
  control_relay_paste_passes(argv[2]);
  video_probe_rejects_control_relay(argv[2]);
  video_relay_probe_counts_frames(argv[2], argv[1]);
  std::cout << "probe_cli: passed\n";
  return 0;
}
