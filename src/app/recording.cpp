#include "app/recording.hpp"

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <memory>
#include <string_view>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace kvmux {
namespace {
std::string av_error(int code) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
  av_strerror(code, text.data(), text.size());
  return text.data();
}
const char* pixel_format_name(const VideoFrame& frame) {
  if (!frame.frame) return "none";
  const char* const name =
      av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame.frame->format));
  return name != nullptr ? name : "unknown";
}
const char* range_name(ColorRange range) {
  switch (range) {
    case ColorRange::limited:
      return "limited";
    case ColorRange::full:
      return "full";
    case ColorRange::unknown:
      return "unknown";
  }
  return "unknown";
}
std::string path_text(const std::filesystem::path& path) {
  if (path.empty()) return "-";
  const auto utf8 = path.u8string();
  return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}
void log_snapshot_failed(const std::filesystem::path& path, unsigned width,
                         unsigned height, const char* format,
                         std::string_view error, int err) {
  spdlog::debug(
      "snapshot failed: path={} width={} height={} format={} error={} errno={}",
      path_text(path), width, height, format != nullptr ? format : "unknown",
      error, err);
}
void log_recording_result(const char* event, const char* result,
                          const std::string& encoder,
                          const std::filesystem::path& path, unsigned frames,
                          std::string_view error) {
  spdlog::debug("recording {}: result={} encoder={} path={} frames={} error={}",
                event, result, encoder.empty() ? "-" : encoder, path_text(path),
                frames, error.empty() ? "-" : error);
}
const char* stitch_result_name(StitchResult result) {
  switch (result) {
    case StitchResult::started:
      return "started";
    case StitchResult::appended:
      return "appended";
    case StitchResult::unchanged:
      return "unchanged";
    case StitchResult::bad_match:
      return "bad-match";
    case StitchResult::reverse:
      return "reverse";
    case StitchResult::stale:
      return "stale";
    case StitchResult::size_limit:
      return "size-limit";
    case StitchResult::invalid:
      return "invalid";
  }
  return "unknown";
}
bool even_crop_inside(const FrameCrop& crop, unsigned source_width,
                      unsigned source_height) {
  return crop.width && crop.height && crop.x < source_width &&
         crop.y < source_height && crop.width <= source_width - crop.x &&
         crop.height <= source_height - crop.y &&
         (crop.x | crop.y | crop.width | crop.height) % 2 == 0;
}
std::filesystem::path downloads_directory() {
  const char* path = SDL_GetUserFolder(SDL_FOLDER_DOWNLOADS);
  if (!path) return {};
  std::filesystem::path result(path);
  return result;
}
std::filesystem::path timestamped(const std::filesystem::path& dir,
                                  std::string_view extension) {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &time);
#else
  localtime_r(&time, &local);
#endif
  std::array<char, 32> stamp{};
  std::strftime(stamp.data(), stamp.size(), "kvmux_%Y%m%d_%H%M%S", &local);
  const auto base = std::string(stamp.data());
  for (unsigned suffix = 0;; ++suffix) {
    const auto name = suffix == 0 ? base + std::string(extension)
                                  : base + "_" + std::to_string(suffix) +
                                        std::string(extension);
    const auto candidate = dir / name;
    if (!std::filesystem::exists(candidate)) return candidate;
  }
}
bool valid_frame(const VideoFrame& input) {
  const auto* frame = input.frame.get();
  return frame && !frame->hw_frames_ctx && frame->width > 0 &&
         frame->height > 0 &&
         frame->width <= static_cast<int>(kMaxCaptureWidth) &&
         frame->height <= static_cast<int>(kMaxCaptureHeight) &&
         av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
}
}  // namespace

struct Recording::Impl {
  AVFormatContext* format{};
  AVCodecContext* codec{};
  AVStream* stream{};
  SwsContext* convert{};
  std::chrono::steady_clock::time_point first_arrival{};
  std::int64_t last_pts{-1};
  FrameCrop crop{};
  std::string encoder_name;
  unsigned frames_written{};
  // Reused for every appended frame.
  AVFrame* picture{};
  AVPacket* packet{};
  Impl() = default;
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;
  ~Impl() {
    av_frame_free(&picture);
    av_packet_free(&packet);
  }
};

Recording::Recording(std::filesystem::path output_directory)
    : output_directory_(std::move(output_directory)) {}
Recording::~Recording() { shutdown(); }
std::filesystem::path Recording::downloads_directory() const {
  return output_directory_.empty() ? kvmux::downloads_directory()
                                   : output_directory_;
}
void Recording::fail(std::string message) noexcept {
  std::lock_guard lock(mutex_);
  status_.error = std::move(message);
  status_.state = RecordingState::failed;
}
RecordingStatus Recording::status() const {
  std::lock_guard lock(mutex_);
  return status_;
}

bool Recording::snapshot_now(const VideoFrame& input,
                             std::optional<FrameCrop> requested_crop) {
  const char* const format = pixel_format_name(input);
  const unsigned source_width =
      input.frame ? static_cast<unsigned>(input.frame->width) : 0;
  const unsigned source_height =
      input.frame ? static_cast<unsigned>(input.frame->height) : 0;
  if (!valid_frame(input)) {
    constexpr char message[] = "snapshot requires a valid CPU VideoFrame";
    {
      std::lock_guard lock(mutex_);
      status_.snapshot_error = message;
    }
    log_snapshot_failed({}, source_width, source_height, format, message, 0);
    return false;
  }
  const FrameCrop crop =
      requested_crop.value_or(FrameCrop{0, 0, source_width, source_height});
  if (!crop.width || !crop.height || crop.x >= source_width ||
      crop.y >= source_height || crop.width > source_width - crop.x ||
      crop.height > source_height - crop.y) {
    constexpr char message[] = "snapshot crop is outside the source frame";
    {
      std::lock_guard lock(mutex_);
      status_.snapshot_error = message;
    }
    log_snapshot_failed({}, crop.width, crop.height, format, message, 0);
    return false;
  }
  const auto directory = downloads_directory();
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    const std::string message = "create Downloads directory: " + ec.message();
    {
      std::lock_guard lock(mutex_);
      status_.snapshot_error = message;
    }
    log_snapshot_failed({}, crop.width, crop.height, format, message,
                        ec.value());
    return false;
  }
  const auto path = timestamped(directory, ".jpeg");
  const auto* encoder = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
  if (!encoder) {
    constexpr char message[] = "FFmpeg MJPEG encoder unavailable";
    {
      std::lock_guard lock(mutex_);
      status_.snapshot_error = message;
    }
    log_snapshot_failed({}, crop.width, crop.height, format, message, 0);
    return false;
  }
  AVCodecContext* context = avcodec_alloc_context3(encoder);
  AVFrame* image = av_frame_alloc();
  AVPacket* packet = av_packet_alloc();
  if (!context || !image || !packet) {
    avcodec_free_context(&context);
    av_frame_free(&image);
    av_packet_free(&packet);
    constexpr char message[] = "allocate JPEG encoder";
    {
      std::lock_guard lock(mutex_);
      status_.snapshot_error = message;
    }
    log_snapshot_failed({}, crop.width, crop.height, format, message, 0);
    return false;
  }
  context->width = static_cast<int>(crop.width);
  context->height = static_cast<int>(crop.height);
  context->pix_fmt = AV_PIX_FMT_YUVJ420P;
  context->time_base = {1, 1};
  int result = avcodec_open2(context, encoder, nullptr);
  if (result >= 0) {
    image->format = context->pix_fmt;
    image->width = context->width;
    image->height = context->height;
    result = av_frame_get_buffer(image, 32);
  }
  AVFrame* source = av_frame_clone(input.frame.get());
  if (result >= 0 && !source) result = AVERROR(ENOMEM);
  if (result >= 0) {
    source->crop_left = crop.x;
    source->crop_top = crop.y;
    source->crop_right = source_width - crop.x - crop.width;
    source->crop_bottom = source_height - crop.y - crop.height;
    result = av_frame_apply_cropping(source, AV_FRAME_CROP_UNALIGNED);
  }
  if (result >= 0 && (source->width != static_cast<int>(crop.width) ||
                      source->height != static_cast<int>(crop.height)))
    result = AVERROR(EINVAL);
  SwsContext* convert = nullptr;
  if (result >= 0)
    convert = sws_getContext(source->width, source->height,
                             static_cast<AVPixelFormat>(source->format),
                             image->width, image->height, context->pix_fmt,
                             SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (result >= 0 && !convert) result = AVERROR(ENOMEM);
  if (result >= 0) {
    const int rows = sws_scale(convert, source->data, source->linesize, 0,
                               source->height, image->data, image->linesize);
    result = rows == image->height ? 0 : AVERROR(EINVAL);
  }
  if (result >= 0) result = avcodec_send_frame(context, image);
  if (result >= 0) result = avcodec_receive_packet(context, packet);
  auto temporary = path;
  temporary += ".part";
  int io_errno = 0;
  const int jpeg_bytes = packet != nullptr ? packet->size : 0;
  if (result >= 0) {
    std::ofstream file(temporary, std::ios::binary);
    if (!file) {
      io_errno = errno;
      result = AVERROR(EIO);
    } else {
      file.write(reinterpret_cast<const char*>(packet->data), packet->size);
      if (!file) {
        io_errno = errno;
        result = AVERROR(EIO);
      }
    }
  }
  if (result >= 0) {
    std::error_code rename_error;
    std::filesystem::rename(temporary, path, rename_error);
    if (rename_error) {
      io_errno = rename_error.value();
      result = AVERROR(EIO);
    }
  }
  if (result < 0) {
    std::error_code remove_error;
    std::filesystem::remove(temporary, remove_error);
  }
  sws_freeContext(convert);
  av_frame_free(&source);
  av_packet_free(&packet);
  av_frame_free(&image);
  avcodec_free_context(&context);
  if (result < 0) {
    const std::string message = "write JPEG: " + av_error(result);
    {
      std::lock_guard lock(mutex_);
      status_.snapshot_error = message;
    }
    log_snapshot_failed(path, crop.width, crop.height, format, message,
                        io_errno);
    return false;
  }
  {
    std::lock_guard lock(mutex_);
    status_.last_snapshot_path = path;
    status_.snapshot_error.clear();
  }
  spdlog::debug(
      "snapshot saved: path={} width={} height={} format={} range={} bytes={}",
      path_text(path), crop.width, crop.height, format,
      range_name(input.color_range), jpeg_bytes);
  return true;
}

using SwsContextPtr = std::unique_ptr<SwsContext, decltype(&sws_freeContext)>;

// Converts only the crop region; scroll crops are even, so 4:2:0 chroma planes
// start on a whole sample, as in snapshot_now.
static std::vector<std::uint8_t> crop_rgb(const VideoFrame& input,
                                          FrameCrop crop,
                                          SwsContextPtr& convert) {
  if (!valid_frame(input) || !crop.width || !crop.height ||
      crop.x + crop.width > static_cast<unsigned>(input.frame->width) ||
      crop.y + crop.height > static_cast<unsigned>(input.frame->height))
    return {};
  AVFrame* source = av_frame_clone(input.frame.get());
  int result = source ? 0 : AVERROR(ENOMEM);
  if (result >= 0) {
    source->crop_left = crop.x;
    source->crop_top = crop.y;
    source->crop_right =
        static_cast<unsigned>(input.frame->width) - crop.x - crop.width;
    source->crop_bottom =
        static_cast<unsigned>(input.frame->height) - crop.y - crop.height;
    result = av_frame_apply_cropping(source, AV_FRAME_CROP_UNALIGNED);
  }
  if (result >= 0 && (source->width != static_cast<int>(crop.width) ||
                      source->height != static_cast<int>(crop.height)))
    result = AVERROR(EINVAL);
  if (result >= 0) {
    convert.reset(
        sws_getCachedContext(convert.release(), source->width, source->height,
                             static_cast<AVPixelFormat>(source->format),
                             source->width, source->height, AV_PIX_FMT_RGB24,
                             SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!convert) result = AVERROR(ENOMEM);
  }
  std::vector<std::uint8_t> rgb;
  if (result >= 0) {
    rgb.resize(static_cast<std::size_t>(crop.width) * crop.height * 3);
    std::uint8_t* data[]{rgb.data()};
    int stride[]{static_cast<int>(crop.width * 3)};
    if (sws_scale(convert.get(), source->data, source->linesize, 0,
                  source->height, data, stride) < 0)
      rgb.clear();
  }
  av_frame_free(&source);
  return rgb;
}
static bool write_rgb_jpeg(std::span<const std::uint8_t> rgb, unsigned width,
                           unsigned height, const std::filesystem::path& path) {
  const auto* encoder = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
  AVCodecContext* context = encoder ? avcodec_alloc_context3(encoder) : nullptr;
  AVFrame* image = av_frame_alloc();
  AVPacket* packet = av_packet_alloc();
  if (!context || !image || !packet) {
    avcodec_free_context(&context);
    av_frame_free(&image);
    av_packet_free(&packet);
    return false;
  }
  context->width = static_cast<int>(width);
  context->height = static_cast<int>(height);
  context->pix_fmt = AV_PIX_FMT_YUVJ420P;
  context->time_base = {1, 1};
  int result = avcodec_open2(context, encoder, nullptr);
  if (result >= 0) {
    image->format = context->pix_fmt;
    image->width = context->width;
    image->height = context->height;
    result = av_frame_get_buffer(image, 32);
  }
  SwsContext* convert =
      result >= 0
          ? sws_getContext(context->width, context->height, AV_PIX_FMT_RGB24,
                           context->width, context->height, context->pix_fmt,
                           SWS_BILINEAR, nullptr, nullptr, nullptr)
          : nullptr;
  if (result >= 0 && !convert) result = AVERROR(ENOMEM);
  const std::uint8_t* source[]{rgb.data()};
  int stride[]{static_cast<int>(width * 3)};
  if (result >= 0 &&
      sws_scale(convert, source, stride, 0, static_cast<int>(height),
                image->data, image->linesize) < 0)
    result = AVERROR(EINVAL);
  if (result >= 0) result = avcodec_send_frame(context, image);
  if (result >= 0) result = avcodec_receive_packet(context, packet);
  auto temporary = path;
  temporary += ".part";
  if (result >= 0) {
    std::ofstream f(temporary, std::ios::binary);
    f.write(reinterpret_cast<const char*>(packet->data), packet->size);
    if (!f) result = AVERROR(EIO);
  }
  if (result >= 0) {
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) result = AVERROR(EIO);
  } else {
    std::error_code ec;
    std::filesystem::remove(temporary, ec);
  }
  sws_freeContext(convert);
  av_packet_free(&packet);
  av_frame_free(&image);
  avcodec_free_context(&context);
  return result >= 0;
}

bool Recording::start_now(const VideoFrame& first,
                          std::optional<FrameCrop> requested_crop) {
  {
    std::lock_guard lock(mutex_);
    status_.error.clear();
    status_.output_path.clear();
  }
  const auto reject = [](std::string message, const std::string& encoder,
                         const std::filesystem::path& path) {
    log_recording_result("start", "failed", encoder, path, 0, message);
    return message;
  };
  if (!valid_frame(first)) {
    const std::string message =
        "recording requires a valid CPU VideoFrame first=" +
        std::to_string(first.frame ? first.frame->width : -1);
    fail(reject(message, {}, {}));
    return false;
  }
  const auto directory = downloads_directory();
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    fail(reject("create Downloads directory: " + ec.message(), {}, {}));
    return false;
  }
  auto impl = std::make_unique<Impl>();
  const auto* encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
  if (!encoder) {
    fail(reject("FFmpeg H.264 encoder unavailable", {}, {}));
    return false;
  }
  impl->encoder_name = encoder->name != nullptr ? encoder->name : "h264";
  int result =
      avformat_alloc_output_context2(&impl->format, nullptr, "mp4", nullptr);
  if (result < 0 || !impl->format) {
    fail(reject("allocate MP4 muxer: " + av_error(result), impl->encoder_name,
                {}));
    return false;
  }
  impl->stream = avformat_new_stream(impl->format, nullptr);
  impl->codec = avcodec_alloc_context3(encoder);
  if (!impl->stream || !impl->codec) {
    avformat_free_context(impl->format);
    fail(reject("allocate MP4 stream/encoder", impl->encoder_name, {}));
    return false;
  }
  const unsigned source_width = static_cast<unsigned>(first.frame->width),
                 source_height = static_cast<unsigned>(first.frame->height);
  const FrameCrop crop =
      requested_crop.value_or(FrameCrop{0, 0, source_width, source_height});
  if (!crop.width || !crop.height || crop.x >= source_width ||
      crop.y >= source_height || crop.width > source_width - crop.x ||
      crop.height > source_height - crop.y ||
      (crop.x | crop.y | crop.width | crop.height) & 1U) {
    fail(reject(
        "recording crop must be inside the source frame with even coordinates "
        "and dimensions",
        impl->encoder_name, {}));
    return false;
  }
  const unsigned width = crop.width, height = crop.height;
  impl->crop = crop;
  {
    std::lock_guard lock(mutex_);
    status_.width = width;
    status_.height = height;
  }
  impl->codec->codec_id = encoder->id;
  impl->codec->codec_type = AVMEDIA_TYPE_VIDEO;
  impl->codec->width = static_cast<int>(width);
  impl->codec->height = static_cast<int>(height);
  impl->codec->pix_fmt = AV_PIX_FMT_YUV420P;
  impl->codec->time_base = {1, 1'000'000};
  impl->codec->framerate = {0, 1};
  impl->codec->max_b_frames = 0;
  // libx264 defaults to "medium"; recording runs beside live capture.
  if (impl->encoder_name == "libx264")
    (void)av_opt_set(impl->codec->priv_data, "preset", "veryfast", 0);
  if (impl->format->oformat->flags & AVFMT_GLOBALHEADER)
    impl->codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  result = avcodec_open2(impl->codec, encoder, nullptr);
  if (result >= 0)
    result =
        avcodec_parameters_from_context(impl->stream->codecpar, impl->codec);
  impl->stream->time_base = impl->codec->time_base;
  std::filesystem::path output = timestamped(directory, ".mp4");
  {
    std::lock_guard lock(mutex_);
    status_.output_path = output;
  }
  if (result >= 0) {
    const auto utf8_path = output.u8string();
    result = avio_open(&impl->format->pb,
                       reinterpret_cast<const char*>(utf8_path.c_str()),
                       AVIO_FLAG_WRITE);
  }
  if (result >= 0) result = avformat_write_header(impl->format, nullptr);
  if (result < 0) {
    if (impl->format->pb) avio_closep(&impl->format->pb);
    avcodec_free_context(&impl->codec);
    avformat_free_context(impl->format);
    fail(reject("start MP4 recording: " + av_error(result), impl->encoder_name,
                output));
    return false;
  }
  impl->first_arrival = first.arrival;
  impl_ = impl.release();
  if (!write_frame(first)) {
    std::string error;
    {
      std::lock_guard lock(mutex_);
      error = status_.error;
    }
    log_recording_result("start", "failed", impl_->encoder_name, output,
                         impl_->frames_written, error);
    return false;
  }
  spdlog::debug(
      "recording start: result=ok encoder={} path={} width={} height={} "
      "format={} range={} frames={}",
      impl_->encoder_name, path_text(output), width, height,
      pixel_format_name(first), range_name(first.color_range),
      impl_->frames_written);
  return true;
}

bool Recording::write_frame(const VideoFrame& input) {
  auto& impl = *impl_;
  auto* source = input.frame.get();
  if (!impl.picture) impl.picture = av_frame_alloc();
  if (!impl.packet) impl.packet = av_packet_alloc();
  if (!impl.picture || !impl.packet) {
    fail("allocate MP4 frame/packet");
    return false;
  }
  AVFrame* const picture = impl.picture;
  AVPacket* const packet = impl.packet;
  int result{};
  if (picture->buf[0] == nullptr) {
    picture->format = impl.codec->pix_fmt;
    picture->width = impl.codec->width;
    picture->height = impl.codec->height;
    result = av_frame_get_buffer(picture, 32);
  } else {
    // Copies only if the encoder still holds a reference to the last frame.
    result = av_frame_make_writable(picture);
  }
  const auto crop = impl.crop;
  if (!valid_frame(input) ||
      crop.x + crop.width > static_cast<unsigned>(source->width) ||
      crop.y + crop.height > static_cast<unsigned>(source->height))
    result = AVERROR(EINVAL);
  if (result >= 0) {
    impl.convert = sws_getCachedContext(
        impl.convert, static_cast<int>(crop.width),
        static_cast<int>(crop.height),
        static_cast<AVPixelFormat>(source->format), picture->width,
        picture->height, impl.codec->pix_fmt, SWS_BILINEAR, nullptr, nullptr,
        nullptr);
    if (!impl.convert) result = AVERROR(ENOMEM);
  }
  std::array<const std::uint8_t*, 4> source_data{};
  if (result >= 0) {
    const auto* descriptor =
        av_pix_fmt_desc_get(static_cast<AVPixelFormat>(source->format));
    for (int plane = 0; plane < 4 && source->data[plane]; ++plane) {
      const int shift_x = plane == 0 ? 0 : descriptor->log2_chroma_w;
      const int shift_y = plane == 0 ? 0 : descriptor->log2_chroma_h;
      const int component =
          plane == 0 ? 0 : std::min(plane, descriptor->nb_components - 1);
      source_data[plane] =
          source->data[plane] + (crop.y >> shift_y) * source->linesize[plane] +
          (crop.x >> shift_x) * descriptor->comp[component].step;
    }
    if (sws_scale(impl.convert, source_data.data(), source->linesize, 0,
                  static_cast<int>(crop.height), picture->data,
                  picture->linesize) < 0)
      result = AVERROR(EINVAL);
  }
  std::chrono::steady_clock::duration paused;
  {
    std::lock_guard lock(mutex_);
    paused = paused_duration_;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                           input.arrival - impl.first_arrival - paused)
                           .count();
  if (result >= 0)
    picture->pts = std::max<std::int64_t>(impl.last_pts + 1,
                                          std::max<std::int64_t>(0, elapsed));
  if (result >= 0) result = avcodec_send_frame(impl.codec, picture);
  while (result >= 0) {
    result = avcodec_receive_packet(impl.codec, packet);
    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
      result = 0;
      break;
    }
    if (result >= 0) {
      av_packet_rescale_ts(packet, impl.codec->time_base,
                           impl.stream->time_base);
      packet->stream_index = impl.stream->index;
      result = av_interleaved_write_frame(impl.format, packet);
      av_packet_unref(packet);
    }
  }
  if (result >= 0) impl.last_pts = picture->pts;
  if (result < 0) {
    const std::string message = "append MP4 frame: " + av_error(result);
    std::filesystem::path path;
    {
      std::lock_guard lock(mutex_);
      path = status_.output_path;
    }
    fail(message);
    spdlog::debug("recording write: result=failed path={} error={}",
                  path_text(path), message);
    return false;
  }
  ++impl.frames_written;
  return true;
}
bool Recording::append(const VideoFrame& input) {
  if (!valid_frame(input)) {
    std::lock_guard lock(mutex_);
    status_.error = "recording requires a valid CPU VideoFrame";
    return false;
  }
  ensure_worker();
  {
    std::lock_guard lock(mutex_);
    if (status_.state != RecordingState::recording) return false;
    latest_ = input;
  }
  wake_.notify_one();
  return true;
}
bool Recording::snapshot(const VideoFrame& input,
                         std::optional<FrameCrop> crop) {
  const unsigned width =
      input.frame != nullptr ? static_cast<unsigned>(input.frame->width) : 0;
  const unsigned height =
      input.frame != nullptr ? static_cast<unsigned>(input.frame->height) : 0;
  const FrameCrop shown = crop.value_or(FrameCrop{0, 0, width, height});
  if (!valid_frame(input)) {
    constexpr char message[] = "snapshot requires a valid CPU VideoFrame";
    {
      std::lock_guard lock(mutex_);
      status_.snapshot_error = message;
    }
    log_snapshot_failed({}, width, height, pixel_format_name(input), message,
                        0);
    return false;
  }
  ensure_worker();
  {
    std::lock_guard lock(mutex_);
    status_.snapshot_error.clear();
    snapshot_ = std::pair{input, crop};
  }
  wake_.notify_one();
  spdlog::debug(
      "snapshot request: width={} height={} format={} range={} "
      "crop={}x{}+{},{}",
      shown.width, shown.height, pixel_format_name(input),
      range_name(input.color_range), shown.width, shown.height, shown.x,
      shown.y);
  return true;
}
bool Recording::start_scroll(const VideoFrame& input, FrameCrop crop) {
  const unsigned source_width =
      input.frame ? static_cast<unsigned>(input.frame->width) : 0;
  const unsigned source_height =
      input.frame ? static_cast<unsigned>(input.frame->height) : 0;
  spdlog::debug(
      "scroll start request: crop={}x{}+{},{} source={}x{} format={} range={}",
      crop.width, crop.height, crop.x, crop.y, source_width, source_height,
      pixel_format_name(input), range_name(input.color_range));
  const auto reject = [this](const char* message) {
    {
      std::lock_guard lock(mutex_);
      if (status_.scroll_state == ScrollCaptureState::idle)
        status_.scroll_error = message;
    }
    spdlog::debug("scroll start: result=rejected error={}", message);
    return false;
  };
  if (!valid_frame(input))
    return reject("scrolling capture requires a valid CPU VideoFrame");
  if (!even_crop_inside(crop, source_width, source_height))
    return reject(
        "scrolling crop must be inside the source frame with even "
        "coordinates and dimensions");
  const char* busy = nullptr;
  {
    std::lock_guard lock(mutex_);
    if (status_.scroll_state != ScrollCaptureState::idle) {
      busy = scroll_state_name(status_.scroll_state);
    } else {
      status_.scroll_state = ScrollCaptureState::starting;
      status_.scroll_error.clear();
      status_.last_scroll_path.clear();
      status_.scroll_width = status_.scroll_height = 0;
      scroll_start_ = ScrollRequest{input, crop};
    }
  }
  if (busy) {
    spdlog::debug("scroll start: result=rejected error=busy state={}", busy);
    return false;
  }
  spdlog::debug("scroll state: starting crop={}x{}+{},{}", crop.width,
                crop.height, crop.x, crop.y);
  ensure_worker();
  wake_.notify_one();
  return true;
}
bool Recording::sample_scroll(const VideoFrame& input) {
  if (!valid_frame(input)) return false;
  std::lock_guard lock(mutex_);
  if (status_.scroll_state != ScrollCaptureState::capturing) return false;
  scroll_latest_ = input;
  wake_.notify_one();
  return true;
}
bool Recording::finish_scroll() {
  const char* state = nullptr;
  bool accepted = false;
  {
    std::lock_guard lock(mutex_);
    state = scroll_state_name(status_.scroll_state);
    if (status_.scroll_state == ScrollCaptureState::capturing) {
      status_.scroll_state = ScrollCaptureState::finishing;
      scroll_finish_ = true;
      accepted = true;
    }
  }
  if (!accepted) {
    spdlog::debug("scroll finish: result=rejected state={}", state);
    return false;
  }
  wake_.notify_one();
  spdlog::debug("scroll state: finishing");
  return true;
}
void Recording::cancel_scroll(const char* reason) {
  bool requested = false;
  {
    std::lock_guard lock(mutex_);
    if (status_.scroll_state == ScrollCaptureState::idle) return;
    scroll_cancel_ = true;
    scroll_start_.reset();
    scroll_latest_.reset();
    requested = true;
  }
  if (!requested) return;
  spdlog::debug("scroll cancel: reason={}", reason != nullptr ? reason : "-");
  wake_.notify_one();
}
bool Recording::start(const VideoFrame& input, std::optional<FrameCrop> crop) {
  const unsigned source_width =
      input.frame ? static_cast<unsigned>(input.frame->width) : 0;
  const unsigned source_height =
      input.frame ? static_cast<unsigned>(input.frame->height) : 0;
  const FrameCrop shown =
      crop.value_or(FrameCrop{0, 0, source_width, source_height});
  const bool full_frame =
      source_width > 0 && source_height > 0 && shown.x == 0 && shown.y == 0 &&
      shown.width == source_width && shown.height == source_height;
  spdlog::debug(
      "recording start request: mode={} crop={}x{}+{},{} source={}x{} "
      "format={} range={}",
      full_frame ? "full" : "region", shown.width, shown.height, shown.x,
      shown.y, source_width, source_height, pixel_format_name(input),
      range_name(input.color_range));
  if (!valid_frame(input)) {
    constexpr char message[] = "recording requires a valid CPU VideoFrame";
    {
      std::lock_guard lock(mutex_);
      status_.error = message;
    }
    log_recording_result("start", "rejected", {}, {}, 0, message);
    return false;
  }
  if (crop && !even_crop_inside(*crop, source_width, source_height)) {
    constexpr char message[] =
        "recording crop must be inside the source frame with even coordinates "
        "and dimensions";
    {
      std::lock_guard lock(mutex_);
      status_.error = message;
    }
    log_recording_result("start", "rejected", {}, {}, 0, message);
    return false;
  }
  ensure_worker();
  std::filesystem::path path;
  bool busy = false;
  {
    std::lock_guard lock(mutex_);
    path = status_.output_path;
    if (status_.state != RecordingState::idle &&
        status_.state != RecordingState::failed) {
      busy = true;
    } else {
      status_.error.clear();
      status_.state = RecordingState::starting;
      start_ = std::pair{input, crop};
    }
  }
  if (busy) {
    log_recording_result("start", "rejected", {}, path, 0, "busy");
    return false;
  }
  wake_.notify_one();
  return true;
}
bool Recording::pause() {
  std::filesystem::path path;
  bool accepted = false;
  {
    std::lock_guard lock(mutex_);
    path = status_.output_path;
    if (status_.state == RecordingState::recording) {
      status_.state = RecordingState::paused;
      paused_at_ = std::chrono::steady_clock::now();
      accepted = true;
    }
  }
  log_recording_result("pause", accepted ? "ok" : "rejected", {}, path, 0, {});
  return accepted;
}
bool Recording::resume() {
  std::filesystem::path path;
  bool accepted = false;
  {
    std::lock_guard lock(mutex_);
    path = status_.output_path;
    if (status_.state == RecordingState::paused) {
      paused_duration_ += std::chrono::steady_clock::now() - paused_at_;
      status_.state = RecordingState::recording;
      accepted = true;
    }
  }
  log_recording_result("resume", accepted ? "ok" : "rejected", {}, path, 0, {});
  return accepted;
}
bool Recording::stop() {
  std::filesystem::path path;
  const char* result = "rejected";
  {
    std::lock_guard lock(mutex_);
    path = status_.output_path;
    if (status_.state == RecordingState::failed) {
      status_.state = RecordingState::idle;
      status_.error.clear();
      result = "reset";
    } else if (status_.state == RecordingState::idle) {
      result = "idle";
    } else if (status_.state == RecordingState::recording ||
               status_.state == RecordingState::paused) {
      status_.state = RecordingState::stopping;
      stop_requested_ = true;
      result = "requested";
    }
  }
  if (std::string_view{result} == "requested") wake_.notify_one();
  log_recording_result("stop", result, {}, path, 0, {});
  return result == std::string_view{"requested"} ||
         result == std::string_view{"reset"} ||
         result == std::string_view{"idle"};
}
void Recording::ensure_worker() {
  if (!worker_.joinable())
    worker_ = std::jthread([this](std::stop_token token) { worker(token); });
}
void Recording::worker(std::stop_token token) {
  SwsContextPtr scroll_convert(nullptr, sws_freeContext);
  while (!token.stop_requested()) {
    std::optional<std::pair<VideoFrame, std::optional<FrameCrop>>> begin;
    std::optional<VideoFrame> next, scroll_next;
    std::optional<ScrollRequest> scroll_begin;
    std::optional<std::pair<VideoFrame, std::optional<FrameCrop>>> shot;
    bool stop = false, scroll_finish = false, scroll_cancel = false;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, token, [this] {
        return start_ || snapshot_ || latest_ || stop_requested_ ||
               scroll_start_ || scroll_latest_ || scroll_finish_ ||
               scroll_cancel_;
      });
      if (token.stop_requested()) break;
      begin = std::move(start_);
      start_.reset();
      shot = std::move(snapshot_);
      snapshot_.reset();
      stop = std::exchange(stop_requested_, false);
      next = std::move(latest_);
      latest_.reset();
      scroll_begin = std::move(scroll_start_);
      scroll_start_.reset();
      scroll_next = std::move(scroll_latest_);
      scroll_latest_.reset();
      scroll_finish = std::exchange(scroll_finish_, false);
      scroll_cancel = std::exchange(scroll_cancel_, false);
    }
    if (begin) {
      const bool begun = start_now(begin->first, begin->second);
      if (begun) {
        std::lock_guard lock(mutex_);
        status_.state = RecordingState::recording;
        paused_duration_ = {};
      }
    }
    if (shot) (void)snapshot_now(shot->first, shot->second);
    if (scroll_cancel) {
      scroll_stitcher_.reset();
      {
        std::lock_guard lock(mutex_);
        status_.scroll_state = ScrollCaptureState::idle;
      }
      spdlog::debug("scroll state: idle reason=cancel");
    }
    if (scroll_begin) {
      auto pixels =
          crop_rgb(scroll_begin->frame, scroll_begin->crop, scroll_convert);
      scroll_stitcher_.emplace();
      scroll_crop_ = scroll_begin->crop;
      scroll_sequence_ = 1;
      StitchUpdate update{StitchResult::invalid, 0};
      if (!pixels.empty()) {
        update =
            scroll_stitcher_->add(pixels, scroll_begin->crop.width,
                                  scroll_begin->crop.height, scroll_sequence_);
      }
      std::string error;
      bool capturing = false;
      {
        std::lock_guard lock(mutex_);
        if (update.result == StitchResult::started) {
          status_.scroll_state = ScrollCaptureState::capturing;
          status_.scroll_width = scroll_begin->crop.width;
          status_.scroll_height = scroll_begin->crop.height;
          capturing = true;
        } else {
          status_.scroll_state = ScrollCaptureState::failed;
          error = pixels.empty()
                      ? "Scrolling crop is outside the source frame."
                      : std::string("Invalid scrolling capture region (") +
                            stitch_result_name(update.result) + ").";
          status_.scroll_error = error;
          scroll_stitcher_.reset();
        }
      }
      if (capturing) {
        spdlog::debug("scroll state: capturing size={}x{}",
                      scroll_begin->crop.width, scroll_begin->crop.height);
      } else {
        spdlog::debug("scroll state: failed error={}", error);
      }
    }
    if (scroll_next && scroll_stitcher_) {
      auto pixels = crop_rgb(*scroll_next, scroll_crop_, scroll_convert);
      const auto sequence = ++scroll_sequence_;
      StitchUpdate update{StitchResult::invalid, 0};
      if (!pixels.empty()) {
        update = scroll_stitcher_->add(pixels, scroll_crop_.width,
                                       scroll_crop_.height, sequence);
      }
      bool limited = false;
      unsigned height = 0;
      {
        std::lock_guard lock(mutex_);
        if (scroll_stitcher_) height = scroll_stitcher_->height();
        status_.scroll_height = height;
        if (update.result == StitchResult::size_limit) {
          status_.scroll_state = ScrollCaptureState::failed;
          status_.scroll_error = "Scrolling screenshot reached its size limit.";
          limited = true;
        }
      }
      spdlog::debug(
          "scroll sample: result={} offset={} sequence={} size={}x{}",
          pixels.empty() ? "empty-crop" : stitch_result_name(update.result),
          update.displacement, sequence, scroll_crop_.width, height);
      if (limited)
        spdlog::debug(
            "scroll state: failed error=Scrolling screenshot reached its "
            "size limit.");
    }
    if (scroll_finish && scroll_stitcher_) {
      const auto dir = downloads_directory();
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      const auto path = timestamped(dir, ".jpeg");
      const unsigned width = scroll_stitcher_->width();
      const unsigned height = scroll_stitcher_->height();
      const bool ok = !ec && write_rgb_jpeg(scroll_stitcher_->pixels(), width,
                                            height, path);
      const std::string failure =
          ec ? "create Downloads directory: " + ec.message()
             : "Could not write scrolling JPEG.";
      scroll_stitcher_.reset();
      {
        std::lock_guard lock(mutex_);
        status_.scroll_state =
            ok ? ScrollCaptureState::idle : ScrollCaptureState::failed;
        if (ok)
          status_.last_scroll_path = path;
        else
          status_.scroll_error = failure;
      }
      if (ok) {
        spdlog::debug("scroll saved: path={} width={} height={}",
                      path_text(path), width, height);
      } else {
        spdlog::debug("scroll state: failed error={}", failure);
      }
    }
    if (stop && impl_) {
      int result = avcodec_send_frame(impl_->codec, nullptr);
      AVPacket* packet = av_packet_alloc();
      while (result >= 0) {
        result = avcodec_receive_packet(impl_->codec, packet);
        if (result == AVERROR_EOF || result == AVERROR(EAGAIN)) {
          result = 0;
          break;
        }
        if (result >= 0) {
          av_packet_rescale_ts(packet, impl_->codec->time_base,
                               impl_->stream->time_base);
          packet->stream_index = impl_->stream->index;
          result = av_interleaved_write_frame(impl_->format, packet);
          av_packet_unref(packet);
        }
      }
      av_packet_free(&packet);
      if (result >= 0) result = av_write_trailer(impl_->format);
      if (impl_->format->pb) avio_closep(&impl_->format->pb);
      const std::string encoder = impl_->encoder_name;
      const unsigned frames = impl_->frames_written;
      sws_freeContext(impl_->convert);
      avcodec_free_context(&impl_->codec);
      avformat_free_context(impl_->format);
      delete impl_;
      impl_ = nullptr;
      std::filesystem::path path;
      {
        std::lock_guard lock(mutex_);
        status_.state =
            result < 0 ? RecordingState::failed : RecordingState::idle;
        path = status_.output_path;
      }
      log_recording_result("stop", result < 0 ? "failed" : "ok", encoder, path,
                           frames, result < 0 ? av_error(result) : "");
    }
    if (next) {
      bool accept = false;
      {
        std::lock_guard lock(mutex_);
        accept = status_.state == RecordingState::recording && impl_;
      }
      if (accept) (void)write_frame(*next);
    }
  }
  if (!impl_) {
    std::lock_guard lock(mutex_);
    if (status_.state == RecordingState::recording)
      status_.state = RecordingState::idle;
  }
  if (impl_) {  // flush directly during destruction
    int result = avcodec_send_frame(impl_->codec, nullptr);
    AVPacket* packet = av_packet_alloc();
    while (result >= 0) {
      result = avcodec_receive_packet(impl_->codec, packet);
      if (result == AVERROR_EOF || result == AVERROR(EAGAIN)) {
        result = 0;
        break;
      }
      if (result >= 0) {
        av_packet_rescale_ts(packet, impl_->codec->time_base,
                             impl_->stream->time_base);
        packet->stream_index = impl_->stream->index;
        result = av_interleaved_write_frame(impl_->format, packet);
        av_packet_unref(packet);
      }
    }
    av_packet_free(&packet);
    if (result >= 0) result = av_write_trailer(impl_->format);
    if (impl_->format->pb) avio_closep(&impl_->format->pb);
    const std::string encoder = impl_->encoder_name;
    const unsigned frames = impl_->frames_written;
    sws_freeContext(impl_->convert);
    avcodec_free_context(&impl_->codec);
    avformat_free_context(impl_->format);
    delete impl_;
    impl_ = nullptr;
    std::filesystem::path path;
    {
      std::lock_guard lock(mutex_);
      status_.state =
          result < 0 ? RecordingState::failed : RecordingState::idle;
      path = status_.output_path;
    }
    log_recording_result("shutdown", result < 0 ? "failed" : "ok", encoder,
                         path, frames, result < 0 ? av_error(result) : "");
  }
}
void Recording::shutdown() noexcept {
  if (worker_.joinable()) {
    worker_.request_stop();
    wake_.notify_all();
    worker_.join();
  }
}
}  // namespace kvmux
