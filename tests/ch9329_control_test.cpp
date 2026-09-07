#include "control/ch9329_protocol.hpp"
#include "control/control_queue.hpp"
#include "control/hid_keyboard.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void require(bool value, const char* message) {
    if (!value) { throw std::runtime_error(message); }
}

}  // namespace

int main() {
    using namespace kvmux;
    using namespace kvmux::ch9329;

    const std::array<std::uint8_t, 6> keys{0x04, 0, 0, 0, 0, 0};
    const auto encoded = encode(keyboard_report(0x00, 0x02, keys));
    require(encoded == std::vector<std::uint8_t>({0x57, 0xab, 0x00, 0x02, 0x08,
                0x02, 0x00, 0x04, 0, 0, 0, 0, 0, 0x12}), "keyboard vector");

    const auto absolute = encode(absolute_mouse_report(3, 5, 4095, 0, -1));
    Parser parser;
    std::vector<Frame> decoded;
    for (const auto byte : absolute) {
        const std::array one{byte};
        auto part = parser.feed(one);
        decoded.insert(decoded.end(), part.begin(), part.end());
    }
    require(decoded.size() == 1 && decoded.front().address == 3 &&
            decoded.front().data[2] == 0xff && decoded.front().data[3] == 0x0f &&
            decoded.front().data[6] == 0xff, "fragmented absolute frame");

    auto bad = encoded;
    bad.back() ^= 0x01;
    std::vector<std::uint8_t> noisy{0x00, 0x57, 0x00, 0xff};
    noisy.insert(noisy.end(), bad.begin(), bad.end());
    noisy.insert(noisy.end(), encoded.begin(), encoded.end());
    decoded = parser.feed(noisy);
    require(decoded.size() == 1 && decoded.front().command == 0x02,
            "noise and checksum recovery");

    const std::vector<std::uint8_t> oversized{0x57, 0xab, 0, 1, 65};
    require(parser.feed(oversized).empty() && parser.buffered_bytes() <= 1,
            "oversized length rejected");

    HidKeyboardState keyboard;
    require(keyboard.press(0xe4) && keyboard.modifiers() == 0x10, "right control");
    for (std::uint8_t usage = 4; usage < 10; ++usage) {
        require(keyboard.press(usage), "first six keys admitted");
    }
    require(!keyboard.press(10) && keyboard.ignored(10), "seventh key ignored");
    keyboard.release(4);
    require(!keyboard.press(10), "ignored key not admitted while held");
    keyboard.release(10);
    require(keyboard.press(10), "released ignored key can be pressed again");

    ControlQueue queue;
    queue.set_ready(true);
    const auto now = std::chrono::steady_clock::now();
    const auto epoch = queue.epoch();
    require(queue.submit({epoch, 1, now, RelativeMotion{1.25, -2.0}}, now) ==
                SubmitResult::accepted, "first motion");
    require(queue.submit({epoch, 2, now, RelativeMotion{0.75, 3.0}}, now) ==
                SubmitResult::accepted && queue.size() == 1, "motion merged");
    auto event = queue.pop();
    const auto merged = std::get<RelativeMotion>(event->payload);
    require(merged.dx == 2.0 && merged.dy == 1.0 && event->sequence == 2,
            "motion totals");
    require(queue.submit({epoch, 3, now, KeyEdge{4, true}}, now) ==
                SubmitResult::accepted, "key edge");
    require(queue.submit({epoch, 4, now, RelativeMotion{1, 1}}, now) ==
                SubmitResult::accepted && queue.size() == 2, "edge is boundary");
    queue.request_release();
    require(queue.size() == 0 && queue.release_pending() && queue.epoch() == epoch + 1,
            "release invalidates queue");
    require(queue.take_release_request() && !queue.take_release_request(),
            "release request consumed once");
    require(queue.submit({epoch, 5, now, KeyEdge{4, false}}, now) ==
                SubmitResult::not_ready, "old epoch rejected");

    ControlQueue stale;
    stale.set_ready(true);
    const auto stale_epoch = stale.epoch();
    require(stale.submit({stale_epoch, 1, now - std::chrono::milliseconds(101),
                          KeyEdge{4, true}}, now) == SubmitResult::accepted,
            "initial stale event accepted before age is observable");
    require(stale.submit({stale_epoch, 2, now, KeyEdge{4, false}}, now) ==
                SubmitResult::overloaded && stale.release_pending(),
            "stale queue overload releases");

    return EXIT_SUCCESS;
}
