#include "video/video_processor.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
}

namespace kvmux {
namespace {

AvFramePtr own(AVFrame* frame) {
    return AvFramePtr(frame, [](AVFrame* value) { av_frame_free(&value); });
}

AVPixelFormat av_format(const PixelFormat format) {
    switch (format) {
    case PixelFormat::yuy2: return AV_PIX_FMT_YUYV422;
    case PixelFormat::uyvy: return AV_PIX_FMT_UYVY422;
    case PixelFormat::nv12: return AV_PIX_FMT_NV12;
    case PixelFormat::yuv420p: return AV_PIX_FMT_YUV420P;
    case PixelFormat::yuv422p: return AV_PIX_FMT_YUV422P;
    case PixelFormat::yuv444p: return AV_PIX_FMT_YUV444P;
    case PixelFormat::bgra: return AV_PIX_FMT_BGRA;
    case PixelFormat::rgba: return AV_PIX_FMT_RGBA;
    case PixelFormat::mjpeg:
    case PixelFormat::unknown: return AV_PIX_FMT_NONE;
    }
    return AV_PIX_FMT_NONE;
}

std::string av_error(const int error) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
    av_strerror(error, text.data(), text.size());
    return text.data();
}

VideoFrame metadata(const CaptureSample& sample, AvFramePtr frame) {
    ColorRange range = sample.color_range;
    ColorMatrix matrix = sample.color_matrix;
    if (range == ColorRange::unknown && frame) {
        if (frame->color_range == AVCOL_RANGE_JPEG) range = ColorRange::full;
        else if (frame->color_range == AVCOL_RANGE_MPEG) range = ColorRange::limited;
    }
    if (matrix == ColorMatrix::unknown && frame) {
        if (frame->colorspace == AVCOL_SPC_BT709) matrix = ColorMatrix::bt709;
        else if (frame->colorspace == AVCOL_SPC_SMPTE170M ||
                 frame->colorspace == AVCOL_SPC_BT470BG) matrix = ColorMatrix::bt601;
    }
    return {sample.generation, sample.sequence, sample.arrival,
            std::chrono::steady_clock::now(), sample.device_timestamp,
            sample.device_time_base_numerator, sample.device_time_base_denominator,
            sample.sample_aspect_ratio_numerator, sample.sample_aspect_ratio_denominator,
            range, matrix, std::move(frame)};
}

}  // namespace

VideoProcessor::VideoProcessor() = default;

VideoProcessor::~VideoProcessor() { avcodec_free_context(&decoder_); }

std::optional<VideoFrame> VideoProcessor::process(const CaptureSample& sample) {
    last_error_.clear();
    if (!valid_dimensions(sample.width, sample.height) ||
        (sample.raw.has_value() == sample.mjpeg.has_value())) {
        fail("sample must contain exactly one valid payload");
        return std::nullopt;
    }
    return sample.raw ? process_raw(sample) : process_mjpeg(sample);
}

std::optional<VideoFrame> VideoProcessor::process_raw(const CaptureSample& sample) {
    const auto& raw = *sample.raw;
    const auto format = av_format(raw.format);
    const int expected_planes = raw.format == PixelFormat::nv12 ? 2 :
        (raw.format == PixelFormat::yuv420p || raw.format == PixelFormat::yuv422p ||
         raw.format == PixelFormat::yuv444p ? 3 : 1);
    if (format == AV_PIX_FMT_NONE || raw.planes.size() != static_cast<std::size_t>(expected_planes)) {
        fail("unsupported raw pixel layout");
        return std::nullopt;
    }

    auto frame = own(av_frame_alloc());
    if (!frame) {
        fail("could not allocate AVFrame");
        return std::nullopt;
    }
    frame->format = format;
    frame->width = static_cast<int>(sample.width);
    frame->height = static_cast<int>(sample.height);
    if (const int result = av_frame_get_buffer(frame.get(), 32); result < 0) {
        fail("could not allocate raw frame: " + av_error(result));
        return std::nullopt;
    }

    for (int plane_index = 0; plane_index < expected_planes; ++plane_index) {
        const auto& plane = raw.planes[static_cast<std::size_t>(plane_index)];
        const auto* source = raw.bytes.data() + plane.offset;
        for (std::size_t row = 0; row < plane.rows; ++row) {
            const auto source_offset = static_cast<std::ptrdiff_t>(row) * plane.stride;
            auto* destination = frame->data[plane_index] +
                static_cast<std::ptrdiff_t>(row) * frame->linesize[plane_index];
            std::memcpy(destination, source + source_offset, plane.row_bytes);
        }
    }
    return metadata(sample, std::move(frame));
}

bool VideoProcessor::ensure_mjpeg_decoder() {
    if (decoder_) { return true; }
    const auto* codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    if (!codec) {
        fail("FFmpeg MJPEG decoder is unavailable");
        return false;
    }
    decoder_ = avcodec_alloc_context3(codec);
    if (!decoder_) {
        fail("could not allocate MJPEG decoder");
        return false;
    }
    decoder_->thread_count = 1;
    decoder_->max_pixels = static_cast<std::int64_t>(kMaxCaptureWidth) * kMaxCaptureHeight;
    if (const int result = avcodec_open2(decoder_, codec, nullptr); result < 0) {
        fail("could not open MJPEG decoder: " + av_error(result));
        avcodec_free_context(&decoder_);
        return false;
    }
    return true;
}

std::optional<VideoFrame> VideoProcessor::process_mjpeg(const CaptureSample& sample) {
    if (!ensure_mjpeg_decoder()) { return std::nullopt; }
    const auto& jpeg = *sample.mjpeg;
    AVPacket packet{};
    packet.data = const_cast<std::uint8_t*>(jpeg.bytes.data());
    packet.size = static_cast<int>(jpeg.payload_size);
    auto frame = own(av_frame_alloc());
    if (!frame) {
        fail("could not allocate decoded AVFrame");
        return std::nullopt;
    }
    int sent = avcodec_send_packet(decoder_, &packet);
    if (sent == AVERROR(EAGAIN)) {
        const int drained = avcodec_receive_frame(decoder_, frame.get());
        if (drained >= 0) av_frame_unref(frame.get());
        sent = avcodec_send_packet(decoder_, &packet);
    }
    if (sent < 0) {
        fail("MJPEG packet rejected: " + av_error(sent));
        return std::nullopt;
    }
    const int result = avcodec_receive_frame(decoder_, frame.get());
    if (result < 0) {
        fail("MJPEG frame unavailable: " + av_error(result));
        return std::nullopt;
    }
    if (!valid_dimensions(static_cast<std::uint32_t>(frame->width),
                          static_cast<std::uint32_t>(frame->height)) ||
        frame->width != static_cast<int>(sample.width) ||
        frame->height != static_cast<int>(sample.height)) {
        fail("decoded MJPEG dimensions do not match the sample");
        return std::nullopt;
    }
    return metadata(sample, std::move(frame));
}

void VideoProcessor::fail(std::string message) { last_error_ = std::move(message); }

}  // namespace kvmux
