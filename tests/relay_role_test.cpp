#include <sys/wait.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "network/kcp_channel.hpp"
#include "network/relay_session.hpp"
#include "relay_test_fakes.hpp"
#include "support/crc32.hpp"

using namespace kvmux;
using namespace kvmux::relay;
using namespace std::chrono_literals;
namespace w = kvmux::relay::wire;
using Clock = std::chrono::steady_clock;

void check(bool ok,
           std::source_location loc = std::source_location::current()) {
  if (!ok) {
    std::cerr << "relay role check failed at " << loc.line() << '\n';
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
udp::Socket bind_local() {
  std::string error;
  auto socket = udp::Socket::bind("127.0.0.1", 0, error);
  check(socket.has_value());
  return std::move(*socket);
}
udp::Endpoint resolve_local(std::uint16_t port) {
  std::string error;
  auto endpoint = udp::resolve("127.0.0.1", port, error);
  check(endpoint.has_value());
  return *endpoint;
}
ServerOptions local_options(std::uint16_t control, std::uint16_t video) {
  ServerOptions options;
  options.bind_address = "127.0.0.1";
  options.control_port = control;
  options.video_port = video;
  options.codec = VideoCodec::mjpeg;
  return options;
}
std::uint32_t milliseconds(Clock::time_point now) {
  return static_cast<std::uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now.time_since_epoch())
          .count());
}
bool keyboard_down(const ch9329::Frame& frame) {
  return frame.command == 0x02 && frame.data.size() >= 8 && frame.data[2] == 4;
}
bool keyboard_clear(const ch9329::Frame& frame) {
  return frame.command == 0x02 && frame.data.size() >= 8 &&
         frame.data[0] == 0 && frame.data[2] == 0;
}

struct DirectClient {
  udp::Socket socket;
  udp::Endpoint control_peer;
  ClientSession session;
  std::unique_ptr<KcpChannel> kcp;
  bool drop_proofs{false};
  int media{0};
  int media_late{0};
  int challenges_late{0};
  int state_acks{0};
  int paste_statuses{0};
  std::optional<Clock::time_point> late_at;
  std::optional<w::BusyReason> rejection;
  std::optional<w::Status> status;
  std::optional<w::StateAck> ack;
  std::optional<w::PasteStatus> paste;

  DirectClient(std::uint16_t port, w::RelayRole role, std::uint8_t codecs = 7)
      : socket(bind_local()),
        control_peer(resolve_local(port)),
        session(control_peer, codecs, role) {}

  bool send_to(const udp::Endpoint& peer, std::span<const std::uint8_t> bytes) {
    for (int attempt = 0; attempt < 20; ++attempt) {
      const auto result = socket.send_to(peer, bytes);
      if (result == udp::SendStatus::sent) return true;
      if (result != udp::SendStatus::would_block) return false;
      std::this_thread::sleep_for(1ms);
    }
    return false;
  }
  void send_actions(const SessionActions& actions) {
    for (const auto& action : actions) {
      if (action.kind == SessionAction::Kind::rejected)
        rejection = action.rejection;
      if (action.kind == SessionAction::Kind::established)
        kcp = std::make_unique<KcpChannel>(session.tuple().conversation);
      if (action.kind != SessionAction::Kind::send_raw) continue;
      if (drop_proofs) {
        const auto envelope = w::decode_envelope(action.datagram);
        if (envelope && envelope->kind == w::EnvelopeKind::proof) continue;
      }
      check(send_to(action.peer, action.datagram));
    }
  }
  void note(w::EnvelopeKind kind) {
    const bool late = late_at && Clock::now() >= *late_at;
    if (kind == w::EnvelopeKind::media) {
      ++media;
      if (late) ++media_late;
    } else if (kind == w::EnvelopeKind::challenge && late) {
      ++challenges_late;
    }
  }
  void take_control() {
    if (!kcp) return;
    while (auto bytes = kcp->receive()) {
      auto message = w::decode_control(*bytes, w::Direction::server_to_client);
      if (!message) continue;
      if (const auto* value = std::get_if<w::Status>(&*message))
        status = *value;
      if (const auto* value = std::get_if<w::StateAck>(&*message)) {
        ack = *value;
        ++state_acks;
      }
      if (const auto* value = std::get_if<w::PasteStatus>(&*message)) {
        paste = *value;
        ++paste_statuses;
      }
    }
  }
  bool flush() {
    if (!kcp) return false;
    const auto now = Clock::now();
    kcp->update(milliseconds(now));
    check(!kcp->failed());
    bool sent = false;
    for (auto& datagram : kcp->take_datagrams()) {
      auto bytes =
          w::encode_envelope({w::EnvelopeKind::kcp, session.tuple(), datagram});
      check(bytes.has_value());
      check(send_to(control_peer, *bytes));
      sent = true;
    }
    return sent;
  }
  // KCP flushes on a 10 ms interval. A submit just after update emits nothing
  // until the next interval.
  bool emit() {
    return until([this] { return flush(); }, 40ms);
  }
  void pump_once() {
    const auto now = Clock::now();
    send_actions(session.tick(now));
    for (int i = 0; i < 32; ++i) {
      auto packet = socket.receive(i == 0 ? 2ms : 0ms);
      if (packet.status == udp::ReceiveStatus::idle) break;
      check(packet.status == udp::ReceiveStatus::datagram);
      auto envelope = w::decode_envelope(packet.datagram.bytes);
      if (!envelope) continue;
      note(envelope->kind);
      if (envelope->kind == w::EnvelopeKind::media) continue;
      if (envelope->kind == w::EnvelopeKind::kcp) {
        if (kcp) check(kcp->input(envelope->body));
        continue;
      }
      send_actions(
          session.on_datagram(control_peer, packet.datagram.bytes, now));
    }
    take_control();
    (void)flush();
  }
  void pump_for(std::chrono::milliseconds budget) {
    const auto end = Clock::now() + budget;
    while (Clock::now() < end) pump_once();
  }
  void handshake() {
    send_actions(session.start(11, Clock::now()));
    const auto deadline = Clock::now() + 2s;
    while (session.phase() != SessionPhase::established && !rejection &&
           Clock::now() < deadline)
      pump_once();
  }
  bool submit(const w::Control& value) {
    if (!kcp) return false;
    auto bytes = w::encode_control(value, w::Direction::client_to_server);
    return bytes && kcp->submit(*bytes) == kvmux::relay::SubmitResult::accepted;
  }
  std::uint64_t fresh_challenge() {
    const auto previous = session.latest_challenge();
    const auto deadline = Clock::now() + 500ms;
    while (Clock::now() < deadline && (session.latest_challenge() == 0 ||
                                       session.latest_challenge() == previous))
      pump_once();
    check(session.latest_challenge() != 0);
    check(session.latest_challenge() != previous || previous == 0);
    return session.latest_challenge();
  }
};

struct ControlRelay {
  FakeSerial serial;
  Ch9329ControlSink sink;
  udp::Socket sniffer;
  RelayServer server;
  std::unique_ptr<DirectClient> client;
  int sniffed{0};

  ControlRelay()
      : sink(serial.io()), sniffer(bind_local()), server(nullptr, &sink) {
    sink.connect("fake", 115200);
    check(until([&] { return sink.snapshot().release_confirmed; }));
    std::string error;
    check(server.start(local_options(0, *sniffer.local_port()), error));
    check(server.video_port() == 0);
    client = std::make_unique<DirectClient>(server.control_port(),
                                            w::RelayRole::control);
    client->handshake();
    check(client->session.phase() == SessionPhase::established);
    client->session.set_intent(1, true, true, 1);
    check(until([&] {
      drain();
      client->pump_once();
      const auto& state = client->status;
      return state && state->connection == ControlConnectionState::ready &&
             state->usb_ready && state->release_confirmed;
    }));
  }
  void drain() {
    for (;;) {
      auto packet = sniffer.receive(0ms);
      if (packet.status != udp::ReceiveStatus::datagram) break;
      ++sniffed;
    }
  }
};

void press_key(ControlRelay& relay) {
  auto& client = *relay.client;
  DesiredInputState state;
  state.mode = MouseMode::relative;
  bool acked = false;
  for (int attempt = 0; attempt < 2 && !acked; ++attempt) {
    const auto challenge = client.fresh_challenge();
    check(client.status.has_value());
    check(client.submit(
        w::Sync{client.status->epoch, 1, 1, challenge, 0, state}));
    check(client.emit());
    const auto deadline = Clock::now() + 500ms;
    while (Clock::now() < deadline) {
      relay.drain();
      client.pump_once();
      if (client.ack) {
        acked = true;
        break;
      }
    }
  }
  check(acked);
  const auto challenge = client.fresh_challenge();
  const auto sequence = client.ack->edge_floor + 1;
  check(client.submit(w::Edge{client.ack->epoch, 1, sequence, sequence,
                              challenge, KeyEdge{4, true}}));
  check(client.emit());
  check(until([&] {
    relay.drain();
    client.pump_once();
    std::lock_guard lock(relay.serial.mutex);
    return std::any_of(relay.serial.received.begin(),
                       relay.serial.received.end(), keyboard_down);
  }));
}

void sync_owner(ControlRelay& relay) {
  auto& client = *relay.client;
  DesiredInputState state;
  state.mode = MouseMode::relative;
  bool acked = false;
  for (int attempt = 0; attempt < 2 && !acked; ++attempt) {
    const auto challenge = client.fresh_challenge();
    check(client.status.has_value());
    check(client.submit(
        w::Sync{client.status->epoch, 1, 1, challenge, 0, state}));
    check(client.emit());
    const auto deadline = Clock::now() + 500ms;
    while (Clock::now() < deadline) {
      relay.drain();
      client.pump_once();
      if (client.ack) {
        acked = true;
        break;
      }
    }
  }
  check(acked);
  check(until([&] {
    relay.drain();
    client.pump_once();
    const auto snapshot = relay.sink.snapshot();
    return snapshot.applied.known &&
           snapshot.state == ControlConnectionState::ready &&
           snapshot.release_confirmed && !snapshot.ordinary_input_pending;
  }));
}

void both_null_start_fails() {
  RelayServer server(nullptr, nullptr);
  std::string error = "unchanged";
  check(!server.start(local_options(0, 0), error));
  check(error == "Relay requires capture, serial, or both");
}
void mismatched_role_is_rejected(const char* jpeg) {
  {
    FakeCapture capture;
    capture.jpeg = load_file(jpeg);
    RelayServer server(&capture, nullptr);
    std::string error;
    check(server.start(local_options(0, 0), error));
    DirectClient client(server.control_port(), w::RelayRole::control);
    client.handshake();
    check(client.rejection == w::BusyReason::role_unsupported);
    check(client.session.phase() == SessionPhase::idle);
  }
  {
    FakeCapture capture;
    capture.jpeg = load_file(jpeg);
    FakeSerial serial;
    Ch9329ControlSink sink(serial.io());
    RelayServer server(&capture, &sink);
    std::string error;
    check(server.start(local_options(0, 0), error));
    DirectClient client(server.control_port(), w::RelayRole::video);
    client.handshake();
    check(client.rejection == w::BusyReason::role_unsupported);
    check(client.session.phase() == SessionPhase::idle);
  }
}
void video_relay_keeps_session_and_rejects_control(const char* jpeg) {
  FakeSerial serial;
  FakeCapture capture;
  capture.jpeg = load_file(jpeg);
  RelayServer server(&capture, nullptr);
  std::string error;
  check(server.start(local_options(0, 0), error));
  check(server.video_port() != 0);
  DirectClient client(server.control_port(), w::RelayRole::video);
  client.handshake();
  check(client.session.phase() == SessionPhase::established);
  client.late_at = Clock::now() + 10s;
  client.pump_for(11s);
  check(client.session.phase() == SessionPhase::established);
  check(client.media_late > 0);
  check(client.challenges_late > 0);
  const auto acks = client.state_acks;
  const auto pastes = client.paste_statuses;
  DesiredInputState state;
  state.mode = MouseMode::relative;
  check(client.submit(w::Sync{1, 1, 1, client.fresh_challenge(), 0, state}));
  check(client.emit());
  client.pump_for(300ms);
  const std::vector<std::uint8_t> text = {'a'};
  check(client.submit(w::PasteBegin{9, 1, support::crc32_ieee(text)}));
  check(client.emit());
  client.pump_for(300ms);
  check(client.state_acks == acks);
  check(client.paste_statuses == pastes);
  check(!serial.opened);
  check(serial.received.empty());
}
void control_relay_delivers_keys_without_media() {
  ControlRelay relay;
  press_key(relay);
  check(relay.client->media == 0);
  check(relay.sniffed == 0);
  std::lock_guard lock(relay.serial.mutex);
  check(std::any_of(relay.serial.received.begin(), relay.serial.received.end(),
                    keyboard_down));
}
void control_lease_expiry_releases_keyboard() {
  ControlRelay relay;
  press_key(relay);
  std::size_t key_at = 0;
  {
    std::lock_guard lock(relay.serial.mutex);
    for (std::size_t i = 0; i < relay.serial.received.size(); ++i)
      if (keyboard_down(relay.serial.received[i])) key_at = i;
    check(key_at + 1 == relay.serial.received.size() ||
          !keyboard_clear(relay.serial.received.back()));
  }
  relay.client->drop_proofs = true;
  check(until(
      [&] {
        relay.drain();
        relay.client->pump_once();
        std::lock_guard lock(relay.serial.mutex);
        for (std::size_t i = key_at + 1; i < relay.serial.received.size(); ++i)
          if (keyboard_clear(relay.serial.received[i])) return true;
        return false;
      },
      2000ms));
  check(relay.client->session.phase() == SessionPhase::established);
  check(relay.client->media == 0);
  check(relay.sniffed == 0);
}
void control_stale_picture_cancels_paste_and_releases() {
  ControlRelay relay;
  // An edge clears applied ownership. Paste can start only after a sync.
  sync_owner(relay);
  std::size_t mark = 0;
  {
    std::lock_guard lock(relay.serial.mutex);
    relay.serial.write_limit = 4096;
    relay.serial.hold_keyboard_ack = true;
    mark = relay.serial.received.size();
  }
  const std::vector<std::uint8_t> text = {'a'};
  const auto crc = support::crc32_ieee(text);
  constexpr std::uint64_t id = 77;
  auto& client = *relay.client;
  (void)client.fresh_challenge();
  check(client.submit(w::PasteBegin{id, 1, crc}));
  check(client.submit(w::PasteChunk{id, 0, text}));
  check(client.submit(w::PasteExecute{id}));
  check(client.emit());
  auto retried = Clock::time_point{};
  check(until(
      [&] {
        relay.drain();
        client.pump_once();
        const auto now = Clock::now();
        if (now - retried >= 50ms) {
          if (!client.paste ||
              client.paste->state == w::PasteState::uploading ||
              client.paste->state == w::PasteState::uploaded)
            (void)client.submit(w::PasteExecute{id});
          else
            (void)client.submit(w::PasteKeepalive{id});
          (void)client.flush();
          retried = now;
        }
        bool down = false;
        {
          std::lock_guard lock(relay.serial.mutex);
          for (std::size_t i = mark; i < relay.serial.received.size(); ++i)
            down = down || keyboard_down(relay.serial.received[i]);
        }
        return down && client.paste &&
               client.paste->state == w::PasteState::executing;
      },
      1000ms));
  check(client.submit(w::PasteKeepalive{id}));
  check(client.emit());
  client.pump_for(30ms);
  client.session.set_intent(1, true, false, 1);
  auto kept = Clock::time_point{};
  check(until(
      [&] {
        relay.drain();
        const auto now = Clock::now();
        if (now - kept >= 50ms) {
          (void)client.submit(w::PasteKeepalive{id});
          (void)client.flush();
          kept = now;
        }
        client.pump_once();
        return relay.sink.ascii_paste_snapshot().state ==
               AsciiPasteState::canceled;
      },
      300ms));
  {
    std::lock_guard lock(relay.serial.mutex);
    relay.serial.hold_keyboard_ack = false;
    relay.serial.incoming.insert(relay.serial.incoming.end(),
                                 relay.serial.held_ack.begin(),
                                 relay.serial.held_ack.end());
    relay.serial.held_ack.clear();
  }
  std::size_t key_at = 0;
  {
    std::lock_guard lock(relay.serial.mutex);
    for (std::size_t i = 0; i < relay.serial.received.size(); ++i)
      if (keyboard_down(relay.serial.received[i])) key_at = i;
  }
  check(until(
      [&] {
        relay.drain();
        client.pump_once();
        std::lock_guard lock(relay.serial.mutex);
        for (std::size_t i = key_at + 1; i < relay.serial.received.size(); ++i)
          if (keyboard_clear(relay.serial.received[i])) return true;
        return false;
      },
      1000ms));
  check(relay.client->media == 0);
  check(relay.sniffed == 0);
}
void combined_flags_exit_before_device_enumeration(const char* binary) {
  const char* commands[] = {
      " --serve --no-capture --no-serial --device nowhere",
      " --serve --device nowhere --no-serial --no-capture",
  };
  for (const char* args : commands) {
    const std::string command =
        std::string("\"") + binary + "\"" + args + " 2>&1";
    FILE* pipe = popen(command.c_str(), "r");
    check(pipe != nullptr);
    std::string output;
    char buffer[512];
    while (std::fgets(buffer, sizeof buffer, pipe) != nullptr) output += buffer;
    const int status = pclose(pipe);
    if (output.find("--no-capture and --no-serial cannot be combined") ==
        std::string::npos)
      std::cerr << output << '\n';
    check(status >= 0 && WIFEXITED(status) && WEXITSTATUS(status) != 0);
    check(output.find("--no-capture and --no-serial cannot be combined") !=
          std::string::npos);
    check(output.find("Capture device not found") == std::string::npos);
    check(output.find("No capture devices") == std::string::npos);
    check(output.find("No serial") == std::string::npos);
  }
}
int main(int argc, char** argv) {
  check(argc >= 3);
  both_null_start_fails();
  mismatched_role_is_rejected(argv[1]);
  control_relay_delivers_keys_without_media();
  control_lease_expiry_releases_keyboard();
  control_stale_picture_cancels_paste_and_releases();
  combined_flags_exit_before_device_enumeration(argv[2]);
  video_relay_keeps_session_and_rejects_control(argv[1]);
  std::cout << "relay_role: passed\n";
}
