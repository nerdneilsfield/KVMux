#pragma once
#include <cstdint>
#include <span>
namespace kvmux::support {
[[nodiscard]] std::uint32_t crc32_ieee(std::span<const std::uint8_t> bytes) noexcept;
} // namespace kvmux::support
