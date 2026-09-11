#include "video/codec/video_codec.hpp"
#include "video/codec/ffmpeg_decoder.hpp"
#include "video/codec/ffmpeg_encoder.hpp"
#if KVMUX_HAS_JETSON_ENCODER
#include "video/codec/jetson_encoder.hpp"
#endif

namespace kvmux {
namespace {
// Selection can change at configure or before the first accepted input.
// Never replace a live reference chain; the caller owns recovery and generation.
template<class Interface, class Input, class Output>
class AutomaticCodec : public Interface {
public:
    using Factory = std::unique_ptr<Interface> (*)(CodecBackend, std::string&);
    AutomaticCodec(Factory factory, CodecBackend hardware) : factory_(factory), hardware_(hardware) {}
    CodecResult configure(const CodecConfig& config) override {
        shutdown();
        config_ = config;
        first_submit_ = true;
        reason_.clear();
        std::string error;
        selected_ = factory_(hardware_, error);
        if (selected_) {
            const auto result = selected_->configure(config);
            if (result.ok()) return result;
            error = result.message;
            selected_->shutdown();
            selected_.reset();
        }
        reason_ = "Automatic CPU fallback: " + error;
        selected_ = factory_(CodecBackend::ffmpeg_software, error);
        if (selected_) {
            const auto result = selected_->configure(config);
            if (result.ok()) return result;
            error = result.message;
            selected_->shutdown();
            selected_.reset();
        }
        reason_ += "; software unavailable: " + error;
        return {CodecStatus::unsupported, reason_};
    }
    CodecResult submit(const Input& input) override {
        if (!selected_) return unavailable();
        auto result = selected_->submit(input);
        // Some hardware sessions open only when the first IDR/raw frame arrives.
        // No earlier input was accepted, so retrying this one on CPU is safe.
        if (first_submit_ && selected_->backend() == hardware_ &&
            (result.status == CodecStatus::failed || result.status == CodecStatus::unsupported)) {
            reason_ = "Automatic CPU fallback on first input: " + result.message;
            selected_->shutdown();
            std::string error;
            selected_ = factory_(CodecBackend::ffmpeg_software, error);
            if (!selected_) {
                reason_ += "; software unavailable: " + error;
                return {CodecStatus::unsupported, reason_};
            }
            result = selected_->configure(config_);
            if (!result.ok()) {
                reason_ += "; software configuration failed: " + result.message;
                selected_->shutdown();
                selected_.reset();
                return {result.status, reason_};
            }
            result = selected_->submit(input);
        }
        if (result.ok()) first_submit_ = false;
        return result;
    }
    CodecResult poll(Output& output) override {
        return selected_ ? selected_->poll(output) : unavailable();
    }
    CodecResult finish() override { return selected_ ? selected_->finish() : unavailable(); }
    CodecResult reset() override {
        if (!selected_) return unavailable();
        auto result = selected_->reset();
        if (result.ok()) first_submit_ = true;
        return result;
    }
    void shutdown() noexcept override {
        if (selected_) selected_->shutdown();
        selected_.reset();
    }
    CodecBackend backend() const noexcept override {
        return selected_ ? selected_->backend() : CodecBackend::automatic;
    }
    CodecDiagnostic diagnostic() const override {
        auto result = selected_ ? selected_->diagnostic() : CodecDiagnostic{};
        if (!reason_.empty()) result.detail = reason_ + "; " + result.detail;
        return result;
    }
protected:
    CodecResult unavailable() const { return {CodecStatus::failed, "Automatic codec is not configured"}; }
    std::unique_ptr<Interface> selected_;
private:
    Factory factory_;
    CodecBackend hardware_;
    CodecConfig config_;
    bool first_submit_{true};
    std::string reason_;
};
class AutomaticEncoder final : public AutomaticCodec<VideoEncoder, EncoderInput, EncodedAccessUnit> {
public:
    AutomaticEncoder() : AutomaticCodec(create_video_encoder, CodecBackend::jetson_gstreamer) {}
    CodecResult request_keyframe() override {
        return selected_ ? selected_->request_keyframe() : unavailable();
    }
};
using AutomaticDecoder = AutomaticCodec<VideoDecoder, EncodedAccessUnit, VideoFrame>;
}  // namespace

std::unique_ptr<VideoEncoder> create_video_encoder(CodecBackend backend, std::string& error) {
    error.clear();
    if (backend == CodecBackend::automatic) return std::make_unique<AutomaticEncoder>();
    if (backend == CodecBackend::ffmpeg_software) return create_ffmpeg_encoder(backend, error);
    if (backend == CodecBackend::jetson_gstreamer) {
#if KVMUX_HAS_JETSON_ENCODER
        return create_jetson_encoder(error);
#else
        error = "Jetson encoder unavailable: build with KVMUX_JETSON_ENCODER=ON on Linux with GStreamer core/app/video development packages";
        return {};
#endif
    }
    error = "Unsupported encoder backend";
    return {};
}

std::unique_ptr<VideoDecoder> create_video_decoder(CodecBackend backend, std::string& error) {
    error.clear();
    if (backend == CodecBackend::automatic)
        return std::make_unique<AutomaticDecoder>(create_video_decoder, CodecBackend::videotoolbox);
    if (backend == CodecBackend::ffmpeg_software)
        return create_ffmpeg_decoder(backend, error);
    if (backend == CodecBackend::videotoolbox) {
#if KVMUX_HAS_VIDEOTOOLBOX
        return create_ffmpeg_decoder(CodecBackend::videotoolbox, error);
#else
        error = "VideoToolbox decoder unavailable: build with KVMUX_VIDEOTOOLBOX=ON on macOS";
        return {};
#endif
    }
    error = "Unsupported decoder backend";
    return {};
}
}  // namespace kvmux
