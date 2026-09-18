#pragma once
#include <functional>
#include <memory>
#include <string>

#include "control/serial_worker.hpp"
#include "network/udp_media.hpp"
#include "video/capture/capture_source.hpp"
#include "video/codec/video_codec.hpp"
namespace kvmux::relay {
struct ServerOptions {
  std::string bind_address{"0.0.0.0"};
  std::uint16_t control_port{17000}, video_port{17001};
  VideoCodec codec{VideoCodec::mjpeg};
  CodecBackend encoder_backend{CodecBackend::automatic};
  EncodingPriority priority{EncodingPriority::quality};
  std::uint32_t bitrate{40'000'000};
  // UDP envelope, media payload and XOR parity; excludes UDP/IP headers.
  std::uint64_t transport_bytes_per_second{12'000'000};
};
struct ServerSnapshot {
  std::uint64_t generation{}, transport_bytes_per_second{}, feedback_samples{},
      source_admissions{};
  std::chrono::microseconds nominal_interval{}, admission_interval{};
  MediaStats feedback{}, feedback_delta{};
  MediaPacerStats
      pacer{};  // sent_bytes means paced bytes, not confirmed socket delivery.
  MediaReason last_reason{MediaReason::none};
};
// Caller starts native capture and connects the serial sink first.
// stop() joins all workers; capture and sink must outlive the server.
class RelayServer {
 public:
  using EncoderFactory =
      std::function<std::unique_ptr<VideoEncoder>(CodecBackend, std::string&)>;
  RelayServer(CaptureSource&, Ch9329ControlSink&,
              EncoderFactory = create_video_encoder);
  ~RelayServer();
  bool start(const ServerOptions&, std::string& error);
  void stop() noexcept;
  std::uint16_t control_port() const;
  std::uint16_t video_port() const;
  ServerSnapshot snapshot() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace kvmux::relay
