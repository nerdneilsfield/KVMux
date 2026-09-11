#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kvmux::tcp {

enum class SendStatus { complete, deadline, error };
struct SendResult {
    SendStatus status{SendStatus::error};
    std::size_t bytes_sent{};
    operator bool() const noexcept { return status == SendStatus::complete; }
    [[nodiscard]] bool unsent_deadline() const noexcept {
        return status == SendStatus::deadline && bytes_sent == 0;
    }
};

class Socket {
public:
    Socket() = default;
    explicit Socket(std::intptr_t handle) noexcept : handle_(handle) {}
    ~Socket();
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::intptr_t native_handle() const noexcept { return handle_; }
    void close() noexcept;
    [[nodiscard]] SendResult send_all(std::span<const std::uint8_t> bytes,
                                std::chrono::milliseconds timeout = std::chrono::seconds(2)) const;
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> receive_exact(
        std::size_t size, std::chrono::milliseconds timeout) const;
private:
    std::intptr_t handle_{-1};
};

class Listener {
public:
    Listener() = default;
    ~Listener() = default;
    Listener(Listener&&) noexcept = default;
    Listener& operator=(Listener&&) noexcept = default;
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;
    [[nodiscard]] static std::optional<Listener> bind(std::string_view address, std::uint16_t port,
                                                       std::string& error);
    [[nodiscard]] std::optional<Socket> accept(std::chrono::milliseconds timeout) const;
    [[nodiscard]] std::optional<std::uint16_t> local_port() const;
private:
    explicit Listener(Socket socket) noexcept : socket_(std::move(socket)) {}
    Socket socket_;
};

[[nodiscard]] std::optional<Socket> connect(std::string_view host, std::uint16_t port,
                                            std::chrono::milliseconds timeout, std::string& error);
[[nodiscard]] bool initialize(std::string& error);

}  // namespace kvmux::tcp
