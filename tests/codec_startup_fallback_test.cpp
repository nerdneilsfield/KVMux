#include "video/codec/video_codec.hpp"
#include <stdexcept>
using namespace kvmux;
namespace {
int hardware_configures{}, software_submits{};
class Decoder final : public VideoDecoder {
public:
    explicit Decoder(CodecBackend value) : value_(value) {}
    CodecResult configure(const CodecConfig&) override { if (value_==CodecBackend::videotoolbox) ++hardware_configures; return {}; }
    CodecResult submit(const EncodedAccessUnit&) override {
        if (value_==CodecBackend::videotoolbox) return {CodecStatus::unsupported,"injected first IDR hardware rejection"};
        ++software_submits; return {};
    }
    CodecResult poll(VideoFrame&) override { return {CodecStatus::again,{}}; }
    CodecResult finish() override { return {}; }
    CodecResult reset() override { return {}; }
    void shutdown() noexcept override {}
    CodecBackend backend() const noexcept override { return value_; }
    CodecDiagnostic diagnostic() const override { return {value_,false,false,"test decoder"}; }
private: CodecBackend value_;
};
void require(bool value) { if (!value) throw std::runtime_error("startup fallback check failed"); }
}
namespace kvmux {
std::unique_ptr<VideoDecoder> create_ffmpeg_decoder(CodecBackend backend, std::string&) {
    return std::make_unique<Decoder>(backend);
}
std::unique_ptr<VideoEncoder> create_ffmpeg_encoder(CodecBackend, std::string& error) {
    error="unused test encoder"; return {};
}
}
int main() {
    std::string error;
    CodecConfig config; config.width=128; config.height=128;
    EncodedAccessUnit au; au.idr=true;
    auto decoder=create_video_decoder(CodecBackend::automatic,error);
    require(decoder->configure(config).ok());
    require(decoder->backend()==CodecBackend::videotoolbox);
    require(decoder->submit(au).ok());
    require(hardware_configures==1 && software_submits==1);
    require(decoder->backend()==CodecBackend::ffmpeg_software);
    require(decoder->diagnostic().detail.find("injected first IDR")!=std::string::npos);
    auto strict=create_video_decoder(CodecBackend::videotoolbox,error);
    require(strict->configure(config).ok());
    require(strict->submit(au).status==CodecStatus::unsupported);
    require(software_submits==1);
}
