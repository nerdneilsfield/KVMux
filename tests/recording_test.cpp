#include "app/recording.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
}

using namespace std::chrono_literals;

static void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

static kvmux::VideoFrame frame(int width = 64, int height = 64) {
  kvmux::VideoFrame value;
  value.arrival = std::chrono::steady_clock::now();
  value.frame = {av_frame_alloc(), [](AVFrame* p) { av_frame_free(&p); }};
  value.frame->format = AV_PIX_FMT_YUV420P;
  value.frame->width = width;
  value.frame->height = height;
  require(av_frame_get_buffer(value.frame.get(), 32) >= 0, "frame buffer");
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x)
      value.frame->data[0][y * value.frame->linesize[0] + x] =
          static_cast<std::uint8_t>(32 + 192 * x / width);
  for (int plane = 1; plane < 3; ++plane)
    for (int y = 0; y < height / 2; ++y)
      std::fill_n(value.frame->data[plane] + y * value.frame->linesize[plane],
                  width / 2, 128);
  return value;
}

// Stride is wider than the width, and the limited range lives on VideoFrame.
// That is the CPU frame process_raw builds from macOS 420v. AVFrame.color_range
// stays unspecified, as it does in that path.
static kvmux::VideoFrame nv12_frame() {
  constexpr int width = 64;
  constexpr int height = 48;
  constexpr int stride = 96;
  kvmux::VideoFrame value;
  value.arrival = std::chrono::steady_clock::now();
  value.color_range = kvmux::ColorRange::limited;
  value.frame = {av_frame_alloc(),
                 [](AVFrame* frame) { av_frame_free(&frame); }};
  auto* frame = value.frame.get();
  require(frame != nullptr, "nv12 frame");
  frame->format = AV_PIX_FMT_NV12;
  frame->width = width;
  frame->height = height;
  const auto bytes = static_cast<std::size_t>(stride) * (height + height / 2);
  auto* storage = static_cast<std::uint8_t*>(av_malloc(bytes));
  require(storage != nullptr, "nv12 storage");
  std::fill_n(storage, bytes, 0);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      storage[static_cast<std::size_t>(y * stride + x)] =
          static_cast<std::uint8_t>(16 + (219 * x) / (width - 1));
    }
  }
  const auto chroma = static_cast<std::size_t>(stride) * height;
  std::fill_n(storage + chroma, static_cast<std::size_t>(stride) * (height / 2),
              128);
  frame->buf[0] =
      av_buffer_create(storage, bytes, av_buffer_default_free, nullptr, 0);
  if (frame->buf[0] == nullptr) {
    av_free(storage);
    require(false, "nv12 buffer");
  }
  frame->data[0] = storage;
  frame->data[1] = storage + chroma;
  frame->linesize[0] = stride;
  frame->linesize[1] = stride;
  frame->extended_data = frame->data;
  return value;
}

static kvmux::RecordingStatus wait_snapshot(
    kvmux::Recording& recording, const std::filesystem::path& previous = {}) {
  for (int i = 0; i < 500; ++i) {
    auto status = recording.status();
    if ((!status.last_snapshot_path.empty() &&
         status.last_snapshot_path != previous) ||
        !status.snapshot_error.empty())
      return status;
    std::this_thread::sleep_for(10ms);
  }
  return recording.status();
}

static kvmux::RecordingStatus wait(kvmux::Recording& recording,
                                   kvmux::RecordingState desired) {
  for (int i = 0; i < 200; ++i) {
    auto status = recording.status();
    if (status.state == desired ||
        status.state == kvmux::RecordingState::failed)
      return status;
    std::this_thread::sleep_for(10ms);
  }
  return recording.status();
}

static void parse_output(const std::filesystem::path& path,
                         int expected_width = 0, int expected_height = 0) {
  AVFormatContext* context = nullptr;
  const auto utf8_path = path.u8string();
  require(avformat_open_input(&context,
                              reinterpret_cast<const char*>(utf8_path.c_str()),
                              nullptr, nullptr) >= 0,
          "open output");
  require(avformat_find_stream_info(context, nullptr) >= 0, "stream info");
  bool found = false;
  for (unsigned i = 0; i < context->nb_streams; ++i)
    found |= context->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
  if (expected_width) {
    const AVCodecParameters* parameters = nullptr;
    for (unsigned i = 0; i < context->nb_streams; ++i)
      if (context->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        parameters = context->streams[i]->codecpar;
    require(parameters && parameters->width == expected_width &&
                parameters->height == expected_height,
            "crop dimensions");
  }
  avformat_close_input(&context);
  require(found && std::filesystem::file_size(path) > 0,
          "video stream/output bytes");
}

static void require_nonblank_jpeg(const std::filesystem::path& path) {
  AVFormatContext* format = nullptr;
  const auto utf8_path = path.u8string();
  require(avformat_open_input(&format,
                              reinterpret_cast<const char*>(utf8_path.c_str()),
                              nullptr, nullptr) >= 0,
          "open JPEG");
  const int stream =
      av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  require(stream >= 0, "find JPEG stream");
  const auto* codec =
      avcodec_find_decoder(format->streams[stream]->codecpar->codec_id);
  AVCodecContext* context = codec ? avcodec_alloc_context3(codec) : nullptr;
  require(context, "allocate JPEG decoder");
  require(avcodec_parameters_to_context(
              context, format->streams[stream]->codecpar) >= 0 &&
              avcodec_open2(context, codec, nullptr) >= 0,
          "open JPEG decoder");
  AVPacket* packet = av_packet_alloc();
  AVFrame* decoded = av_frame_alloc();
  bool received = false;
  while (!received && av_read_frame(format, packet) >= 0) {
    if (packet->stream_index == stream &&
        avcodec_send_packet(context, packet) >= 0)
      received = avcodec_receive_frame(context, decoded) >= 0;
    av_packet_unref(packet);
  }
  require(received && decoded->data[0], "decode JPEG");
  std::uint8_t minimum = 255, maximum = 0;
  for (int y = 0; y < decoded->height; ++y)
    for (int x = 0; x < decoded->width; ++x) {
      const auto value = decoded->data[0][y * decoded->linesize[0] + x];
      minimum = std::min(minimum, value);
      maximum = std::max(maximum, value);
    }
  av_frame_free(&decoded);
  av_packet_free(&packet);
  avcodec_free_context(&context);
  avformat_close_input(&format);
  require(maximum - minimum > 32, "JPEG contains visible image content");
}

int main() {
  const auto directory =
      std::filesystem::temp_directory_path() / u8"kvmux-recording-测试";
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  kvmux::Recording recording(directory);
  auto first = frame();

  require(recording.start(first), "queue start");
  const auto started = wait(recording, kvmux::RecordingState::recording);
  require(started.state == kvmux::RecordingState::recording,
          started.error.c_str());
  require(started.output_path.extension() == ".mp4", "recording path");

  require(recording.snapshot(first), "queue snapshot during recording");
  kvmux::RecordingStatus snapshot;
  for (int i = 0; i < 200; ++i) {
    snapshot = recording.status();
    if (!snapshot.last_snapshot_path.empty() ||
        !snapshot.snapshot_error.empty())
      break;
    std::this_thread::sleep_for(10ms);
  }
  require(snapshot.snapshot_error.empty(), snapshot.snapshot_error.c_str());
  require(snapshot.last_snapshot_path.extension() == ".jpeg", "snapshot path");
  require(snapshot.output_path == started.output_path,
          "snapshot preserves recording path");
  parse_output(snapshot.last_snapshot_path);

  const auto full_snapshot_path = snapshot.last_snapshot_path;
  require(recording.snapshot(first, kvmux::FrameCrop{8, 12, 24, 20}),
          "queue crop snapshot");
  for (int i = 0; i < 200; ++i) {
    snapshot = recording.status();
    if (snapshot.last_snapshot_path != full_snapshot_path ||
        !snapshot.snapshot_error.empty())
      break;
    std::this_thread::sleep_for(10ms);
  }
  require(snapshot.snapshot_error.empty(), snapshot.snapshot_error.c_str());
  parse_output(snapshot.last_snapshot_path, 24, 20);

  kvmux::Recording high_resolution(directory);
  auto four_k = frame(3840, 2160);
  require(high_resolution.snapshot(four_k), "queue 4K full snapshot");
  auto four_k_snapshot = wait_snapshot(high_resolution);
  require(four_k_snapshot.snapshot_error.empty(),
          four_k_snapshot.snapshot_error.c_str());
  parse_output(four_k_snapshot.last_snapshot_path, 3840, 2160);
  require_nonblank_jpeg(four_k_snapshot.last_snapshot_path);
  const auto four_k_full_path = four_k_snapshot.last_snapshot_path;
  require(
      high_resolution.snapshot(four_k, kvmux::FrameCrop{641, 359, 1280, 720}),
      "queue 4K region snapshot");
  four_k_snapshot = wait_snapshot(high_resolution, four_k_full_path);
  require(four_k_snapshot.snapshot_error.empty(),
          four_k_snapshot.snapshot_error.c_str());
  parse_output(four_k_snapshot.last_snapshot_path, 1280, 720);
  require_nonblank_jpeg(four_k_snapshot.last_snapshot_path);
  high_resolution.shutdown();

  require(recording.snapshot(first, kvmux::FrameCrop{60, 0, 8, 8}),
          "queue invalid crop");
  for (int i = 0; i < 200 && recording.status().snapshot_error.empty(); ++i)
    std::this_thread::sleep_for(10ms);
  require(!recording.status().snapshot_error.empty(), "reject invalid crop");
  require(recording.append(frame()), "append");
  require(recording.pause(), "pause");
  require(!recording.append(frame()), "paused rejects frame");
  std::this_thread::sleep_for(20ms);
  require(recording.stop(), "stop paused");
  const auto done = wait(recording, kvmux::RecordingState::idle);
  require(done.state == kvmux::RecordingState::idle, done.error.c_str());
  require(done.output_path == started.output_path, "completed recording path");
  parse_output(done.output_path);

  kvmux::Recording cropped(directory);
  auto cropped_first = frame();
  require(cropped.start(cropped_first, kvmux::FrameCrop{8, 12, 24, 20}),
          "queue cropped start");
  const auto cropped_started = wait(cropped, kvmux::RecordingState::recording);
  require(cropped_started.state == kvmux::RecordingState::recording,
          cropped_started.error.c_str());
  require(cropped_started.width == 24 && cropped_started.height == 20,
          "cropped recording status dimensions");
  require(cropped.append(frame()), "append cropped recording");
  require(cropped.stop(), "stop cropped recording");
  const auto cropped_done = wait(cropped, kvmux::RecordingState::idle);
  require(cropped_done.state == kvmux::RecordingState::idle,
          cropped_done.error.c_str());
  parse_output(cropped_done.output_path, 24, 20);
  cropped.shutdown();
  recording.shutdown();

  kvmux::Recording nv12(directory);
  auto nv12_first = nv12_frame();
  require(nv12.snapshot(nv12_first), "queue nv12 snapshot");
  const auto nv12_shot = wait_snapshot(nv12);
  require(nv12_shot.snapshot_error.empty(), nv12_shot.snapshot_error.c_str());
  parse_output(nv12_shot.last_snapshot_path, 64, 48);
  require_nonblank_jpeg(nv12_shot.last_snapshot_path);
  require(nv12.start(nv12_first), "queue nv12 recording");
  const auto nv12_started = wait(nv12, kvmux::RecordingState::recording);
  require(nv12_started.state == kvmux::RecordingState::recording,
          nv12_started.error.c_str());
  require(nv12.append(nv12_frame()), "append nv12");
  require(nv12.stop(), "stop nv12");
  const auto nv12_done = wait(nv12, kvmux::RecordingState::idle);
  require(nv12_done.state == kvmux::RecordingState::idle,
          nv12_done.error.c_str());
  parse_output(nv12_done.output_path, 64, 48);

  kvmux::Recording nv12_crop(directory);
  require(nv12_crop.start(nv12_frame(), kvmux::FrameCrop{8, 4, 32, 24}),
          "queue nv12 cropped recording");
  const auto nv12_crop_started =
      wait(nv12_crop, kvmux::RecordingState::recording);
  require(nv12_crop_started.state == kvmux::RecordingState::recording,
          nv12_crop_started.error.c_str());
  require(nv12_crop_started.width == 32 && nv12_crop_started.height == 24,
          "nv12 cropped recording dimensions");
  require(nv12_crop.append(nv12_frame()), "append nv12 crop");
  require(nv12_crop.stop(), "stop nv12 crop");
  const auto nv12_crop_done = wait(nv12_crop, kvmux::RecordingState::idle);
  require(nv12_crop_done.state == kvmux::RecordingState::idle,
          nv12_crop_done.error.c_str());
  parse_output(nv12_crop_done.output_path, 32, 24);
  nv12_crop.shutdown();
  nv12.shutdown();
  std::filesystem::remove_all(directory);
  std::cout << "JPEG snapshots and full/cropped MP4 outputs parsed\n";
}
