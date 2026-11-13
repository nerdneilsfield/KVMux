#pragma once

#include <spdlog/spdlog.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "control/ch9329_protocol.hpp"

namespace kvmux {

// OUT logs stay at length plus the CH9329 command byte. The bytes after that
// command are keyboard reports and ASCII paste, so they are never printed.
inline void log_serial_out(const char* backend,
                           const std::span<const std::uint8_t> bytes) {
  if (!spdlog::should_log(spdlog::level::debug)) {
    return;
  }
  std::string commands;
  std::size_t index = 0;
  while (index < bytes.size() && bytes.size() - index >= 4 &&
         bytes[index] == ch9329::kHeader0 &&
         bytes[index + 1] == ch9329::kHeader1) {
    constexpr char kHex[] = "0123456789abcdef";
    const auto command = bytes[index + 3];
    if (!commands.empty()) {
      commands.push_back(',');
    }
    commands.append("0x");
    commands.push_back(kHex[command >> 4U]);
    commands.push_back(kHex[command & 0x0fU]);
    if (bytes.size() - index < 5) {
      break;
    }
    const auto frame = static_cast<std::size_t>(bytes[index + 4]) + 6U;
    if (bytes.size() - index < frame) {
      break;
    }
    index += frame;
  }
  if (commands.empty()) {
    commands = "none";
  }
  spdlog::debug("{} out: length={} cmd={} partial={}", backend, bytes.size(),
                commands, bytes.size() - index);
}

inline void log_serial_in(const char* backend,
                          const std::span<const std::uint8_t> bytes) {
  if (!spdlog::should_log(spdlog::level::debug) || bytes.empty()) {
    return;
  }
  constexpr std::size_t kLimit = 64;
  const auto shown = bytes.size() < kLimit ? bytes.size() : kLimit;
  std::string hex;
  hex.reserve(shown * 3U);
  constexpr char kHex[] = "0123456789abcdef";
  for (std::size_t i = 0; i < shown; ++i) {
    if (i != 0) {
      hex.push_back(' ');
    }
    hex.push_back(kHex[bytes[i] >> 4U]);
    hex.push_back(kHex[bytes[i] & 0x0fU]);
  }
  spdlog::debug("{} in: length={} shown={} hex={}", backend, bytes.size(),
                shown, hex);
}

}  // namespace kvmux
