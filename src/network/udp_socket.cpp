#include "network/udp_socket.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
// Order matters: ws2tcpip.h and mswsock.h need winsock2.h first, and mswsock.h
// is what provides SIO_UDP_CONNRESET.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
// clang-format on
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

void request_socket_buffers(NativeSocket raw) {
  const int bytes = 4 * 1024 * 1024;
#ifdef _WIN32
  const auto* value = reinterpret_cast<const char*>(&bytes);
  const int length = static_cast<int>(sizeof(bytes));
#else
  const void* value = &bytes;
  const socklen_t length = sizeof(bytes);
#endif
  if (::setsockopt(raw, SOL_SOCKET, SO_RCVBUF, value, length) != 0)
    spdlog::warn("UDP SO_RCVBUF was not set");
  if (::setsockopt(raw, SOL_SOCKET, SO_SNDBUF, value, length) != 0)
    spdlog::warn("UDP SO_SNDBUF was not set");
}
bool Endpoint::operator==(const Endpoint& other) const noexcept {
  if (!has_address_ || !other.has_address_)
    return has_address_ == other.has_address_;
  return address_ == other.address_ && port_ == other.port_;
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
  const auto* resolved =
      reinterpret_cast<const sockaddr_in*>(addresses->ai_addr);
  Endpoint endpoint;
  endpoint.has_address_ = true;
  endpoint.address_ = resolved->sin_addr.s_addr;
  endpoint.port_ = resolved->sin_port;
  freeaddrinfo(addresses);
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
#ifdef _WIN32
  // A departed UDP peer must not terminate the shared relay listener. Session
  // liveness and release are governed by the protocol's existing deadlines.
  BOOL report_port_unreachable = FALSE;
  DWORD returned = 0;
  if (WSAIoctl(raw, SIO_UDP_CONNRESET, &report_port_unreachable,
               sizeof(report_port_unreachable), nullptr, 0, &returned, nullptr,
               nullptr) == SOCKET_ERROR) {
    error = "could not configure UDP ICMP handling";
    return std::nullopt;
  }
#endif
  sockaddr_in bound{};
  bound.sin_family = AF_INET;
  bound.sin_addr.s_addr = endpoint->address_;
  bound.sin_port = endpoint->port_;
  if (!nonblocking(raw) ||
      ::bind(raw, reinterpret_cast<const sockaddr*>(&bound), sizeof(bound)) !=
          0) {
    error = "could not configure/bind UDP socket";
    return std::nullopt;
  }
  request_socket_buffers(raw);
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
  if (!endpoint.has_address_ || bytes.size() > max_payload)
    return SendStatus::invalid;
  if (native(handle_) == invalid_socket) return SendStatus::error;
  // Nonblocking: a caller may drop an unsent datagram; never retry a partial
  // one.
  sockaddr_in destination{};
  destination.sin_family = AF_INET;
  destination.sin_addr.s_addr = endpoint.address_;
  destination.sin_port = endpoint.port_;
  const auto count = ::sendto(
      native(handle_), reinterpret_cast<const char*>(bytes.data()),
      static_cast<BufferLength>(bytes.size()), 0,
      reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
  if (count >= 0 && static_cast<std::size_t>(count) == bytes.size())
    return SendStatus::sent;
  if (count < 0 && (would_block(last_error()) || interrupted(last_error())))
    return SendStatus::would_block;
  return SendStatus::error;
}
namespace {
#ifdef _WIN32
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
// select() sleeps in whole scheduler ticks (15.6 ms unless the process raised
// the timer), so a wait shorter than 1 ms goes through a high-resolution
// waitable timer instead. Returns false when no such timer exists (before
// Windows 10 1803) so the caller falls back to select().
bool sleep_precisely(std::chrono::nanoseconds duration) {
  struct Timer {
    HANDLE handle = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);
    ~Timer() {
      if (handle) CloseHandle(handle);
    }
  };
  thread_local const Timer timer;
  if (!timer.handle) return false;
  LARGE_INTEGER due;
  due.QuadPart = -std::max<std::int64_t>(1, duration.count() / 100);
  return SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE) &&
         WaitForSingleObject(timer.handle, 100) == WAIT_OBJECT_0;
}
int select_readable(NativeSocket socket, std::chrono::microseconds timeout) {
  fd_set ready;
  FD_ZERO(&ready);
  FD_SET(socket, &ready);
  const auto micros = static_cast<long long>(timeout.count());
  timeval tv{static_cast<long>(micros / 1000000),
             static_cast<long>(micros % 1000000)};
  return select(0, &ready, nullptr, nullptr, &tv);
}
// A positive result means readable, zero means the wait timed out.
int wait_readable(NativeSocket socket, std::chrono::nanoseconds remaining) {
  if (remaining < std::chrono::milliseconds(1)) {
    const int ready = select_readable(socket, {});
    if (ready != 0 || remaining <= std::chrono::nanoseconds::zero())
      return ready;
    // Not interruptible by a datagram, but the wait is under 1 ms.
    if (sleep_precisely(remaining)) return select_readable(socket, {});
  }
  return select_readable(
      socket, std::chrono::ceil<std::chrono::microseconds>(remaining));
}
#elif defined(__linux__)
int wait_readable(NativeSocket socket, std::chrono::nanoseconds remaining) {
  pollfd fd{socket, POLLIN, 0};
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
      std::min<std::chrono::nanoseconds>(remaining,
                                         std::chrono::hours(24 * 365)));
  timespec limit{
      static_cast<time_t>(seconds.count()),
      static_cast<long>(
          (remaining -
           std::chrono::duration_cast<std::chrono::nanoseconds>(seconds))
              .count())};
  return ::ppoll(&fd, 1, &limit, nullptr);
}
#else
int wait_readable(NativeSocket socket, std::chrono::nanoseconds remaining) {
  pollfd fd{socket, POLLIN, 0};
  const auto milliseconds = static_cast<int>(std::clamp<std::int64_t>(
      std::chrono::ceil<std::chrono::milliseconds>(remaining).count(), 0,
      INT_MAX));
  return ::poll(&fd, 1, milliseconds);
}
#endif
}  // namespace
ReceiveResult Socket::receive(std::chrono::nanoseconds timeout) const {
  if (native(handle_) == invalid_socket) return {};
  using Clock = std::chrono::steady_clock;
  const auto deadline =
      Clock::now() + std::max(timeout, std::chrono::nanoseconds::zero());
  for (;;) {
    const auto remaining =
        std::max(deadline - Clock::now(), Clock::duration::zero());
    const int result = wait_readable(native(handle_), remaining);
    if (result == 0) return {ReceiveStatus::idle, {}};
    if (result < 0) {
      if (!interrupted(last_error())) return {};
    } else {
      // One spare byte identifies over-limit datagrams even if recvfrom
      // truncates.
      std::array<std::uint8_t, max_payload + 1> bytes{};
      sockaddr_in address{};
      SocketLength length = sizeof(address);
      const auto count =
          ::recvfrom(native(handle_), reinterpret_cast<char*>(bytes.data()),
                     static_cast<BufferLength>(bytes.size()), 0,
                     reinterpret_cast<sockaddr*>(&address), &length);
#ifdef _WIN32
      if (count < 0 && last_error() == WSAEMSGSIZE)
        return {ReceiveStatus::oversized, {}};
#endif
      if (count > static_cast<int>(max_payload))
        return {ReceiveStatus::oversized, {}};
      if (count >= 0) {
        Endpoint source;
        source.has_address_ = true;
        source.address_ = address.sin_addr.s_addr;
        source.port_ = address.sin_port;
        return {ReceiveStatus::datagram,
                {source, {bytes.begin(), bytes.begin() + count}}};
      }
      if (!would_block(last_error()) && !interrupted(last_error())) return {};
    }
    if (Clock::now() >= deadline) return {ReceiveStatus::idle, {}};
  }
}
}  // namespace kvmux::udp
