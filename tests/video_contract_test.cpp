#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

#include "video/capture_sample.hpp"
#include "video/video_mailbox.hpp"
#include "video/video_processor.hpp"

namespace {

void require(bool value, const char* message) {
  if (!value) {
    throw std::runtime_error(message);
  }
}

}  // namespace

int main(int argc, char** argv) {
  using namespace kvmux;
  const auto now = std::chrono::steady_clock::now();

  std::vector<std::uint8_t> raw_bytes(26, 0x4a);
  const std::array planes{PlaneLayout{4, 8, 6, 3}};
  auto raw = CaptureSample::make_raw(2, 10, now, 3, 3, PixelFormat::yuy2,
                                     planes, raw_bytes);
  require(raw.has_value() && raw->raw->bytes.data() != raw_bytes.data(),
          "raw sample owns a copy");
  raw_bytes[4] = 0;
  require(raw->raw->bytes[4] == 0x4a, "source mutation cannot change sample");

  const std::array bad_plane{PlaneLayout{20, 8, 8, 2}};
  require(!CaptureSample::make_raw(2, 11, now, 3, 3, PixelFormat::yuy2,
                                   bad_plane, raw_bytes),
          "out-of-bounds plane rejected");
  require(!CaptureSample::make_raw(2, 12, now, 1921, 1, PixelFormat::yuy2,
                                   planes, raw_bytes),
          "oversized dimensions rejected");

  const std::array<std::uint8_t, 4> jpeg{0xff, 0xd8, 0xff, 0xd9};
  auto mjpeg = CaptureSample::make_mjpeg(2, 13, now, 640, 480, jpeg);
  require(mjpeg.has_value() && mjpeg->mjpeg->payload_size == jpeg.size(),
          "mjpeg accepted");
  require(mjpeg->mjpeg->bytes.size() == jpeg.size() + kInputPaddingBytes,
          "mjpeg padding allocated");
  for (std::size_t i = jpeg.size(); i < mjpeg->mjpeg->bytes.size(); ++i) {
    require(mjpeg->mjpeg->bytes[i] == 0, "mjpeg padding is zero");
  }

  GenerationMailbox<CaptureSample> mailbox;
  mailbox.set_generation(2);
  mailbox.publish(std::move(*raw));
  mailbox.publish(std::move(*mjpeg));
  require(mailbox.overwritten() == 1, "new sample overwrites old sample");
  auto newest = mailbox.take();
  require(newest && newest->sequence == 13, "consumer gets newest sample");
  require(!mailbox.take(), "sample is consumed once");

  auto stale = CaptureSample::make_mjpeg(2, 14, now, 640, 480, jpeg);
  mailbox.set_generation(3);
  mailbox.publish(std::move(*stale));
  require(!mailbox.take(), "stale generation is rejected");

  const std::array<std::uint8_t, 8> yuy2{16, 128, 235, 128, 81, 90, 145, 240};
  const std::array packed{PlaneLayout{0, 4, 4, 2}};
  auto processable = CaptureSample::make_raw(4, 20, now, 2, 2,
                                             PixelFormat::yuy2, packed, yuy2);
  VideoProcessor processor;
  auto frame = processor.process(*processable);
  require(frame && frame->frame && frame->frame->width == 2 &&
              frame->frame->height == 2 && frame->generation == 4 &&
              frame->sequence == 20,
          "raw frame is copied into an owned AVFrame");
  require(frame->frame->data[0][0] == 16 &&
              frame->frame->data[0][frame->frame->linesize[0]] == 81,
          "raw stride rows are preserved");

  CaptureSample retained;
  retained.generation = 4;
  retained.sequence = 21;
  retained.arrival = now;
  retained.width = 2;
  retained.height = 2;
  retained.decoded = frame->frame;
  auto retained_frame = processor.process(retained);
  require(retained_frame && retained_frame->frame == frame->frame,
          "decoded sample retains owned frame without copying pixels");
  retained.raw = processable->raw;
  require(!processor.process(retained),
          "mixed decoded and raw payload rejected");
  retained.raw.reset();
  retained.width = 3;
  require(!processor.process(retained), "decoded dimensions validated");

  const std::array reversed{PlaneLayout{4, -4, 4, 2}};
  auto bottom_up = CaptureSample::make_raw(4, 21, now, 2, 2, PixelFormat::yuy2,
                                           reversed, yuy2);
  auto reversed_frame = processor.process(*bottom_up);
  require(
      reversed_frame && reversed_frame->frame->data[0][0] == 81 &&
          reversed_frame->frame->data[0][reversed_frame->frame->linesize[0]] ==
              16,
      "negative source stride reverses rows safely");

  auto invalid_jpeg = CaptureSample::make_mjpeg(4, 22, now, 640, 480, jpeg);
  require(!processor.process(*invalid_jpeg) && !processor.last_error().empty(),
          "invalid MJPEG fails with a diagnostic");

  require(argc == 2, "MJPEG fixture path is required");
  std::ifstream input(argv[1], std::ios::binary);
  const std::vector<std::uint8_t> jpeg_fixture{
      std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  auto compressed = CaptureSample::make_mjpeg(4, 23, now, 16, 16, jpeg_fixture);
  auto decoded = processor.process(*compressed);
  require(decoded && decoded->frame->width == 16 &&
              decoded->frame->height == 16 && decoded->sequence == 23,
          "complete MJPEG image decodes to an owned AVFrame");
  require(decoded->frame->color_range == AVCOL_RANGE_JPEG &&
              decoded->color_range == ColorRange::full,
          "MJPEG full-range metadata reaches the renderer without "
          "limited-range conversion");

  for (const auto format :
       {AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_YUVJ444P}) {
    AVFrame legacy{};
    legacy.format = format;
    legacy.color_range = AVCOL_RANGE_UNSPECIFIED;
    require(frame_color_range(legacy) == ColorRange::full,
            "legacy JPEG planar formats imply full range without metadata");
    legacy.color_range = AVCOL_RANGE_MPEG;
    require(frame_color_range(legacy) == ColorRange::limited,
            "explicit range metadata takes precedence over legacy format "
            "inference");
  }

  return EXIT_SUCCESS;
}
