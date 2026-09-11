#include "video/codec/video_codec.hpp"
#include "video/codec/ffmpeg_decoder.hpp"
#if KVMUX_HAS_JETSON_ENCODER
#include "video/codec/jetson_encoder.hpp"
#endif

namespace kvmux {
std::unique_ptr<VideoEncoder> create_video_encoder(CodecBackend backend, std::string& error) {
    error.clear();
    if (backend == CodecBackend::automatic || backend == CodecBackend::jetson_gstreamer) {
#if KVMUX_HAS_JETSON_ENCODER
        return create_jetson_encoder(error);
#else
        error = "Jetson encoder unavailable: build with KVMUX_JETSON_ENCODER=ON on Linux with GStreamer core/app development packages";
        return {};
#endif
    }
    error = "Unsupported encoder backend: only Jetson GStreamer encoding is supported";
    return {};
}

std::unique_ptr<VideoDecoder> create_video_decoder(CodecBackend backend, std::string& error) {
    error.clear();
    if (backend == CodecBackend::ffmpeg_software)
        return create_ffmpeg_decoder(backend, error);
    if (backend == CodecBackend::automatic || backend == CodecBackend::videotoolbox) {
#if KVMUX_HAS_VIDEOTOOLBOX
        return create_ffmpeg_decoder(CodecBackend::videotoolbox, error);
#else
        error = "VideoToolbox decoder unavailable: automatic decoding requires hardware; select FFmpeg software explicitly or build with KVMUX_VIDEOTOOLBOX=ON on macOS";
        return {};
#endif
    }
    error = "Unsupported decoder backend";
    return {};
}
}  // namespace kvmux
