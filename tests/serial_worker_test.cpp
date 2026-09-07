#include "control/ch9329_protocol.hpp"
#include "control/serial_worker.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;

void require(bool value, const char* message) {
    if (!value) { throw std::runtime_error(message); }
}

struct FakeSerial {
    std::mutex mutex;
    bool opened{};
    std::size_t write_limit{3};
    bool answer{true};
    std::deque<std::uint8_t> incoming;
    kvmux::ch9329::Parser requests;
    std::vector<kvmux::ch9329::Frame> received;

    kvmux::SerialIo io() {
        return {
            [this](const std::string&, int) { std::lock_guard lock(mutex); opened = true; return true; },
            [this] { std::lock_guard lock(mutex); opened = false; },
            [this](std::span<const std::uint8_t> bytes) -> std::ptrdiff_t {
                std::lock_guard lock(mutex);
                if (!opened) { return -1; }
                const auto count = std::min(write_limit, bytes.size());
                auto frames = requests.feed(bytes.first(count));
                for (const auto& request : frames) {
                    received.push_back(request);
                    if (!answer) { continue; }
                    kvmux::ch9329::Frame response{request.address,
                        static_cast<std::uint8_t>(request.command | 0x80U), {0}};
                    if (request.command == 0x01U) {
                        response.data = {0x31, 1, 0, 0, 0, 0, 0, 0};
                    } else if (request.command == 0x08U) {
                        response.data.assign(50, 0);
                        response.data[0] = 0x80;
                        response.data[1] = 0x80;
                    }
                    const auto encoded = kvmux::ch9329::encode(response);
                    incoming.insert(incoming.end(), encoded.begin(), encoded.end());
                }
                return static_cast<std::ptrdiff_t>(count);
            },
            [this](std::span<std::uint8_t> bytes) -> std::ptrdiff_t {
                std::lock_guard lock(mutex);
                if (!opened) { return -1; }
                const auto count = std::min(bytes.size(), incoming.size());
                for (std::size_t i = 0; i < count; ++i) {
                    bytes[i] = incoming.front(); incoming.pop_front();
                }
                return static_cast<std::ptrdiff_t>(count);
            },
        };
    }
};

template<class Predicate>
bool eventually(Predicate predicate, std::chrono::milliseconds timeout = 1000ms) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < until) {
        if (predicate()) { return true; }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

}  // namespace

int main() {
    using namespace kvmux;
    FakeSerial fake;
    Ch9329ControlSink sink(fake.io());
    sink.connect("fake", 57600);
    require(eventually([&] { return sink.snapshot().state == ControlConnectionState::ready; }),
            "handshake and initial release reach ready");
    {
        std::lock_guard lock(fake.mutex);
        require(fake.received.size() >= 5 && fake.received[0].command == 0x01 &&
                fake.received[1].command == 0x08 && fake.received[2].command == 0x02 &&
                fake.received[3].command == 0x04 && fake.received[4].command == 0x05,
                "partial writes preserve handshake and full release order");
    }

    auto epoch = sink.snapshot().epoch;
    sink.set_control_active(true);
    require(sink.submit({epoch, 1, std::chrono::steady_clock::now(), KeyEdge{0xe0, true}}) ==
                SubmitResult::accepted, "control event accepted");
    require(eventually([&] {
        std::lock_guard lock(fake.mutex);
        return std::ranges::any_of(fake.received, [](const auto& frame) {
            return frame.command == 0x02 && !frame.data.empty() && frame.data[0] == 1;
        });
    }), "key down sent");

    {
        std::lock_guard lock(fake.mutex);
        fake.answer = false;
    }
    require(sink.submit({epoch, 2, std::chrono::steady_clock::now(), RelativeMotion{19, -7}}) ==
                SubmitResult::accepted, "relative move accepted");
    require(eventually([&] { return sink.snapshot().state == ControlConnectionState::stalled; }, 300ms),
            "late ack stalls and invalidates session");
    require(sink.snapshot().epoch != epoch, "stalled transaction advances epoch");
    require(eventually([&] { return sink.snapshot().timeout_count == 1; }, 800ms),
            "missing ack reaches hard timeout");

    std::size_t motion_count{};
    {
        std::lock_guard lock(fake.mutex);
        motion_count = std::ranges::count_if(fake.received, [](const auto& frame) {
            return frame.command == 0x05 && frame.data.size() == 5 && frame.data[2] == 19;
        });
        fake.answer = true;
    }
    require(eventually([&] { return sink.snapshot().state == ControlConnectionState::ready; }, 1200ms),
            "reconnect handshakes and clears before ready");
    {
        std::lock_guard lock(fake.mutex);
        require(std::ranges::count_if(fake.received, [](const auto& frame) {
            return frame.command == 0x05 && frame.data.size() == 5 && frame.data[2] == 19;
        }) == static_cast<std::ptrdiff_t>(motion_count), "relative motion not replayed");
        require(std::ranges::any_of(fake.received, [](const auto& frame) {
            return frame.command == 0x05 && frame.data.size() == 5 &&
                   frame.data[1] == 0 && frame.data[2] == 0 && frame.data[3] == 0;
        }), "reconnect includes a clear report, not old input");
    }

    sink.disconnect();
    return EXIT_SUCCESS;
}
