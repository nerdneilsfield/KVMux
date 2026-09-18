#include "video/codec/ffmpeg_encoder.hpp"

#include <array>
#include <cstring>
#include <limits>
#include <map>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

namespace kvmux {
namespace {
CodecResult fail(std::string message) {
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
// The public input contract requires owned storage, not just plausible strides.
bool valid_plane(AVFrame* frame, int plane, int width, int height) {
  const auto* buffer = av_frame_get_plane_buffer(frame, plane);
  if (!buffer || !frame->data[plane] || frame->linesize[plane] < width)
    return false;
  const auto base = reinterpret_cast<std::uintptr_t>(buffer->data);
  const auto start = reinterpret_cast<std::uintptr_t>(frame->data[plane]);
  const auto bytes =
      static_cast<std::uint64_t>(height - 1) * frame->linesize[plane] + width;
  return start >= base && start - base <= buffer->size &&
         bytes <= buffer->size - (start - base);
}
class FfmpegEncoder final : public VideoEncoder {
 public:
  ~FfmpegEncoder() override { shutdown(); }
  CodecBackend backend() const noexcept override {
    return CodecBackend::ffmpeg_software;
  }
  CodecDiagnostic diagnostic() const override {
    const auto name = config_.codec == VideoCodec::h264 ? "libx264" : "libx265";
    const auto preset =
        encoding_profile(config_.priority, config_.bitrate).ffmpeg_preset;
    return {backend(), false, observed_,
            std::string(name) + " CPU encoding; priority=" +
                (config_.priority == EncodingPriority::quality ? "quality"
                                                               : "size") +
                " preset=" + preset +
                " effective_bitrate=" + std::to_string(effective_bitrate_) +
                (observed_ ? " output=verified hardware_active=no"
                           : " output=unverified")};
  }
  CodecResult configure(const CodecConfig& config) override {
    shutdown();
    if (!config.width || !config.height || config.width > kMaxCaptureWidth ||
        config.height > kMaxCaptureHeight || config.width % 2 ||
        config.height % 2 || !config.fps_numerator || !config.fps_denominator ||
        config.fps_numerator < config.fps_denominator ||
        config.fps_numerator >
            static_cast<unsigned>(std::numeric_limits<int>::max()) ||
        config.fps_denominator >
            static_cast<unsigned>(std::numeric_limits<int>::max()) ||
        config.fps_numerator > 240ULL * config.fps_denominator ||
        !config.bitrate || config.bitrate > 100'000'000 ||
        !config.keyframe_interval ||
        config.keyframe_interval >
            static_cast<unsigned>(std::numeric_limits<int>::max()))
      return {CodecStatus::invalid_input,
              "Invalid H.26x dimensions/rate/bitrate/keyframe interval"};
    if (config.codec != VideoCodec::h264 && config.codec != VideoCodec::hevc)
      return {CodecStatus::unsupported,
              "FFmpeg encoder accepts H.264 or HEVC only"};
    config_ = config;
    encoded_sequence_ = 0;
    const auto profile = encoding_profile(config.priority, config.bitrate);
    effective_bitrate_ = profile.bitrate;
    const char* encoder_name =
        config.codec == VideoCodec::h264 ? "libx264" : "libx265";
    const auto* codec = avcodec_find_encoder_by_name(encoder_name);
    if (!codec)
      return {CodecStatus::unsupported,
              std::string("FFmpeg ") + encoder_name + " encoder unavailable"};
    context_ = avcodec_alloc_context3(codec);
    if (!context_)
      return fail(std::string("allocate ") + encoder_name + " encoder");
    context_->width = static_cast<int>(config.width);
    context_->height = static_cast<int>(config.height);
    context_->pix_fmt = AV_PIX_FMT_YUV420P;
    context_->time_base = {static_cast<int>(config.fps_denominator),
                           static_cast<int>(config.fps_numerator)};
    context_->framerate = {static_cast<int>(config.fps_numerator),
                           static_cast<int>(config.fps_denominator)};
    context_->bit_rate = effective_bitrate_;
    context_->gop_size = static_cast<int>(config.keyframe_interval);
    context_->max_b_frames = 0;
    context_->thread_count = 1;
    // No lookahead, frame reordering, open GOP or unbounded frame-thread queue.
    const char* preset = profile.ffmpeg_preset;
    int result = av_opt_set(context_->priv_data, "preset", preset, 0);
    if (result >= 0)
      result = av_opt_set(context_->priv_data, "tune", "zerolatency", 0);
    if (result >= 0)
      result = av_opt_set(context_->priv_data, "forced-idr", "1", 0);
    if (result >= 0 && config.codec == VideoCodec::hevc)
      result = av_opt_set(
          context_->priv_data, "x265-params",
          "bframes=0:rc-lookahead=0:frame-threads=1:pools=none:open-gop=0:"
          "repeat-headers=1:annexb=1:aud=1:scenecut=0:log-level=error",
          0);
    if (result >= 0 && config.codec == VideoCodec::h264)
      result =
          av_opt_set(context_->priv_data, "x264-params",
                     "bframes=0:rc-lookahead=0:sync-lookahead=0:threads=1:open-"
                     "gop=0:repeat-headers=1:annexb=1:aud=1:scenecut=0",
                     0);
    if (result >= 0) result = avcodec_open2(context_, codec, nullptr);
    if (result < 0) {
      shutdown();
      return fail(std::string("open ") + encoder_name + ": " +
                  av_error(result));
    }
    return {};
  }
  CodecResult submit(const EncoderInput& input) override {
    if (!context_ || failed_ || finishing_)
      return fail("encoder needs configure/reset before submit");
    auto* source = input.frame.get();
    if (!source || source->hw_frames_ctx ||
        source->width != static_cast<int>(config_.width) ||
        source->height != static_cast<int>(config_.height) ||
        input.generation != config_.generation || input.pts_ns < 0 ||
        input.pts_ns <= last_pts_ ||
        (source->format != AV_PIX_FMT_NV12 &&
         source->format != AV_PIX_FMT_YUV420P))
      return {CodecStatus::invalid_input,
              "Expected matching owned CPU NV12/YUV420P and increasing "
              "nonnegative PTS"};
    const int w = source->width, h = source->height;
    const bool planar = source->format == AV_PIX_FMT_YUV420P;
    if (!valid_plane(source, 0, w, h) ||
        !valid_plane(source, 1, planar ? w / 2 : w, h / 2) ||
        (planar && !valid_plane(source, 2, w / 2, h / 2)))
      return {CodecStatus::invalid_input,
              "Invalid owned CPU frame planes or strides"};
    if (pending_.size() >= 4)
      return {CodecStatus::again, "poll encoder then retry input"};
    auto frame = own(av_frame_alloc());
    if (!frame) return fail("allocate encoder frame");
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = w;
    frame->height = h;
    int result = av_frame_get_buffer(frame.get(), 32);
    if (result >= 0) result = av_frame_copy_props(frame.get(), source);
    if (result < 0)
      return fail("allocate/copy encoder frame: " + av_error(result));
    for (int y = 0; y < h; ++y)
      std::memcpy(frame->data[0] + y * frame->linesize[0],
                  source->data[0] + y * source->linesize[0], w);
    for (int y = 0; y < h / 2; ++y)
      for (int x = 0; x < w / 2; ++x) {
        frame->data[1][y * frame->linesize[1] + x] =
            source->data[1][y * source->linesize[1] + (planar ? x : 2 * x)];
        frame->data[2][y * frame->linesize[2] + x] =
            planar ? source->data[2][y * source->linesize[2] + x]
                   : source->data[1][y * source->linesize[1] + 2 * x + 1];
      }
    frame->pts = next_token_;
    frame->pict_type = force_idr_ ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    result = avcodec_send_frame(context_, frame.get());
    if (result == AVERROR(EAGAIN))
      return {CodecStatus::again, "poll encoder then retry same input"};
    if (result < 0) {
      failed_ = true;
      return fail("send H.26x frame: " + av_error(result));
    }
    EncodedAccessUnit meta;
    meta.width = config_.width;
    meta.height = config_.height;
    meta.pts_ns = input.pts_ns;
    meta.capture_sequence = input.capture_sequence;
    meta.generation = input.generation;
    meta.arrival = input.arrival;
    meta.color_range = source->color_range;
    meta.color_space = source->colorspace;
    meta.color_primaries = source->color_primaries;
    meta.color_transfer = source->color_trc;
    meta.sample_aspect_ratio = source->sample_aspect_ratio.num > 0 &&
                                       source->sample_aspect_ratio.den > 0
                                   ? source->sample_aspect_ratio
                                   : AVRational{1, 1};
    pending_.emplace(next_token_++, std::move(meta));
    last_pts_ = input.pts_ns;
    force_idr_ = false;
    return {};
  }
  CodecResult poll(EncodedAccessUnit& output) override {
    if (!context_ || failed_)
      return fail("encoder is not configured or needs reset");
    auto* packet = av_packet_alloc();
    if (!packet) return fail("allocate encoder packet");
    const int result = avcodec_receive_packet(context_, packet);
    if (result < 0) {
      av_packet_free(&packet);
      if (result == AVERROR(EAGAIN)) return {CodecStatus::again, {}};
      if (result == AVERROR_EOF && pending_.empty())
        return {CodecStatus::end_of_stream, {}};
      failed_ = true;
      return fail("receive libx265 packet or missing accepted frame: " +
                  av_error(result));
    }
    const auto it = pending_.find(packet->pts);
    if (it == pending_.end() || it != pending_.begin() || packet->size <= 0 ||
        static_cast<std::size_t>(packet->size) > kMaxCompressedSampleBytes) {
      av_packet_free(&packet);
      failed_ = true;
      return fail("H.26x output violates AU/PTS bounds");
    }
    auto au = std::move(it->second);
    au.bytes.assign(packet->data, packet->data + packet->size);
    av_packet_free(&packet);
    bool vps = false, sps = false, pps = false;
    for (std::size_t i = 0; i + 4 < au.bytes.size(); ++i) {
      if (au.bytes[i] || au.bytes[i + 1] || au.bytes[i + 2] != 1) continue;
      if (config_.codec == VideoCodec::hevc) {
        const auto type = (au.bytes[i + 3] >> 1) & 63;
        vps |= type == 32;
        sps |= type == 33;
        pps |= type == 34;
        au.idr |= type == 19 || type == 20;
      } else {
        const auto type = au.bytes[i + 3] & 31;
        sps |= type == 7;
        pps |= type == 8;
        au.idr |= type == 5;
      }
    }
    if (au.idr && !(sps && pps && (config_.codec == VideoCodec::h264 || vps))) {
      failed_ = true;
      return fail("H.26x IDR missing repeated parameter sets");
    }
    au.codec = config_.codec;
    pending_.erase(it);
    au.encoded_sequence = ++encoded_sequence_;
    output = std::move(au);
    observed_ = true;
    return {};
  }
  CodecResult request_keyframe() override {
    if (!context_ || failed_ || finishing_)
      return fail("encoder not accepting keyframe requests");
    force_idr_ = true;
    return {};
  }
  CodecResult finish() override {
    if (!context_ || failed_)
      return fail("encoder is not configured or needs reset");
    if (finishing_) return {};
    const int result = avcodec_send_frame(context_, nullptr);
    if (result == AVERROR(EAGAIN))
      return {CodecStatus::again, "poll before finish"};
    if (result < 0) {
      failed_ = true;
      return fail("finish libx265: " + av_error(result));
    }
    finishing_ = true;
    return {};
  }
  CodecResult reset() override {
    if (!context_) return fail("encoder is not configured");
    const auto config = config_;
    const auto sequence = encoded_sequence_;
    auto result = configure(config);
    encoded_sequence_ = sequence;
    return result;
  }
  void shutdown() noexcept override {
    avcodec_free_context(&context_);
    pending_.clear();
    next_token_ = 0;
    last_pts_ = -1;
    force_idr_ = true;
    finishing_ = false;
    failed_ = false;
    observed_ = false;
  }

 private:
  CodecConfig config_{};
  AVCodecContext* context_{};
  std::map<std::int64_t, EncodedAccessUnit> pending_;
  std::int64_t next_token_{}, last_pts_{-1};
  std::uint64_t encoded_sequence_{};
  std::uint32_t effective_bitrate_{};
  bool force_idr_{true}, finishing_{}, failed_{}, observed_{};
};
}  // namespace
std::unique_ptr<VideoEncoder> create_ffmpeg_encoder(CodecBackend backend,
                                                    std::string& error) {
  error.clear();
  if (backend != CodecBackend::ffmpeg_software) {
    error = "FFmpeg encoder requires explicit ffmpeg_software backend";
    return {};
  }
  return std::make_unique<FfmpegEncoder>();
}
}  // namespace kvmux
