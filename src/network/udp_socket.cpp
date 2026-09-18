#include "network/udp_socket.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace kvmux::udp {
namespace {
#ifdef _WIN32
using NativeSocket = SOCKET;
using SocketLength = int;
using BufferLength = int;
constexpr auto invalid_socket = INVALID_SOCKET;
int last_error() { return WSAGetLastError(); }
bool interrupted(int error) { return error == WSAEINTR; }
bool would_block(int error) { return error == WSAEWOULDBLOCK; }
void close_native(NativeSocket socket) { closesocket(socket); }
bool nonblocking(NativeSocket socket) {
  u_long enabled = 1;
  return ioctlsocket(socket, FIONBIO, &enabled) == 0;
}
#else
using NativeSocket = int;
using SocketLength = socklen_t;
using BufferLength = std::size_t;
constexpr auto invalid_socket = -1;
int last_error() { return errno; }
bool interrupted(int error) { return error == EINTR; }
bool would_block(int error) { return error == EAGAIN || error == EWOULDBLOCK; }
void close_native(NativeSocket socket) { ::close(socket); }
bool nonblocking(NativeSocket socket) {
  const int flags = fcntl(socket, F_GETFL, 0);
  return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}
#endif
NativeSocket native(std::intptr_t handle) {
  return static_cast<NativeSocket>(handle);
}
bool initialize(std::string& error) {
  error.clear();
#ifdef _WIN32
  static const bool ready = [] {
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  if (!ready) error = "WSAStartup failed";
#endif
  return error.empty();
}
}  // namespace

struct Endpoint::Address {
  sockaddr_in value{};
};
bool Endpoint::operator==(const Endpoint& other) const noexcept {
  if (!address_ || !other.address_) return address_ == other.address_;
  return address_->value.sin_addr.s_addr ==
             other.address_->value.sin_addr.s_addr &&
         address_->value.sin_port == other.address_->value.sin_port;
}
std::optional<Endpoint> resolve(std::string_view host, std::uint16_t port,
                                std::string& error) {
  if (!initialize(error)) return std::nullopt;
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;
  addrinfo* addresses{};
  const std::string name(host);
  const auto service = std::to_string(port);
  if (getaddrinfo(name.c_str(), service.c_str(), &hints, &addresses) != 0 ||
      !addresses) {
    error = "could not resolve UDP IPv4 endpoint";
    return std::nullopt;
  }
  auto address = std::make_shared<Endpoint::Address>();
  address->value = *reinterpret_cast<const sockaddr_in*>(addresses->ai_addr);
  freeaddrinfo(addresses);
  Endpoint endpoint;
  endpoint.address_ = std::move(address);
  return endpoint;
}
Socket::~Socket() { close(); }
Socket::Socket(Socket&& other) noexcept
    : handle_(std::exchange(other.handle_, -1)) {}
Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = std::exchange(other.handle_, -1);
  }
  return *this;
}
void Socket::close() noexcept {
  if (native(handle_) != invalid_socket) close_native(native(handle_));
  handle_ = -1;
}
std::optional<Socket> Socket::bind(std::string_view host, std::uint16_t port,
                                   std::string& error) {
  const auto endpoint = resolve(host, port, error);
  if (!endpoint) return std::nullopt;
  const auto raw = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (raw == invalid_socket) {
    error = "could not create UDP socket";
    return std::nullopt;
  }
  Socket socket(static_cast<std::intptr_t>(raw));
  if (!nonblocking(raw) ||
      ::bind(raw, reinterpret_cast<const sockaddr*>(&endpoint->address_->value),
             sizeof(sockaddr_in)) != 0) {
    error = "could not configure/bind UDP socket";
    return std::nullopt;
  }
  return socket;
}
std::optional<std::uint16_t> Socket::local_port() const {
  sockaddr_in address{};
  SocketLength length = sizeof(address);
  if (native(handle_) == invalid_socket ||
      getsockname(native(handle_), reinterpret_cast<sockaddr*>(&address),
                  &length) != 0)
    return std::nullopt;
  return ntohs(address.sin_port);
}
SendStatus Socket::send_to(const Endpoint& endpoint,
                           std::span<const std::uint8_t> bytes) const {
  if (!endpoint.address_ || bytes.size() > max_payload)
    return SendStatus::invalid;
  if (native(handle_) == invalid_socket) return SendStatus::error;
  // Nonblocking: a caller may drop an unsent datagram; never retry a partial
  // one.
  const auto count =
      ::sendto(native(handle_), reinterpret_cast<const char*>(bytes.data()),
               static_cast<BufferLength>(bytes.size()), 0,
               reinterpret_cast<const sockaddr*>(&endpoint.address_->value),
               sizeof(sockaddr_in));
  if (count >= 0 && static_cast<std::size_t>(count) == bytes.size())
    return SendStatus::sent;
  if (count < 0 && (would_block(last_error()) || interrupted(last_error())))
    return SendStatus::would_block;
  return SendStatus::error;
}
ReceiveResult Socket::receive(std::chrono::milliseconds timeout) const {
  if (native(handle_) == invalid_socket) return {};
  using Clock = std::chrono::steady_clock;
  const auto deadline =
      Clock::now() + std::max(timeout, std::chrono::milliseconds::zero());
  for (;;) {
    const auto remaining =
        std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
    const auto milliseconds = static_cast<int>(
        std::clamp<std::int64_t>(remaining.count(), 0, INT_MAX));
#ifdef _WIN32
    fd_set ready;
    FD_ZERO(&ready);
    FD_SET(native(handle_), &ready);
    timeval tv{milliseconds / 1000, (milliseconds % 1000) * 1000};
    const int result = select(0, &ready, nullptr, nullptr, &tv);
#else
    pollfd fd{native(handle_), POLLIN, 0};
    const int result = ::poll(&fd, 1, milliseconds);
#endif
    if (result == 0) return {ReceiveStatus::idle, {}};
    if (result < 0) {
      if (!interrupted(last_error())) return {};
    } else {
      // One spare byte identifies over-limit datagrams even if recvfrom
      // truncates.
      std::array<std::uint8_t, max_payload + 1> bytes{};
      auto address = std::make_shared<Endpoint::Address>();
      SocketLength length = sizeof(address->value);
      const auto count =
          ::recvfrom(native(handle_), reinterpret_cast<char*>(bytes.data()),
                     static_cast<BufferLength>(bytes.size()), 0,
                     reinterpret_cast<sockaddr*>(&address->value), &length);
#ifdef _WIN32
      if (count < 0 && last_error() == WSAEMSGSIZE)
        return {ReceiveStatus::oversized, {}};
#endif
      if (count > static_cast<int>(max_payload))
        return {ReceiveStatus::oversized, {}};
      if (count >= 0) {
        Endpoint source;
        source.address_ = std::move(address);
        return {ReceiveStatus::datagram,
                {std::move(source), {bytes.begin(), bytes.begin() + count}}};
      }
      if (!would_block(last_error()) && !interrupted(last_error())) return {};
    }
    if (Clock::now() >= deadline) return {ReceiveStatus::idle, {}};
  }
}
}  // namespace kvmux::udp
