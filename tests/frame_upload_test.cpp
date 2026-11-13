#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <thread>

#include "relay_test_fakes.hpp"
#include "render/video_renderer.hpp"
#include "support/diagnostics.hpp"
#include "video/video_pipeline.hpp"

namespace {

void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

void test_decision() {
  kvmux::UploadedFrame fresh;
  require(kvmux::frame_upload(fresh, 3, 8, kvmux::ColorOverride::automatic) ==
              kvmux::FrameUpload::new_frame,
          "a renderer uploads its first frame");
  kvmux::UploadedFrame uploaded{true, 3, 8, kvmux::ColorOverride::automatic};
  require(
      kvmux::frame_upload(uploaded, 3, 8, kvmux::ColorOverride::automatic) ==
          kvmux::FrameUpload::skip,
      "the same frame is not uploaded again");
  require(
      kvmux::frame_upload(uploaded, 3, 8, kvmux::ColorOverride::bt709_full) ==
          kvmux::FrameUpload::color_only,
      "a color override change uploads");
  require(
      kvmux::frame_upload(uploaded, 3, 9, kvmux::ColorOverride::automatic) ==
          kvmux::FrameUpload::new_frame,
      "a new sequence uploads");
  require(
      kvmux::frame_upload(uploaded, 4, 8, kvmux::ColorOverride::automatic) ==
          kvmux::FrameUpload::new_frame,
      "a new generation uploads");
  kvmux::UploadedFrame other;
  require(kvmux::frame_upload(other, 3, 8, kvmux::ColorOverride::automatic) ==
              kvmux::FrameUpload::new_frame,
          "a new renderer uploads the current frame");
}

void test_fake_capture(const char* jpeg_path) {
  std::ifstream input(jpeg_path, std::ios::binary);
  require(static_cast<bool>(input), "jpeg fixture opens");
  FakeCapture capture;
  capture.jpeg.assign(std::istreambuf_iterator<char>(input), {});
  require(!capture.jpeg.empty(), "jpeg fixture is not empty");

  kvmux::VideoPipeline pipeline(capture);
  pipeline.start(1);
  kvmux::Diagnostics diagnostics;
  kvmux::UploadedFrame cursor;
  std::optional<kvmux::VideoFrame> held;
  auto color = kvmux::ColorOverride::automatic;
  std::uint64_t presented = 0;
  std::uint64_t distinct = 0;
  std::uint64_t seen_generation = 0;
  std::uint64_t seen_sequence = 0;
  bool have_identity = false;
  bool recolored = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto newest = pipeline.take_latest_frame()) held = std::move(newest);
    if (!held) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    if (!have_identity || held->generation != seen_generation ||
        held->sequence != seen_sequence) {
      ++distinct;
      seen_generation = held->generation;
      seen_sequence = held->sequence;
      have_identity = true;
    }
    for (int tick = 0; tick < 5; ++tick) {
      if (!recolored && cursor.valid && tick == 1) {
        color = kvmux::ColorOverride::bt709_limited;
        recolored = true;
      }
      const auto decision =
          kvmux::frame_upload(cursor, held->generation, held->sequence, color);
      if (decision == kvmux::FrameUpload::skip) continue;
      if (decision == kvmux::FrameUpload::new_frame) {
        ++presented;
        diagnostics.record_sample_to_gpu_submit(std::chrono::milliseconds(1));
        diagnostics.record_present(held->generation, held->sequence);
      }
      cursor = {true, held->generation, held->sequence, color};
    }
  }
  pipeline.stop();
  const auto samples = diagnostics.snapshot().sample_to_gpu_submit.samples;
  if (presented < 3) {
    const auto snap = pipeline.snapshot();
    std::cerr << "processed=" << snap.processed_frames
              << " rejected=" << snap.rejected_samples
              << " error=" << snap.error << '\n';
  }
  require(presented >= 3, "FakeCapture produced new frames");
  require(recolored, "color override changed on a held frame");
  require(samples == presented, "sample_to_gpu_submit counts new frames");
  require(presented == distinct,
          "color override and repeat ticks are not new frames");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    test_decision();
    if (argc < 2) {
      std::cerr << "usage: frame_upload_test <jpeg>\n";
      return 2;
    }
    test_fake_capture(argv[1]);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
