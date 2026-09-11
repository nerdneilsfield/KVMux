#pragma once
#include "control/serial_worker.hpp"
#include "video/capture/capture_source.hpp"
#include <memory>
#include <string>
namespace kvmux::relay {
struct ServerOptions {
    std::string bind_address{"0.0.0.0"};
    std::uint16_t control_port{17000}, video_port{17001};
};
// Caller starts native MJPEG capture and connects the serial sink first.
// stop() joins all workers; capture and sink must outlive the server.
class RelayServer {
public:
    RelayServer(CaptureSource&, Ch9329ControlSink&);
    ~RelayServer();
    bool start(const ServerOptions&, std::string& error);
    void stop() noexcept;
    std::uint16_t control_port() const;
    std::uint16_t video_port() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
