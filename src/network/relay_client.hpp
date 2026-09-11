#pragma once
#include "control/control_sink.hpp"
#include "video/codec/video_codec.hpp"
#include "video/capture/capture_source.hpp"
#include <memory>

namespace kvmux::relay {
struct ClientOptions {
    std::string host{"127.0.0.1"};
    std::uint16_t control_port{17000}, video_port{17001};
    CodecBackend decoder_backend{CodecBackend::automatic};
};
struct ClientVideoSnapshot {
    VideoCodec codec{VideoCodec::mjpeg};
    std::optional<CodecBackend> decoder_backend;
    bool hardware_active{};
    bool hardware_verified{};
    std::string decoder_diagnostic;
    std::uint64_t recoveries{};
    std::string error;
};
// Per-client lifetime totals for complete relay packets, including the 12-byte
// protocol header. Excludes TCP/IP overhead, retransmissions and partial packets.
// stop()/start() preserve totals; a new client starts at zero. An in-flight
// packet may finish after stop(), which does not wait for network workers.
struct TrafficSnapshot {
    std::uint64_t video_received_bytes{}, control_received_bytes{}, control_sent_bytes{};
};
class RelayClient {
public:
    explicit RelayClient(ClientOptions options);
    ~RelayClient();
    void start();
    void stop() noexcept;
    void release() noexcept;
    void mouse_mode(MouseMode mode);
    void active(bool active) noexcept;
    void gui_progress() noexcept;
    void video_presented(std::uint64_t sequence) noexcept;
    SubmitResult submit(ControlEvent event);
    ControlSnapshot control_snapshot() const;
    CaptureSnapshot capture_snapshot() const;
    TrafficSnapshot traffic_snapshot() const;
    ClientVideoSnapshot video_snapshot() const;
    std::optional<CaptureSample> take_sample();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class NetworkCaptureSource final : public CaptureSource {
public:
    explicit NetworkCaptureSource(std::shared_ptr<RelayClient> client) : client_(std::move(client)) {}
    std::vector<DeviceInfo> enumerate_devices() override;
    std::vector<CaptureMode> enumerate_modes(const std::string&) override;
    void start(const CaptureMode&) override { client_->start(); }
    void stop() noexcept override { client_->stop(); }
    std::optional<CaptureSample> take_latest_sample() override { return client_->take_sample(); }
    CaptureSnapshot snapshot() const override { return client_->capture_snapshot(); }
private:
    std::shared_ptr<RelayClient> client_;
};
class NetworkControlSink final : public ControlSink {
public:
    explicit NetworkControlSink(std::shared_ptr<RelayClient> client) : client_(std::move(client)) {}
    void connect(std::string, int, std::uint8_t) override { client_->start(); }
    void disconnect() noexcept override { client_->stop(); }
    void set_mouse_mode(MouseMode mode) override { client_->mouse_mode(mode); }
    void set_control_active(bool value) noexcept override { client_->active(value); }
    void update_ui_heartbeat() noexcept override { client_->gui_progress(); }
    void video_presented(std::uint64_t sequence) noexcept override { client_->video_presented(sequence); }
    SubmitResult submit(ControlEvent event) override { return client_->submit(std::move(event)); }
    void release_all() noexcept override { client_->release(); }
    ControlSnapshot snapshot() const override { return client_->control_snapshot(); }
private:
    std::shared_ptr<RelayClient> client_;
};
} // namespace kvmux::relay
