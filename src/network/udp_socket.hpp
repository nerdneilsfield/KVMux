#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kvmux::udp {
inline constexpr std::size_t max_payload = 1200;

// Owned IPv4 address. Native socket types do not escape this adapter.
class Endpoint {
 public:
  Endpoint() = default;
  [[nodiscard]] bool operator==(const Endpoint& other) const noexcept;

 private:
  struct Address;
  std::shared_ptr<const Address> address_;
  friend class Socket;
  friend std::optional<Endpoint> resolve(std::string_view, std::uint16_t,
                                         std::string&);
};
[[nodiscard]] std::optional<Endpoint> resolve(std::string_view host,
                                              std::uint16_t port,
                                              std::string& error);
struct Datagram {
  Endpoint source;
  std::vector<std::uint8_t> bytes;
};
enum class ReceiveStatus { datagram, idle, oversized, error };
struct ReceiveResult {
  ReceiveStatus status{ReceiveStatus::error};
  Datagram datagram;
};
enum class SendStatus { sent, would_block, invalid, error };

class Socket {
 public:
  Socket() = default;
  ~Socket();
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  [[nodiscard]] static std::optional<Socket> bind(std::string_view host,
                                                  std::uint16_t port,
                                                  std::string& error);
  [[nodiscard]] std::optional<std::uint16_t> local_port() const;
  [[nodiscard]] SendStatus send_to(const Endpoint&,
                                   std::span<const std::uint8_t>) const;
  [[nodiscard]] ReceiveResult receive(std::chrono::milliseconds timeout) const;
  void close() noexcept;

 private:
  explicit Socket(std::intptr_t handle) : handle_(handle) {}
  std::intptr_t handle_{-1};
};
}  // namespace kvmux::udp
