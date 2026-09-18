#include "app/recording.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

using namespace std::chrono_literals;

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

static kvmux::VideoFrame frame() {
    kvmux::VideoFrame value;
    value.arrival = std::chrono::steady_clock::now();
    value.frame = {av_frame_alloc(), [](AVFrame* p) { av_frame_free(&p); }};
    value.frame->format = AV_PIX_FMT_YUV420P;
    value.frame->width = 64;
    value.frame->height = 64;
    require(av_frame_get_buffer(value.frame.get(), 32) >= 0, "frame buffer");
    return value;
}

static kvmux::RecordingStatus wait(kvmux::Recording& recording, kvmux::RecordingState desired) {
    for (int i = 0; i < 200; ++i) {
        auto status = recording.status();
        if (status.state == desired || status.state == kvmux::RecordingState::failed) return status;
        std::this_thread::sleep_for(10ms);
    }
    return recording.status();
}

static void parse_output(const std::filesystem::path& path, int expected_width = 0, int expected_height = 0) {
    AVFormatContext* context = nullptr;
    require(avformat_open_input(&context, path.string().c_str(), nullptr, nullptr) >= 0, "open output");
    require(avformat_find_stream_info(context, nullptr) >= 0, "stream info");
    bool found = false;
    for (unsigned i = 0; i < context->nb_streams; ++i)
        found |= context->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
    if (expected_width) {
        const AVCodecParameters* parameters = nullptr;
        for (unsigned i = 0; i < context->nb_streams; ++i)
            if (context->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) parameters=context->streams[i]->codecpar;
        require(parameters && parameters->width == expected_width && parameters->height == expected_height, "crop dimensions");
    }
    avformat_close_input(&context);
    require(found && std::filesystem::file_size(path) > 0, "video stream/output bytes");
}

int main() {
    const auto directory = std::filesystem::temp_directory_path() / "kvmux-recording-test";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    kvmux::Recording recording(directory);
    auto first = frame();

    require(recording.start(first), "queue start");
    const auto started = wait(recording, kvmux::RecordingState::recording);
    require(started.state == kvmux::RecordingState::recording, started.error.c_str());
    require(started.output_path.extension() == ".mp4", "recording path");

    require(recording.snapshot(first), "queue snapshot during recording");
    kvmux::RecordingStatus snapshot;
    for (int i = 0; i < 200; ++i) {
        snapshot = recording.status();
        if (!snapshot.last_snapshot_path.empty() || !snapshot.snapshot_error.empty()) break;
        std::this_thread::sleep_for(10ms);
    }
    require(snapshot.snapshot_error.empty(), snapshot.snapshot_error.c_str());
    require(snapshot.last_snapshot_path.extension() == ".jpeg", "snapshot path");
    require(snapshot.output_path == started.output_path, "snapshot preserves recording path");
    parse_output(snapshot.last_snapshot_path);

    const auto full_snapshot_path = snapshot.last_snapshot_path;
    require(recording.snapshot(first, kvmux::FrameCrop{8, 12, 24, 20}), "queue crop snapshot");
    for (int i = 0; i < 200; ++i) {
        snapshot = recording.status();
        if (snapshot.last_snapshot_path != full_snapshot_path || !snapshot.snapshot_error.empty()) break;
        std::this_thread::sleep_for(10ms);
    }
    require(snapshot.snapshot_error.empty(), snapshot.snapshot_error.c_str());
    parse_output(snapshot.last_snapshot_path, 24, 20);
    require(recording.snapshot(first, kvmux::FrameCrop{60, 0, 8, 8}), "queue invalid crop");
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
    require(cropped.start(cropped_first, kvmux::FrameCrop{8, 12, 24, 20}), "queue cropped start");
    const auto cropped_started = wait(cropped, kvmux::RecordingState::recording);
    require(cropped_started.state == kvmux::RecordingState::recording, cropped_started.error.c_str());
    require(cropped_started.width == 24 && cropped_started.height == 20, "cropped recording status dimensions");
    require(cropped.append(frame()), "append cropped recording");
    require(cropped.stop(), "stop cropped recording");
    const auto cropped_done = wait(cropped, kvmux::RecordingState::idle);
    require(cropped_done.state == kvmux::RecordingState::idle, cropped_done.error.c_str());
    parse_output(cropped_done.output_path, 24, 20);
    cropped.shutdown();
    recording.shutdown();
    std::filesystem::remove_all(directory);
    std::cout << "JPEG snapshots and full/cropped MP4 outputs parsed\n";
}
