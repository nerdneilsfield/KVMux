#include "control/ch9329_protocol.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <stdexcept>

namespace kvmux::ch9329 {
namespace {

std::uint8_t low_byte(const std::uint16_t value) {
    return static_cast<std::uint8_t>(value & 0xffU);
}

std::uint8_t high_byte(const std::uint16_t value) {
    return static_cast<std::uint8_t>((value >> 8U) & 0xffU);
}

std::uint8_t signed_byte(const std::int8_t value) {
    return static_cast<std::uint8_t>(value);
}

}  // namespace

std::vector<std::uint8_t> encode(const Frame& frame) {
    if (frame.data.size() > kMaxDataLength) {
        throw std::invalid_argument("CH9329 data exceeds 64 bytes");
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(frame.data.size() + 6U);
    bytes.push_back(kHeader0);
    bytes.push_back(kHeader1);
    bytes.push_back(frame.address);
    bytes.push_back(frame.command);
    bytes.push_back(static_cast<std::uint8_t>(frame.data.size()));
    bytes.insert(bytes.end(), frame.data.begin(), frame.data.end());

    std::uint8_t checksum = 0;
    for (const auto byte : bytes) {
        checksum = static_cast<std::uint8_t>(checksum + byte);
    }
    bytes.push_back(checksum);
    return bytes;
}

Frame keyboard_report(const std::uint8_t address, const std::uint8_t modifiers,
                      const std::span<const std::uint8_t, 6> usages) {
    Frame frame{address, static_cast<std::uint8_t>(Command::keyboard),
                {modifiers, 0}};
    frame.data.insert(frame.data.end(), usages.begin(), usages.end());
    return frame;
}

Frame absolute_mouse_report(const std::uint8_t address,
                            const std::uint8_t buttons, const std::uint16_t x,
                            const std::uint16_t y, const std::int8_t wheel) {
    if (x > 4095U || y > 4095U) {
        throw std::invalid_argument("absolute mouse coordinate exceeds 4095");
    }
    return {address,
            static_cast<std::uint8_t>(Command::absolute_mouse),
            {0x02, buttons, low_byte(x), high_byte(x), low_byte(y), high_byte(y),
             signed_byte(wheel)}};
}

Frame relative_mouse_report(const std::uint8_t address,
                            const std::uint8_t buttons, const std::int8_t dx,
                            const std::int8_t dy, const std::int8_t wheel) {
    return {address,
            static_cast<std::uint8_t>(Command::relative_mouse),
            {0x01, buttons, signed_byte(dx), signed_byte(dy), signed_byte(wheel)}};
}

std::vector<Frame> Parser::feed(const std::span<const std::uint8_t> bytes) {
    std::vector<Frame> frames;
    for (const auto byte : bytes) {
        buffer_.push_back(byte);

        for (;;) {
            const std::array header{kHeader0, kHeader1};
            const auto start = std::search(buffer_.begin(), buffer_.end(),
                                           header.begin(), header.end());
            if (start == buffer_.end()) {
                const bool keep_prefix = !buffer_.empty() && buffer_.back() == kHeader0;
                buffer_.clear();
                if (keep_prefix) {
                    buffer_.push_back(kHeader0);
                }
                break;
            }
            buffer_.erase(buffer_.begin(), start);
            if (buffer_.size() < 5U) {
                break;
            }

            const auto data_length = static_cast<std::size_t>(buffer_[4]);
            if (data_length > kMaxDataLength) {
                buffer_.erase(buffer_.begin());
                continue;
            }
            const auto frame_length = data_length + 6U;
            if (buffer_.size() < frame_length) {
                break;
            }

            std::uint8_t checksum = 0;
            for (std::size_t index = 0; index + 1U < frame_length; ++index) {
                checksum = static_cast<std::uint8_t>(checksum + buffer_[index]);
            }
            if (checksum != buffer_[frame_length - 1U]) {
                buffer_.erase(buffer_.begin());
                continue;
            }

            const auto data_end = std::next(
                buffer_.begin(), static_cast<std::ptrdiff_t>(5U + data_length));
            const auto frame_end = std::next(
                buffer_.begin(), static_cast<std::ptrdiff_t>(frame_length));
            frames.push_back(Frame{
                buffer_[2], buffer_[3],
                std::vector<std::uint8_t>(std::next(buffer_.begin(), 5), data_end)});
            buffer_.erase(buffer_.begin(), frame_end);
        }
    }
    return frames;
}

}  // namespace kvmux::ch9329
