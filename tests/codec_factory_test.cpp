#include "video/codec/video_codec.hpp"
#include <iostream>
#include <stdexcept>
using namespace kvmux;
namespace {
void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
}
int main() {
    std::string error;
    CodecConfig config; config.width=128; config.height=128;
    auto software=create_video_encoder(CodecBackend::ffmpeg_software,error);
    const bool software_available=bool(software);
    if (software) require(software->configure(config).ok(), "software encoder configures");
    auto encoder=create_video_encoder(CodecBackend::automatic,error);
    require(bool(encoder), "automatic encoder defers selection until configure");
    auto encoded=encoder->configure(config);
#if !KVMUX_HAS_JETSON_ENCODER
    require(!create_video_encoder(CodecBackend::jetson_gstreamer,error) && !error.empty(), "explicit unavailable encoder fails");
    if (software_available) {
        require(encoded.ok(), encoded.message);
        require(encoder->backend()==CodecBackend::ffmpeg_software, "automatic encoder selects CPU");
        require(encoder->diagnostic().detail.find("fallback")!=std::string::npos, "encoder fallback reason reported");
    } else require(!encoded.ok(), "no available encoder fails clearly");
#endif
    auto decoder=create_video_decoder(CodecBackend::automatic,error);
    require(bool(decoder), "automatic decoder created");
    auto decoded=decoder->configure(config); require(decoded.ok(), decoded.message);
#if !KVMUX_HAS_VIDEOTOOLBOX
    require(!create_video_decoder(CodecBackend::videotoolbox,error) && !error.empty(), "explicit unavailable decoder fails");
    require(decoder->backend()==CodecBackend::ffmpeg_software, "automatic decoder selects CPU");
    require(decoder->diagnostic().detail.find("fallback")!=std::string::npos, "decoder fallback reason reported");
#endif
    require(!create_video_encoder(CodecBackend::videotoolbox,error), "unsupported explicit encoder fails");
    require(!create_video_decoder(CodecBackend::jetson_gstreamer,error), "unsupported explicit decoder fails");
    std::cout << "automatic selection and strict explicit backend checks passed\n";
}
