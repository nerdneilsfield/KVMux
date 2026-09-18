#include "video/codec/ffmpeg_decoder.hpp"

#include <array>
#include <cstring>
#include <map>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#if KVMUX_HAS_VIDEOTOOLBOX
#include <libavcodec/videotoolbox.h>
#endif
}

namespace kvmux {
namespace {
constexpr std::size_t kMaxPending = 16;
CodecResult failure(std::string message) {
  return {CodecStatus::failed, std::move(message)};
}
std::string av_error(int code) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
  av_strerror(code, text.data(), text.size());
  return text.data();
}
AvFramePtr own(AVFrame* frame) {
  return {frame, [](AVFrame* value) { av_frame_free(&value); }};
}
// Validate the framing and recovery boundary before passing untrusted bytes to
// FFmpeg.
bool valid_au(const EncodedAccessUnit& au, bool recovery) {
  bool vps = false, sps = false, pps = false, idr = false, slice = false;
  const auto& b = au.bytes;
  std::size_t pos = 0;
  while (pos < b.size()) {
    std::size_t prefix = 0;
    if (pos + 3 <= b.size() && b[pos] == 0 && b[pos + 1] == 0 &&
        b[pos + 2] == 1)
      prefix = 3;
    else if (pos + 4 <= b.size() && b[pos] == 0 && b[pos + 1] == 0 &&
             b[pos + 2] == 0 && b[pos + 3] == 1)
      prefix = 4;
    if (!prefix || pos + prefix >= b.size()) return false;
    const auto start = pos + prefix;
    if (au.codec == VideoCodec::hevc) {
      if (start + 1 >= b.size() || (b[start] & 0x80) || !(b[start + 1] & 7))
        return false;
      const auto type = (b[start] >> 1) & 63;
      vps |= type == 32;
      sps |= type == 33;
      pps |= type == 34;
      if (type < 32) {
        slice = true;
        idr |= type == 19 || type == 20;
      }
    } else if (au.codec == VideoCodec::h264) {
      if (b[start] & 0x80) return false;
      const auto type = b[start] & 31;
      sps |= type == 7;
      pps |= type == 8;
      slice |= type == 1 || type == 5;
      idr |= type == 5;
    } else
      return false;
    pos = start + (au.codec == VideoCodec::hevc ? 2 : 1);
    while (pos + 3 <= b.size() &&
           !(b[pos] == 0 && b[pos + 1] == 0 &&
             (b[pos + 2] == 1 ||
              (pos + 4 <= b.size() && b[pos + 2] == 0 && b[pos + 3] == 1))))
      ++pos;
    if (pos + 3 > b.size()) pos = b.size();
  }
  const bool parameters = sps && pps && (au.codec == VideoCodec::h264 || vps);
  return slice && idr == au.idr && (!recovery || (idr && parameters));
}

class FfmpegDecoder final : public VideoDecoder {
 public:
  explicit FfmpegDecoder(CodecBackend backend) : backend_(backend) {}
  ~FfmpegDecoder() override { shutdown(); }
  CodecBackend backend() const noexcept override { return backend_; }
  CodecDiagnostic diagnostic() const override {
    CodecDiagnostic value;
    value.backend = backend_;
    value.hardware_active = hardware_active_;
    value.hardware_verified =
        hardware_active_ ||
        (context_ && backend_ == CodecBackend::ffmpeg_software);
    value.detail = diagnostic_;
    return value;
  }
  CodecResult configure(const CodecConfig& config) override {
    shutdown();
    if (!valid_dimensions(config.width, config.height))
      return {CodecStatus::invalid_input, "invalid H.26x dimensions"};
    if (config.codec != VideoCodec::h264 && config.codec != VideoCodec::hevc)
      return {CodecStatus::unsupported, "decoder accepts H.264 or HEVC only"};
    config_ = config;
    const auto codec_id =
        config.codec == VideoCodec::h264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC;
    const auto* codec = avcodec_find_decoder(codec_id);
    if (!codec)
      return {CodecStatus::unsupported,
              "FFmpeg requested H.26x decoder unavailable"};
    context_ = avcodec_alloc_context3(codec);
    if (!context_) return failure("allocate HEVC decoder");
    context_->thread_count = 1;
    context_->max_pixels =
        static_cast<std::int64_t>(kMaxCaptureWidth) * kMaxCaptureHeight;
    context_->err_recognition = AV_EF_EXPLODE;
    context_->opaque = this;
    context_->get_format = [](AVCodecContext* ctx,
                              const AVPixelFormat* formats) {
      const auto* self = static_cast<FfmpegDecoder*>(ctx->opaque);
      for (; *formats != AV_PIX_FMT_NONE; ++formats) {
        if (self->backend_ == CodecBackend::videotoolbox) {
          if (*formats == AV_PIX_FMT_VIDEOTOOLBOX) return *formats;
        } else {
          const auto* desc = av_pix_fmt_desc_get(*formats);
          if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *formats;
        }
      }
      return AV_PIX_FMT_NONE;  // Explicit hardware never falls back to
                               // software.
    };
    if (backend_ == CodecBackend::videotoolbox) {
      const int result = av_hwdevice_ctx_create(&context_->hw_device_ctx,
                                                AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
                                                nullptr, nullptr, 0);
      if (result < 0) {
        shutdown();
        return {CodecStatus::unsupported,
                "VideoToolbox device: " + av_error(result)};
      }
      context_->hwaccel_flags = 0;
    }
    const int result = avcodec_open2(context_, codec, nullptr);
    if (result < 0) {
      shutdown();
      return failure("open HEVC: " + av_error(result));
    }
    diagnostic_ = backend_ == CodecBackend::videotoolbox
                      ? "requested=videotoolbox output=unverified "
                        "apple_session_hardware=unverified"
                      : "requested=ffmpeg_software output=unverified";
    return {};
  }
  CodecResult reset() override {
    if (!context_) return failure("decoder is not configured");
    const auto config = config_;
    return configure(config);
  }
  void shutdown() noexcept override {
    avcodec_free_context(&context_);
    pending_.clear();
    needs_idr_ = true;
    finishing_ = false;
    receive_drained_ = true;
    failed_ = false;
    hardware_active_ = false;
    next_token_ = 0;
    diagnostic_ = "not configured";
  }
  CodecResult submit(const EncodedAccessUnit& au) override {
    if (!context_ || failed_ || finishing_)
      return failure("decoder needs configure/reset before submit");
    if (au.codec != config_.codec)
      return {CodecStatus::unsupported,
              "access-unit codec does not match decoder configuration"};
    if (au.width != config_.width || au.height != config_.height ||
        au.generation != config_.generation || au.bytes.empty() ||
        au.bytes.size() > kMaxCompressedSampleBytes ||
        !valid_au(au, needs_idr_))
      return {
          CodecStatus::invalid_input,
          "invalid Annex B H.26x AU or missing recovery IDR parameter sets"};
    if (pending_.size() >= kMaxPending)
      return {CodecStatus::again,
              "decoder queue full; retry this AU after poll"};
    AVPacket* packet = av_packet_alloc();
    if (!packet) return failure("allocate HEVC packet");
    int result = av_new_packet(packet, static_cast<int>(au.bytes.size()));
    if (result >= 0) {
      std::memcpy(packet->data, au.bytes.data(), au.bytes.size());
      packet->pts = next_token_;
      packet->dts = next_token_;
      result = avcodec_send_packet(context_, packet);
    }
    av_packet_free(&packet);
    if (result == AVERROR(EAGAIN))
      return {CodecStatus::again, "poll decoder then retry the same AU"};
    if (result < 0) {
      failed_ = true;
      return failure("send HEVC: " + av_error(result));
    }
    // Store metadata only, not another copy of the compressed payload.
    Metadata meta{
        au.pts_ns,          au.capture_sequence, au.generation,
        au.arrival,         au.color_range,      au.color_space,
        au.color_primaries, au.color_transfer,   au.sample_aspect_ratio};
    pending_.emplace(next_token_++, meta);
    receive_drained_ = false;
    needs_idr_ = false;
    return {};
  }
  CodecResult finish() override {
    if (!context_ || failed_)
      return failure("decoder is not configured or needs reset");
    if (finishing_) return {};
    // FFmpeg 58 can discard its buffered packet on NULL send. Consume input
    // through receive EAGAIN first; delayed pictures may still be pending.
    if (!receive_drained_) return {CodecStatus::again, "poll before finish"};
    const int result = avcodec_send_packet(context_, nullptr);
    if (result == AVERROR(EAGAIN))
      return {CodecStatus::again, "poll before finish"};
    if (result < 0) {
      failed_ = true;
      return failure("finish HEVC: " + av_error(result));
    }
    finishing_ = true;
    return {};
  }
  CodecResult poll(VideoFrame& output) override {
    if (!context_ || failed_)
      return failure("decoder is not configured or needs reset");
    auto frame = own(av_frame_alloc());
    if (!frame) return failure("allocate HEVC output");
    const int result = avcodec_receive_frame(context_, frame.get());
    if (result == AVERROR(EAGAIN)) {
      receive_drained_ = true;
      return {CodecStatus::again, {}};
    }
    if (result == AVERROR_EOF) {
      if (!pending_.empty()) {
        failed_ = true;
        return failure("HEVC ended with accepted AUs missing output");
      }
      return {CodecStatus::end_of_stream, {}};
    }
    if (result < 0) {
      failed_ = true;
      return failure("receive HEVC: " + av_error(result));
    }
    const auto it = pending_.find(frame->pts);
    if (it == pending_.end() ||
        frame->width != static_cast<int>(config_.width) ||
        frame->height != static_cast<int>(config_.height) ||
        (frame->flags & AV_FRAME_FLAG_CORRUPT)) {
      failed_ = true;
      return failure("HEVC output metadata, dimensions or integrity mismatch");
    }
    const auto meta = it->second;
    if (backend_ == CodecBackend::videotoolbox) {
      if (frame->format != AV_PIX_FMT_VIDEOTOOLBOX || !frame->hw_frames_ctx) {
        failed_ = true;
        return failure("VideoToolbox did not produce hardware-context frames");
      }
      std::string hardware = "unverified";
#if KVMUX_HAS_VIDEOTOOLBOX
      // FFmpeg may keep its session private. Do not infer this property from
      // pixel format.
      const auto* vt =
          static_cast<const AVVideotoolboxContext*>(context_->hwaccel_context);
      if (vt && vt->session) {
        CFTypeRef value = nullptr;
        if (VTSessionCopyProperty(
                vt->session,
                kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder,
                kCFAllocatorDefault, &value) == noErr &&
            value) {
          if (CFGetTypeID(value) == CFBooleanGetTypeID())
            hardware = CFBooleanGetValue(static_cast<CFBooleanRef>(value))
                           ? "yes"
                           : "no";
          CFRelease(value);
        }
      }
#endif
      if (hardware == "no") {
        failed_ = true;
        return failure("VideoToolbox reports a software decoding session");
      }
      auto cpu = own(av_frame_alloc());
      if (!cpu) {
        failed_ = true;
        return failure("allocate HEVC readback");
      }
      int transferred = av_hwframe_transfer_data(cpu.get(), frame.get(), 0);
      if (transferred >= 0)
        transferred = av_frame_copy_props(cpu.get(), frame.get());
      if (transferred < 0) {
        failed_ = true;
        return failure("VideoToolbox CPU transfer: " + av_error(transferred));
      }
      diagnostic_ =
          "requested=videotoolbox output=videotoolbox_vld hw_frames_ctx=yes "
          "cpu_transfer=" +
          std::string(
              av_get_pix_fmt_name(static_cast<AVPixelFormat>(cpu->format))) +
          " apple_session_hardware=" + hardware;
      hardware_active_ = hardware == "yes";
      frame = std::move(cpu);
    } else {
      const auto* desc =
          av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
      if (!desc || (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) ||
          frame->hw_frames_ctx) {
        failed_ = true;
        return failure("software decoder returned non-CPU frame");
      }
      diagnostic_ =
          "requested=ffmpeg_software output=" + std::string(desc->name);
    }
    pending_.erase(it);
    frame->pts = meta.pts;
    if (meta.range != AVCOL_RANGE_UNSPECIFIED) frame->color_range = meta.range;
    if (meta.space != AVCOL_SPC_UNSPECIFIED) frame->colorspace = meta.space;
    if (meta.primaries != AVCOL_PRI_UNSPECIFIED)
      frame->color_primaries = meta.primaries;
    if (meta.transfer != AVCOL_TRC_UNSPECIFIED)
      frame->color_trc = meta.transfer;
    if (meta.sar.num > 0 && meta.sar.den > 0)
      frame->sample_aspect_ratio = meta.sar;
    const auto sar = frame->sample_aspect_ratio;
    ColorMatrix matrix = ColorMatrix::unknown;
    if (frame->colorspace == AVCOL_SPC_BT709) matrix = ColorMatrix::bt709;
    if (frame->colorspace == AVCOL_SPC_BT470BG ||
        frame->colorspace == AVCOL_SPC_SMPTE170M)
      matrix = ColorMatrix::bt601;
    output = {meta.generation,
              meta.sequence,
              meta.arrival,
              std::chrono::steady_clock::now(),
              meta.pts,
              1,
              1'000'000'000,
              static_cast<std::uint32_t>(sar.num > 0 ? sar.num : 1),
              static_cast<std::uint32_t>(sar.den > 0 ? sar.den : 1),
              frame_color_range(*frame),
              matrix,
              std::move(frame)};
    return {};
  }

 private:
  struct Metadata {
    std::int64_t pts;
    std::uint64_t sequence, generation;
    std::chrono::steady_clock::time_point arrival;
    AVColorRange range;
    AVColorSpace space;
    AVColorPrimaries primaries;
    AVColorTransferCharacteristic transfer;
    AVRational sar;
  };
  CodecBackend backend_;
  CodecConfig config_{};
  AVCodecContext* context_{};
  std::map<std::int64_t, Metadata> pending_;
  std::int64_t next_token_{};
  bool needs_idr_{true}, finishing_{}, receive_drained_{true}, failed_{},
      hardware_active_{};
  std::string diagnostic_{"not configured"};
};
}  // namespace

std::unique_ptr<VideoDecoder> create_ffmpeg_decoder(CodecBackend backend,
                                                    std::string& error) {
  error.clear();
  if (backend != CodecBackend::videotoolbox &&
      backend != CodecBackend::ffmpeg_software) {
    error =
        "FFmpeg decoder requires explicit videotoolbox or ffmpeg_software "
        "backend";
    return {};
  }
#if !KVMUX_HAS_VIDEOTOOLBOX
  if (backend == CodecBackend::videotoolbox) {
    error = "VideoToolbox backend was not built";
    return {};
  }
#endif
  if (backend == CodecBackend::videotoolbox &&
      av_hwdevice_find_type_by_name("videotoolbox") == AV_HWDEVICE_TYPE_NONE) {
    error = "FFmpeg VideoToolbox backend unavailable";
    return {};
  }
  return std::make_unique<FfmpegDecoder>(backend);
}
}  // namespace kvmux
