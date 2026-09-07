#pragma once

#include <chrono>
#include <cstdint>
#include <variant>

namespace kvmux {

struct KeyEdge { std::uint8_t usage{}; bool pressed{}; };
struct AbsoluteMotion { double x{}; double y{}; };
struct RelativeMotion { double dx{}; double dy{}; };
struct ButtonEdge { std::uint8_t button{}; bool pressed{}; double x{}; double y{}; };
struct VerticalWheel { double steps{}; double x{}; double y{}; };
using ControlPayload = std::variant<KeyEdge, AbsoluteMotion, RelativeMotion,
                                    ButtonEdge, VerticalWheel>;

struct ControlEvent {
    std::uint64_t epoch{};
    std::uint64_t sequence{};
    std::chrono::steady_clock::time_point timestamp{};
    ControlPayload payload;
};

enum class SubmitResult { accepted, not_ready, overloaded };

}  // namespace kvmux
