#include "support/crc32.hpp"
namespace kvmux::support {
std::uint32_t crc32_ieee(std::span<const std::uint8_t> bytes) noexcept {
  std::uint32_t crc = 0xffffffffU;
  for (auto byte : bytes) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1U) ? 0xedb88320U : 0U);
  }
  return ~crc;
}
}  // namespace kvmux::support
