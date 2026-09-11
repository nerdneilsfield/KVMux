#include "video/codec/jetson_encoder.hpp"

#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <limits>

namespace kvmux {
namespace {
constexpr std::size_t kCapacity = 4;
CodecResult fail(std::string message) { return {CodecStatus::failed, std::move(message)}; }

class JetsonEncoder final : public VideoEncoder {
public:
    ~JetsonEncoder() override { shutdown(); }
    CodecBackend backend() const noexcept override { return CodecBackend::jetson_gstreamer; }
    CodecDiagnostic diagnostic() const override {
        return {backend(), hardware_active_, hardware_active_, hardware_active_
            ? "NVIDIA nvv4l2h265enc emitted a hardware HEVC access unit"
            : "NVIDIA encoder selected; hardware output not yet observed"};
    }
    CodecResult configure(const CodecConfig& config) override {
        shutdown();
        if (!config.width || !config.height || config.width > kMaxCaptureWidth ||
            config.height > kMaxCaptureHeight || (config.width % 2) || (config.height % 2) ||
            !config.fps_numerator || !config.fps_denominator ||
            config.fps_numerator < config.fps_denominator ||
            config.fps_numerator > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            config.fps_denominator > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            static_cast<std::uint64_t>(config.fps_numerator) >
                240ULL * config.fps_denominator ||
            !config.bitrate || config.bitrate > 100'000'000 || !config.keyframe_interval)
            return {CodecStatus::invalid_input, "Invalid HEVC dimensions/rate/bitrate"};
        config_ = config;
        encoded_sequence_ = 0;
        // CPU NV12 upload is explicit. No software encoding element exists here.
        const auto text = std::string("appsrc name=input is-live=true format=time block=false max-buffers=4 max-bytes=0 max-time=0 ! ") +
            "nvvidconv ! video/x-raw(memory:NVMM),format=NV12 ! "
            "nvv4l2h265enc name=encoder num-B-Frames=0 insert-sps-pps=true insert-aud=true bitrate=" +
            std::to_string(config.bitrate) + " idrinterval=" + std::to_string(config.keyframe_interval) +
            " iframeinterval=" + std::to_string(config.keyframe_interval) +
            " ! video/x-h265,stream-format=byte-stream,alignment=au ! "
            "appsink name=output sync=false max-buffers=4 drop=false wait-on-eos=false";
        GError* error = nullptr;
        pipeline_ = gst_parse_launch(text.c_str(), &error);
        if (error || !pipeline_) {
            auto result = fail(error ? error->message : "Cannot create Jetson HEVC pipeline");
            if (error) g_error_free(error);
            shutdown();
            return result;
        }
        input_ = gst_bin_get_by_name(GST_BIN(pipeline_), "input");
        output_ = gst_bin_get_by_name(GST_BIN(pipeline_), "output");
        encoder_ = gst_bin_get_by_name(GST_BIN(pipeline_), "encoder");
        GstCaps* caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "NV12",
            "width", G_TYPE_INT, static_cast<int>(config.width), "height", G_TYPE_INT,
            static_cast<int>(config.height), "framerate", GST_TYPE_FRACTION,
            static_cast<int>(config.fps_numerator), static_cast<int>(config.fps_denominator), nullptr);
        gst_app_src_set_caps(GST_APP_SRC(input_), caps);
        gst_caps_unref(caps);
        if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            auto result = bus_error();
            shutdown();
            return result.ok() ? fail("Jetson HEVC hardware failed to start") : result;
        }
        return {};
    }
    CodecResult submit(const EncoderInput& input) override {
        if (!pipeline_ || finished_) return fail("Encoder is not accepting input");
        if (auto error = bus_error(); !error.ok()) return error;
        if (pending_.size() == kCapacity) return {CodecStatus::again, {}};
        const auto* frame = input.frame.get();
        if (!frame || frame->width != static_cast<int>(config_.width) ||
            frame->height != static_cast<int>(config_.height) || input.generation != config_.generation ||
            input.pts_ns < 0 || (last_pts_ >= 0 && input.pts_ns <= last_pts_) ||
            (frame->format != AV_PIX_FMT_NV12 && frame->format != AV_PIX_FMT_YUV420P))
            return {CodecStatus::invalid_input, "Expected matching CPU NV12/YUV420P frame and increasing nonnegative PTS"};
        const int width = frame->width, height = frame->height;
        const bool planar = frame->format == AV_PIX_FMT_YUV420P;
        if (frame->hw_frames_ctx || !frame->data[0] || !frame->data[1] ||
            frame->linesize[0] < width || frame->linesize[1] < (planar ? width / 2 : width) ||
            (planar && (!frame->data[2] || frame->linesize[2] < width / 2)))
            return {CodecStatus::invalid_input, "Invalid CPU frame planes or strides"};
        GstVideoInfo info;
        gst_video_info_set_format(&info, GST_VIDEO_FORMAT_NV12, width, height);
        GstBuffer* buffer = gst_buffer_new_allocate(nullptr, info.size, nullptr);
        GstMapInfo map{};
        if (!buffer || !gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
            if (buffer) gst_buffer_unref(buffer);
            return fail("Cannot allocate encoder input buffer");
        }
        std::memset(map.data, 0, map.size);
        for (int y = 0; y < height; ++y)
            std::memcpy(map.data + info.offset[0] + y * info.stride[0],
                        frame->data[0] + y * frame->linesize[0], width);
        for (int y = 0; y < height / 2; ++y) {
            auto* row = map.data + info.offset[1] + y * info.stride[1];
            if (!planar) std::memcpy(row, frame->data[1] + y * frame->linesize[1], width);
            else for (int x = 0; x < width / 2; ++x) {
                row[2*x] = frame->data[1][y * frame->linesize[1] + x];
                row[2*x+1] = frame->data[2][y * frame->linesize[2] + x];
            }
        }
        gst_buffer_unmap(buffer, &map);
        GST_BUFFER_PTS(buffer) = static_cast<GstClockTime>(input.pts_ns);
        GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
        GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(GST_SECOND, config_.fps_denominator, config_.fps_numerator);
        EncodedAccessUnit metadata;
        metadata.width = config_.width; metadata.height = config_.height;
        metadata.pts_ns = input.pts_ns; metadata.capture_sequence = input.capture_sequence;
        metadata.generation = input.generation; metadata.arrival = input.arrival;
        metadata.color_range = frame->color_range; metadata.color_space = frame->colorspace;
        metadata.color_primaries = frame->color_primaries; metadata.color_transfer = frame->color_trc;
        metadata.sample_aspect_ratio = frame->sample_aspect_ratio.num > 0 && frame->sample_aspect_ratio.den > 0
            ? frame->sample_aspect_ratio : AVRational{1, 1};
        const auto flow = gst_app_src_push_buffer(GST_APP_SRC(input_), buffer); // takes ownership
        if (flow != GST_FLOW_OK) return fail("Jetson appsrc rejected input: " + std::to_string(flow));
        pending_.push_back(std::move(metadata));
        last_pts_ = input.pts_ns;
        return {};
    }
    CodecResult poll(EncodedAccessUnit& output) override {
        if (!pipeline_) return fail("Encoder is not configured");
        if (auto error = bus_error(); !error.ok()) return error;
        GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(output_), 0);
        if (!sample) {
            if (finished_ && gst_app_sink_is_eos(GST_APP_SINK(output_)))
                return pending_.empty() ? CodecResult{CodecStatus::end_of_stream, {}} : fail("Encoder EOS lost pending frames");
            return {CodecStatus::again, {}};
        }
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        GstCaps* caps = gst_sample_get_caps(sample);
        const GstStructure* structure = caps ? gst_caps_get_structure(caps, 0) : nullptr;
        const char* alignment = structure ? gst_structure_get_string(structure, "alignment") : nullptr;
        const char* stream = structure ? gst_structure_get_string(structure, "stream-format") : nullptr;
        GstMapInfo map{};
        if (!buffer || !alignment || std::strcmp(alignment, "au") || !stream ||
            std::strcmp(stream, "byte-stream") || pending_.empty() ||
            !GST_BUFFER_PTS_IS_VALID(buffer) || GST_BUFFER_PTS(buffer) != static_cast<GstClockTime>(pending_.front().pts_ns) ||
            !gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            gst_sample_unref(sample);
            return fail("Jetson output violates ordered Annex B AU/PTS contract");
        }
        if (!map.size || map.size > kMaxCompressedSampleBytes) {
            gst_buffer_unmap(buffer, &map); gst_sample_unref(sample);
            return fail("Jetson AU exceeds payload limit");
        }
        output = std::move(pending_.front()); pending_.pop_front();
        hardware_active_ = true;
        output.encoded_sequence = ++encoded_sequence_;
        output.bytes.assign(map.data, map.data + map.size);
        for (std::size_t i = 0; i + 4 < map.size; ++i) {
            if (map.data[i] == 0 && map.data[i+1] == 0 && map.data[i+2] == 1) {
                const auto type = (map.data[i+3] >> 1) & 63;
                if (type == 19 || type == 20) output.idr = true;
            }
        }
        gst_buffer_unmap(buffer, &map); gst_sample_unref(sample);
        return {};
    }
    CodecResult request_keyframe() override {
        if (!encoder_ || finished_) return fail("Encoder is not accepting keyframe requests");
        if (auto error = bus_error(); !error.ok()) return error;
        g_signal_emit_by_name(encoder_, "force-IDR");
        return {};
    }
    CodecResult finish() override {
        if (!input_) return fail("Encoder is not configured");
        if (finished_) return {};
        if (gst_app_src_end_of_stream(GST_APP_SRC(input_)) != GST_FLOW_OK)
            return fail("Cannot finish Jetson encoder");
        finished_ = true;
        return {};
    }
    CodecResult reset() override {
        if (!pipeline_) return fail("Encoder is not configured");
        const auto saved = config_;
        const auto sequence = encoded_sequence_;
        auto result = configure(saved);
        encoded_sequence_ = sequence;
        return result;
    }
    void shutdown() noexcept override {
        if (pipeline_) gst_element_set_state(pipeline_, GST_STATE_NULL);
        for (auto* element : {input_, output_, encoder_, pipeline_})
            if (element) gst_object_unref(element);
        input_ = output_ = encoder_ = pipeline_ = nullptr;
        pending_.clear(); last_pts_ = -1; finished_ = false; hardware_active_ = false;
    }
private:
    CodecResult bus_error() {
        GstBus* bus = gst_element_get_bus(pipeline_);
        GstMessage* message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        gst_object_unref(bus);
        if (!message) return {};
        GError* error = nullptr; gchar* debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        auto result = fail(std::string("Jetson HEVC hardware: ") + (error ? error->message : "pipeline failed"));
        if (error) g_error_free(error);
        g_free(debug); gst_message_unref(message);
        return result;
    }
    CodecConfig config_;
    GstElement *pipeline_{}, *input_{}, *output_{}, *encoder_{};
    std::deque<EncodedAccessUnit> pending_;
    std::int64_t last_pts_{-1};
    bool finished_{};
    bool hardware_active_{};
    std::uint64_t encoded_sequence_{};
};
}  // namespace

std::unique_ptr<VideoEncoder> create_jetson_encoder(std::string& error) {
    GError* init_error = nullptr;
    if (!gst_init_check(nullptr, nullptr, &init_error)) {
        error = init_error ? init_error->message : "GStreamer initialization failed";
        if (init_error) g_error_free(init_error);
        return nullptr;
    }
    for (const char* name : {"appsrc", "nvvidconv", "nvv4l2h265enc", "appsink"}) {
        GstElementFactory* factory = gst_element_factory_find(name);
        if (!factory) { error = std::string("Jetson hardware encoder unavailable: missing ") + name; return nullptr; }
        gst_object_unref(factory);
    }
    error.clear();
    return std::make_unique<JetsonEncoder>();
}
}  // namespace kvmux
