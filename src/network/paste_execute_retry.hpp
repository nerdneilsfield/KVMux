#pragma once

#include <chrono>
#include <cstdint>

namespace kvmux::relay {

// Tracks retries for a PasteExecute that could not enter KCP's bounded window.
// The caller records a successful submission in accepted_challenge.
struct PasteExecuteRetry {
    std::uint64_t accepted_challenge{};
    std::uint64_t attempted_challenge{};
    std::chrono::steady_clock::time_point attempted_at{};
};

inline void accept_paste_execute(PasteExecuteRetry& retry, std::uint64_t challenge) noexcept {
    retry.accepted_challenge = challenge;
}

inline bool should_retry_paste_execute(PasteExecuteRetry& retry,
                                       std::uint64_t challenge,
                                       std::chrono::steady_clock::time_point now) noexcept {
    using namespace std::chrono_literals;
    if (challenge <= retry.accepted_challenge ||
        (challenge == retry.attempted_challenge && now - retry.attempted_at < 10ms)) return false;
    retry.attempted_challenge = challenge;
    retry.attempted_at = now;
    return true;
}

} // namespace kvmux::relay
