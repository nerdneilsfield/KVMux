#include "network/tcp_socket.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <utility>
#include <spdlog/spdlog.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace kvmux::tcp {
namespace {
using Clock = std::chrono::steady_clock;
#ifdef _WIN32
using NativeSocket = SOCKET;
using SocketLength = int;
constexpr NativeSocket kInvalid = INVALID_SOCKET;
int close_native(NativeSocket socket) { return closesocket(socket); }
int last_error() { return WSAGetLastError(); }
bool interrupted(int error) { return error == WSAEINTR; }
bool would_block(int error) { return error == WSAEWOULDBLOCK; }
bool connect_pending(int error) { return would_block(error) || error == WSAEINPROGRESS; }
#else
using NativeSocket = int;
using SocketLength = socklen_t;
constexpr NativeSocket kInvalid = -1;
int close_native(NativeSocket socket) { return ::close(socket); }
int last_error() { return errno; }
bool interrupted(int error) { return error == EINTR; }
bool would_block(int error) { return error == EAGAIN || error == EWOULDBLOCK; }
bool connect_pending(int error) { return error == EINPROGRESS || interrupted(error); }
#endif
NativeSocket native(std::intptr_t handle) { return static_cast<NativeSocket>(handle); }

bool wait_for(NativeSocket socket, bool write, Clock::time_point deadline, int* failure_error = nullptr) {
    if (failure_error) *failure_error = 0;
    while (true) {
        const auto now = Clock::now();
        if (now >= deadline) return false;
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
        const int milliseconds = static_cast<int>(std::min<std::int64_t>(remaining.count(), INT_MAX));
#ifdef _WIN32
        fd_set ready;
        fd_set errors;
        FD_ZERO(&ready);
        FD_ZERO(&errors);
        FD_SET(socket, &ready);
        FD_SET(socket, &errors);
        timeval tv{milliseconds / 1000, (milliseconds % 1000) * 1000};
        const int result = select(0, write ? nullptr : &ready, write ? &ready : nullptr, &errors, &tv);
#else
        pollfd fd{socket, static_cast<short>(write ? POLLOUT : POLLIN), 0};
        const int result = ::poll(&fd, 1, milliseconds);
#endif
        // Let send/recv/SO_ERROR report disconnects as well as readiness.
        if (result > 0) return true;
        if (result < 0) {
            const int error = last_error();
            if (!interrupted(error)) {
                if (failure_error) *failure_error = error;
                return false;
            }
        }
    }
}

// One record per failed operation; never log payloads or each would-block retry.
void log_io_failure(std::intptr_t socket, const char* operation, const char* reason,
                    int error, std::size_t transferred, std::size_t requested,
                    Clock::time_point started, std::chrono::milliseconds timeout) {
    spdlog::debug("TCP socket={} {} failed: reason={} error={} bytes={}/{} elapsed_us={} timeout_ms={}",
        socket, operation, reason, error, transferred, requested,
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count(), timeout.count());
}

bool set_nonblocking(NativeSocket socket) {
#ifdef _WIN32
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool prepare_socket(NativeSocket socket) {
#ifdef __APPLE__
    const int enabled = 1;
    if (setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) return false;
#endif
    return set_nonblocking(socket);
}
}  // namespace

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

Socket::~Socket() { close(); }
Socket::Socket(Socket&& other) noexcept : handle_(std::exchange(other.handle_, -1)) {}
Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, -1);
    }
    return *this;
}
bool Socket::valid() const noexcept { return native(handle_) != kInvalid; }
void Socket::close() noexcept {
    if (valid()) {
        (void)close_native(native(handle_));
        handle_ = -1;
    }
}

SendResult Socket::send_all(std::span<const std::uint8_t> bytes, std::chrono::milliseconds timeout) const {
    if (!valid()) return {};
    const auto started = Clock::now();
    const auto deadline = started + timeout;
    std::size_t written{};
    while (written < bytes.size()) {
        int wait_error{};
        if (!wait_for(native(handle_), true, deadline, &wait_error)) {
            // Zero-progress deadlines are recoverable at a packet boundary. The
            // video caller aggregates these instead of logging every dropped frame.
            if (wait_error || written != 0)
                log_io_failure(handle_, "send", wait_error ? "wait error" : "deadline", wait_error, written, bytes.size(), started, timeout);
            return {wait_error ? SendStatus::error : SendStatus::deadline, written};
        }
        const int size = static_cast<int>(std::min<std::size_t>(bytes.size() - written, INT_MAX));
#ifdef _WIN32
        const int count = ::send(native(handle_), reinterpret_cast<const char*>(bytes.data() + written), size, 0);
#else
#ifdef MSG_NOSIGNAL
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;  // macOS uses SO_NOSIGPIPE in prepare_socket.
#endif
        const auto count = ::send(native(handle_), bytes.data() + written, static_cast<std::size_t>(size), flags);
#endif
        if (count > 0) {
            written += static_cast<std::size_t>(count);
            continue;
        }
        if (count == 0) {
            log_io_failure(handle_, "send", "zero write", 0, written, bytes.size(), started, timeout);
            return {SendStatus::error, written};
        }
        const int error = last_error();
        if (!interrupted(error) && !would_block(error)) {
            log_io_failure(handle_, "send", "socket error", error, written, bytes.size(), started, timeout);
            return {SendStatus::error, written};
        }
    }
    return {SendStatus::complete, written};
}

std::optional<std::vector<std::uint8_t>> Socket::receive_exact(
    std::size_t size, std::chrono::milliseconds timeout) const {
    if (!valid()) return std::nullopt;
    const auto started = Clock::now();
    const auto deadline = started + timeout;
    std::vector<std::uint8_t> result(size);
    std::size_t received{};
    while (received < size) {
        int wait_error{};
        if (!wait_for(native(handle_), false, deadline, &wait_error)) {
            log_io_failure(handle_, "receive", wait_error ? "wait error" : "deadline", wait_error, received, size, started, timeout);
            return std::nullopt;
        }
        const int chunk = static_cast<int>(std::min<std::size_t>(size - received, INT_MAX));
#ifdef _WIN32
        const int count = ::recv(native(handle_), reinterpret_cast<char*>(result.data() + received), chunk, 0);
#else
        const auto count = ::recv(native(handle_), result.data() + received, static_cast<std::size_t>(chunk), 0);
#endif
        if (count > 0) {
            received += static_cast<std::size_t>(count);
            continue;
        }
        if (count == 0) {
            log_io_failure(handle_, "receive", "peer closed", 0, received, size, started, timeout);
            return std::nullopt;
        }
        const int error = last_error();
        if (!interrupted(error) && !would_block(error)) {
            log_io_failure(handle_, "receive", "socket error", error, received, size, started, timeout);
            return std::nullopt;
        }
    }
    return result;
}

std::optional<Listener> Listener::bind(std::string_view address, std::uint16_t port, std::string& error) {
    if (!initialize(error)) return std::nullopt;
    const NativeSocket raw = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (raw == kInvalid) {
        error = "could not create TCP listener";
        return std::nullopt;
    }
    Socket socket(static_cast<std::intptr_t>(raw));
    const int reuse = 1;
    (void)setsockopt(raw, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    const std::string address_copy(address);
    if (inet_pton(AF_INET, address_copy.c_str(), &endpoint.sin_addr) != 1 ||
        ::bind(raw, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) != 0 ||
        ::listen(raw, 1) != 0 || !prepare_socket(raw)) {
        error = "could not bind/listen on " + address_copy + ":" + std::to_string(port);
        return std::nullopt;
    }
    return Listener(std::move(socket));
}

std::optional<std::uint16_t> Listener::local_port() const {
    if (!socket_.valid()) return std::nullopt;
    sockaddr_in endpoint{};
    SocketLength size = sizeof(endpoint);
    if (getsockname(native(socket_.native_handle()), reinterpret_cast<sockaddr*>(&endpoint), &size) != 0) {
        return std::nullopt;
    }
    return ntohs(endpoint.sin_port);
}

std::optional<Socket> Listener::accept(std::chrono::milliseconds timeout) const {
    if (!socket_.valid()) return std::nullopt;
    const auto deadline = Clock::now() + timeout;
    const auto raw = native(socket_.native_handle());
    while (wait_for(raw, false, deadline)) {
        const NativeSocket accepted = ::accept(raw, nullptr, nullptr);
        if (accepted == kInvalid) {
            const int error = last_error();
            if (interrupted(error) || would_block(error)) continue;
            return std::nullopt;
        }
        Socket socket(static_cast<std::intptr_t>(accepted));
        if (!prepare_socket(accepted)) return std::nullopt;
        return socket;
    }
    return std::nullopt;
}

std::optional<Socket> connect(std::string_view host, std::uint16_t port,
                              std::chrono::milliseconds timeout, std::string& error) {
    const auto deadline = Clock::now() + timeout;
    if (!initialize(error)) return std::nullopt;
    const NativeSocket raw = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (raw == kInvalid) {
        error = "could not create TCP socket";
        return std::nullopt;
    }
    Socket socket(static_cast<std::intptr_t>(raw));
    if (!prepare_socket(raw)) {
        error = "could not configure TCP socket";
        return std::nullopt;
    }
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    const std::string host_copy(host);
    if (inet_pton(AF_INET, host_copy.c_str(), &endpoint.sin_addr) != 1) {
        error = "relay host must be an IPv4 address";
        return std::nullopt;
    }
    if (::connect(raw, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == 0) return socket;
    if (!connect_pending(last_error())) {
        error = "could not connect to relay";
        return std::nullopt;
    }
    if (!wait_for(raw, true, deadline)) {
        error = "relay connection timed out";
        return std::nullopt;
    }
    int so_error{};
    SocketLength so_size = sizeof(so_error);
    if (getsockopt(raw, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &so_size) != 0 || so_error != 0) {
        error = "could not connect to relay";
        return std::nullopt;
    }
    return socket;
}
}  // namespace kvmux::tcp
