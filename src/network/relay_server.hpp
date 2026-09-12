#pragma once
#include "control/serial_worker.hpp"
#include "video/capture/capture_source.hpp"
#include <memory>
#include <functional>
#include "video/codec/video_codec.hpp"
#include <string>
namespace kvmux::relay {
struct ServerOptions {
    std::string bind_address{"0.0.0.0"};
    std::uint16_t control_port{17000}, video_port{17001};
    VideoCodec codec{VideoCodec::mjpeg};
    CodecBackend encoder_backend{CodecBackend::automatic};
    std::uint32_t bitrate{8'000'000};
    // UDP envelope, media payload and XOR parity; excludes UDP/IP headers.
    std::uint64_t transport_bytes_per_second{12'000'000};
};
// Caller starts native capture and connects the serial sink first.
// stop() joins all workers; capture and sink must outlive the server.
class RelayServer {
public:
    using EncoderFactory = std::function<std::unique_ptr<VideoEncoder>(CodecBackend, std::string&)>;
    RelayServer(CaptureSource&, Ch9329ControlSink&, EncoderFactory = create_video_encoder);
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
