#pragma once

#include <string_view>

namespace kvmux {

[[nodiscard]] std::string_view application_name() noexcept;
[[nodiscard]] std::string_view application_version() noexcept;

}  // namespace kvmux
