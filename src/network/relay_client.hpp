#pragma once
#include "control/control_sink.hpp"
#include "network/udp_media.hpp"
#include "network/relay_wire.hpp"
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
    MediaStats media{};
    MediaReason last_recovery_reason{MediaReason::none};
    std::string error;
};
// Per-client lifetime totals for accepted UDP datagrams, including the 32-byte
// envelope and retransmissions. Excludes UDP/IP headers.
// stop()/start() preserve totals; a new client starts at zero. An in-flight
// packet may finish after stop(), which does not wait for network workers.
enum class PasteUploadState {
    idle, uploading, uploaded, preparing, executing,
    completed, canceled, rejected, expired
};
struct PasteUploadSnapshot {
    PasteUploadState state{PasteUploadState::idle};
    std::uint64_t transaction_id{};
    std::uint32_t total_bytes{}, accepted_bytes{}, completed_bytes{};
    wire::PasteStatusReason reason{wire::PasteStatusReason::none};
    [[nodiscard]] bool terminal() const noexcept { return state == PasteUploadState::completed || state == PasteUploadState::canceled || state == PasteUploadState::rejected || state == PasteUploadState::expired; }
};
// Merges coalesced server feedback without allowing a transaction to regress.
PasteUploadSnapshot merge_paste_status(PasteUploadSnapshot current, const wire::PasteStatus& status) noexcept;
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
    void video_presented(std::uint64_t generation, std::uint64_t sequence) noexcept;
    kvmux::SubmitResult synchronize(InputSync);
    kvmux::SubmitResult submit(ControlEvent event);
    // Takes already normalized US-ASCII transaction bytes. It never logs or retains them after completion/cancel.
    kvmux::SubmitResult start_ascii_paste_text(std::vector<std::uint8_t> normalized);
    void cancel_ascii_paste_text() noexcept;
    PasteUploadSnapshot ascii_paste_text_snapshot() const;
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
    void video_presented(std::uint64_t generation, std::uint64_t sequence) noexcept override { client_->video_presented(generation, sequence); }
    kvmux::SubmitResult synchronize(InputSync value) override { return client_->synchronize(std::move(value)); }
    kvmux::SubmitResult submit(ControlEvent event) override { return client_->submit(std::move(event)); }
    void release_all() noexcept override { client_->release(); }
    kvmux::SubmitResult start_ascii_paste_text(std::vector<std::uint8_t> normalized) override { return client_->start_ascii_paste_text(std::move(normalized)); }
    void cancel_ascii_paste() noexcept override { client_->cancel_ascii_paste_text(); }
    AsciiPasteSnapshot ascii_paste_snapshot() const override;
    ControlSnapshot snapshot() const override { return client_->control_snapshot(); }
private:
    std::shared_ptr<RelayClient> client_;
};
} // namespace kvmux::relay
