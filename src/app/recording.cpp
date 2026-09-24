#include "app/recording.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
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
#include <libswscale/swscale.h>
}

namespace kvmux {
namespace {
std::string av_error(int code) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
  av_strerror(code, text.data(), text.size());
  return text.data();
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
  if (!valid_frame(input)) {
    std::lock_guard lock(mutex_);
    status_.snapshot_error = "snapshot requires a valid CPU VideoFrame";
    return false;
  }
  const unsigned source_width = static_cast<unsigned>(input.frame->width);
  const unsigned source_height = static_cast<unsigned>(input.frame->height);
  const FrameCrop crop =
      requested_crop.value_or(FrameCrop{0, 0, source_width, source_height});
  if (!crop.width || !crop.height || crop.x >= source_width ||
      crop.y >= source_height || crop.width > source_width - crop.x ||
      crop.height > source_height - crop.y) {
    std::lock_guard lock(mutex_);
    status_.snapshot_error = "snapshot crop is outside the source frame";
    return false;
  }
  const auto directory = downloads_directory();
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    {
      std::lock_guard lock(mutex_);
      status_.snapshot_error = "create Downloads directory: " + ec.message();
    }
    return false;
  }
  const auto path = timestamped(directory, ".jpeg");
  const auto* encoder = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
  if (!encoder) {
    {
      std::lock_guard lock(mutex_);
      status_.snapshot_error = "FFmpeg MJPEG encoder unavailable";
    }
    return false;
  }
  AVCodecContext* context = avcodec_alloc_context3(encoder);
  AVFrame* image = av_frame_alloc();
  AVPacket* packet = av_packet_alloc();
  if (!context || !image || !packet) {
    avcodec_free_context(&context);
    av_frame_free(&image);
    av_packet_free(&packet);
    {
      std::lock_guard lock(mutex_);
      status_.snapshot_error = "allocate JPEG encoder";
    }
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
  const auto temporary = path.string() + ".part";
  if (result >= 0) {
    std::ofstream file(temporary, std::ios::binary);
    if (!file)
      result = AVERROR(EIO);
    else {
      file.write(reinterpret_cast<const char*>(packet->data), packet->size);
      if (!file) result = AVERROR(EIO);
    }
  }
  if (result >= 0) {
    std::error_code rename_error;
    std::filesystem::rename(temporary, path, rename_error);
    if (rename_error) result = AVERROR(EIO);
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
    {
      std::lock_guard lock(mutex_);
      status_.snapshot_error = "write JPEG: " + av_error(result);
    }
    return false;
  }
  {
    std::lock_guard lock(mutex_);
    status_.last_snapshot_path = path;
    status_.snapshot_error.clear();
  }
  return true;
}

static std::vector<std::uint8_t> crop_rgb(const VideoFrame& input,
                                          FrameCrop crop) {
  if (!valid_frame(input) || !crop.width || !crop.height ||
      crop.x + crop.width > static_cast<unsigned>(input.frame->width) ||
      crop.y + crop.height > static_cast<unsigned>(input.frame->height))
    return {};
  std::vector<std::uint8_t> rgb(static_cast<std::size_t>(crop.width) *
                                crop.height * 3);
  auto* convert =
      sws_getContext(input.frame->width, input.frame->height,
                     static_cast<AVPixelFormat>(input.frame->format),
                     input.frame->width, input.frame->height, AV_PIX_FMT_RGB24,
                     SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (!convert) return {};
  std::vector<std::uint8_t> full(static_cast<std::size_t>(input.frame->width) *
                                 input.frame->height * 3);
  std::uint8_t* data[]{full.data()};
  int stride[]{input.frame->width * 3};
  if (sws_scale(convert, input.frame->data, input.frame->linesize, 0,
                input.frame->height, data, stride) < 0) {
    sws_freeContext(convert);
    return {};
  }
  sws_freeContext(convert);
  for (unsigned y = 0; y < crop.height; ++y)
    std::copy_n(full.data() +
                    ((crop.y + y) * static_cast<unsigned>(input.frame->width) +
                     crop.x) *
                        3,
                static_cast<std::size_t>(crop.width) * 3,
                rgb.data() + static_cast<std::size_t>(y) * crop.width * 3);
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
  const auto temporary = path.string() + ".part";
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
  if (!valid_frame(first)) {
    fail("recording requires a valid CPU VideoFrame first=" +
         std::to_string(first.frame ? first.frame->width : -1));
    return false;
  }
  const auto directory = downloads_directory();
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    fail("create Downloads directory: " + ec.message());
    return false;
  }
  auto impl = std::make_unique<Impl>();
  const auto* encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
  if (!encoder) {
    fail("FFmpeg H.264 encoder unavailable");
    return false;
  }
  int result =
      avformat_alloc_output_context2(&impl->format, nullptr, "mp4", nullptr);
  if (result < 0 || !impl->format) {
    fail("allocate MP4 muxer: " + av_error(result));
    return false;
  }
  impl->stream = avformat_new_stream(impl->format, nullptr);
  impl->codec = avcodec_alloc_context3(encoder);
  if (!impl->stream || !impl->codec) {
    avformat_free_context(impl->format);
    fail("allocate MP4 stream/encoder");
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
    fail(
        "recording crop must be inside the source frame with even coordinates "
        "and dimensions");
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
  if (result >= 0)
    result =
        avio_open(&impl->format->pb, output.string().c_str(), AVIO_FLAG_WRITE);
  if (result >= 0) result = avformat_write_header(impl->format, nullptr);
  if (result < 0) {
    if (impl->format->pb) avio_closep(&impl->format->pb);
    avcodec_free_context(&impl->codec);
    avformat_free_context(impl->format);
    fail("start MP4 recording: " + av_error(result));
    return false;
  }
  impl->first_arrival = first.arrival;
  impl_ = impl.release();
  return write_frame(first);
}

bool Recording::write_frame(const VideoFrame& input) {
  auto& impl = *impl_;
  auto* source = input.frame.get();
  AVFrame* picture = av_frame_alloc();
  AVPacket* packet = av_packet_alloc();
  if (!picture || !packet) {
    av_frame_free(&picture);
    av_packet_free(&packet);
    fail("allocate MP4 frame/packet");
    return false;
  }
  picture->format = impl.codec->pix_fmt;
  picture->width = impl.codec->width;
  picture->height = impl.codec->height;
  int result = av_frame_get_buffer(picture, 32);
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
  av_frame_free(&picture);
  av_packet_free(&packet);
  if (result < 0) {
    fail("append MP4 frame: " + av_error(result));
    return false;
  }
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
  if (!valid_frame(input)) {
    std::lock_guard lock(mutex_);
    status_.snapshot_error = "snapshot requires a valid CPU VideoFrame";
    return false;
  }
  ensure_worker();
  {
    std::lock_guard lock(mutex_);
    status_.snapshot_error.clear();
    snapshot_ = std::pair{input, crop};
  }
  wake_.notify_one();
  return true;
}
bool Recording::start_scroll(const VideoFrame& input, FrameCrop crop) {
  if (!valid_frame(input)) return false;
  ensure_worker();
  std::lock_guard lock(mutex_);
  if (status_.scroll_state != ScrollCaptureState::idle) return false;
  status_.scroll_state = ScrollCaptureState::starting;
  status_.scroll_error.clear();
  status_.last_scroll_path.clear();
  status_.scroll_width = status_.scroll_height = 0;
  scroll_start_ = ScrollRequest{input, crop};
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
  std::lock_guard lock(mutex_);
  if (status_.scroll_state != ScrollCaptureState::capturing) return false;
  status_.scroll_state = ScrollCaptureState::finishing;
  scroll_finish_ = true;
  wake_.notify_one();
  return true;
}
void Recording::cancel_scroll() {
  std::lock_guard lock(mutex_);
  if (status_.scroll_state == ScrollCaptureState::idle) return;
  scroll_cancel_ = true;
  scroll_start_.reset();
  scroll_latest_.reset();
  wake_.notify_one();
}
bool Recording::start(const VideoFrame& input, std::optional<FrameCrop> crop) {
  if (!valid_frame(input)) {
    std::lock_guard lock(mutex_);
    status_.error = "recording requires a valid CPU VideoFrame";
    return false;
  }
  if (crop &&
      (!crop->width || !crop->height ||
       crop->x >= static_cast<unsigned>(input.frame->width) ||
       crop->y >= static_cast<unsigned>(input.frame->height) ||
       crop->width > static_cast<unsigned>(input.frame->width) - crop->x ||
       crop->height > static_cast<unsigned>(input.frame->height) - crop->y ||
       (crop->x | crop->y | crop->width | crop->height) & 1U)) {
    std::lock_guard lock(mutex_);
    status_.error =
        "recording crop must be inside the source frame with even coordinates "
        "and dimensions";
    return false;
  }
  ensure_worker();
  {
    std::lock_guard lock(mutex_);
    if (status_.state != RecordingState::idle &&
        status_.state != RecordingState::failed)
      return false;
    status_.error.clear();
    status_.state = RecordingState::starting;
    start_ = std::pair{input, crop};
  }
  wake_.notify_one();
  return true;
}
bool Recording::pause() {
  std::lock_guard lock(mutex_);
  if (status_.state != RecordingState::recording) return false;
  status_.state = RecordingState::paused;
  paused_at_ = std::chrono::steady_clock::now();
  return true;
}
bool Recording::resume() {
  std::lock_guard lock(mutex_);
  if (status_.state != RecordingState::paused) return false;
  paused_duration_ += std::chrono::steady_clock::now() - paused_at_;
  status_.state = RecordingState::recording;
  return true;
}
bool Recording::stop() {
  {
    std::lock_guard lock(mutex_);
    if (status_.state == RecordingState::failed) {
      status_.state = RecordingState::idle;
      status_.error.clear();
      return true;
    }
    if (status_.state != RecordingState::recording &&
        status_.state != RecordingState::paused)
      return status_.state == RecordingState::idle;
    status_.state = RecordingState::stopping;
    stop_requested_ = true;
  }
  wake_.notify_one();
  return true;
}
void Recording::ensure_worker() {
  if (!worker_.joinable())
    worker_ = std::jthread([this](std::stop_token token) { worker(token); });
}
void Recording::worker(std::stop_token token) {
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
      std::lock_guard lock(mutex_);
      status_.scroll_state = ScrollCaptureState::idle;
    }
    if (scroll_begin) {
      auto pixels = crop_rgb(scroll_begin->frame, scroll_begin->crop);
      scroll_stitcher_.emplace();
      scroll_crop_ = scroll_begin->crop;
      scroll_sequence_ = 1;
      const auto update =
          scroll_stitcher_->add(pixels, scroll_begin->crop.width,
                                scroll_begin->crop.height, scroll_sequence_);
      std::lock_guard lock(mutex_);
      if (update.result == StitchResult::started) {
        status_.scroll_state = ScrollCaptureState::capturing;
        status_.scroll_width = scroll_begin->crop.width;
        status_.scroll_height = scroll_begin->crop.height;
      } else {
        status_.scroll_state = ScrollCaptureState::failed;
        status_.scroll_error = "Invalid scrolling capture region.";
      }
    }
    if (scroll_next && scroll_stitcher_) {
      auto pixels = crop_rgb(*scroll_next, scroll_crop_);
      auto update = scroll_stitcher_->add(
          pixels, scroll_crop_.width, scroll_crop_.height, ++scroll_sequence_);
      std::lock_guard lock(mutex_);
      status_.scroll_height = scroll_stitcher_->height();
      if (update.result == StitchResult::size_limit) {
        status_.scroll_state = ScrollCaptureState::failed;
        status_.scroll_error = "Scrolling screenshot reached its size limit.";
      }
    }
    if (scroll_finish && scroll_stitcher_) {
      const auto dir = downloads_directory();
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      const auto path = timestamped(dir, ".jpeg");
      const bool ok = !ec && write_rgb_jpeg(scroll_stitcher_->pixels(),
                                            scroll_stitcher_->width(),
                                            scroll_stitcher_->height(), path);
      scroll_stitcher_.reset();
      std::lock_guard lock(mutex_);
      status_.scroll_state =
          ok ? ScrollCaptureState::idle : ScrollCaptureState::failed;
      if (ok)
        status_.last_scroll_path = path;
      else
        status_.scroll_error = "Could not write scrolling JPEG.";
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
      sws_freeContext(impl_->convert);
      avcodec_free_context(&impl_->codec);
      avformat_free_context(impl_->format);
      delete impl_;
      impl_ = nullptr;
      {
        std::lock_guard lock(mutex_);
        status_.state =
            result < 0 ? RecordingState::failed : RecordingState::idle;
      }
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
    sws_freeContext(impl_->convert);
    avcodec_free_context(&impl_->codec);
    avformat_free_context(impl_->format);
    delete impl_;
    impl_ = nullptr;
    {
      std::lock_guard lock(mutex_);
      status_.state =
          result < 0 ? RecordingState::failed : RecordingState::idle;
    }
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
