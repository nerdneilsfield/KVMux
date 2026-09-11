#pragma once

#include "video/video_frame.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct SwsContext;

namespace kvmux {

enum class ColorOverride { automatic, bt601_limited, bt601_full, bt709_limited, bt709_full };

struct RenderSnapshot {
    std::string pixel_path;
    std::string color_choice;
    bool cpu_fallback{};
    std::string error;
};

class VideoRenderer {
public:
    VideoRenderer() = default;
    ~VideoRenderer();
    VideoRenderer(const VideoRenderer&) = delete;
    VideoRenderer& operator=(const VideoRenderer&) = delete;

    bool initialize();
    bool upload(const VideoFrame& frame, ColorOverride color_override);
    void destroy() noexcept;
    [[nodiscard]] std::uintptr_t texture_id() const noexcept { return output_texture_; }
    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }
    [[nodiscard]] RenderSnapshot snapshot() const { return snapshot_; }

private:
    bool compile_program();
    bool ensure_output(int width, int height);
    bool upload_plane(int index, int width, int height, unsigned int format,
                      const std::uint8_t* data, int stride, int row_bytes);

    unsigned int program_{};
    unsigned int vertex_array_{};
    unsigned int framebuffer_{};
    unsigned int output_texture_{};
    std::array<unsigned int, 3> planes_{};
    std::array<int, 3> plane_widths_{};
    std::array<int, 3> plane_heights_{};
    std::array<unsigned int, 3> plane_formats_{};
    int width_{};
    int height_{};
    std::array<std::vector<std::uint8_t>, 3> staging_;
    ::SwsContext* sws_{};
    AvFramePtr fallback_frame_;
    RenderSnapshot snapshot_;
};

}  // namespace kvmux
