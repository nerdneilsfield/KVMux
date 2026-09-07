#include "render/video_renderer.hpp"

#include <glad/gl.h>

extern "C" {
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <cstring>

namespace kvmux {
namespace {

constexpr char kVertexShader[] = R"GLSL(#version 150
out vec2 uv;
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

constexpr char kFragmentShader[] = R"GLSL(#version 150
in vec2 uv;
out vec4 color;
uniform sampler2D plane0;
uniform sampler2D plane1;
uniform sampler2D plane2;
uniform int pixel_format;
uniform ivec2 source_size;
uniform mat3 yuv_matrix;
uniform vec3 yuv_offset;
uniform vec3 yuv_scale;
void main() {
    if (pixel_format == 6 || pixel_format == 7) {
        color = texture(plane0, uv);
        return;
    }
    float y;
    float u;
    float v;
    if (pixel_format == 0 || pixel_format == 1) {
        ivec2 xy = ivec2(clamp(uv, vec2(0.0), vec2(0.999999)) * vec2(source_size));
        vec4 packed = texelFetch(plane0, ivec2(xy.x / 2, xy.y), 0);
        if (pixel_format == 0) {
            y = ((xy.x & 1) == 0) ? packed.r : packed.b;
            u = packed.g; v = packed.a;
        } else {
            y = ((xy.x & 1) == 0) ? packed.g : packed.a;
            u = packed.r; v = packed.b;
        }
    } else if (pixel_format == 2) {
        y = texture(plane0, uv).r;
        vec2 chroma = texture(plane1, uv).rg;
        u = chroma.r; v = chroma.g;
    } else {
        y = texture(plane0, uv).r;
        u = texture(plane1, uv).r;
        v = texture(plane2, uv).r;
    }
    vec3 normalized = (vec3(y, u, v) + yuv_offset) * yuv_scale;
    color = vec4(clamp(yuv_matrix * normalized, 0.0, 1.0), 1.0);
}
)GLSL";

unsigned int compile(unsigned int type, const char* source, std::string& error) {
    const auto shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    int ok{};
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        std::array<char, 1024> log{};
        glGetShaderInfoLog(shader, static_cast<int>(log.size()), nullptr, log.data());
        error = log.data();
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

ColorRange effective_range(const VideoFrame& frame, ColorOverride value) {
    if (value == ColorOverride::bt601_full || value == ColorOverride::bt709_full) return ColorRange::full;
    if (value == ColorOverride::bt601_limited || value == ColorOverride::bt709_limited) return ColorRange::limited;
    return frame.color_range == ColorRange::unknown ? ColorRange::limited : frame.color_range;
}

ColorMatrix effective_matrix(const VideoFrame& frame, ColorOverride value) {
    if (value == ColorOverride::bt601_full || value == ColorOverride::bt601_limited) return ColorMatrix::bt601;
    if (value == ColorOverride::bt709_full || value == ColorOverride::bt709_limited) return ColorMatrix::bt709;
    if (frame.color_matrix != ColorMatrix::unknown) return frame.color_matrix;
    return frame.frame->height > 576 ? ColorMatrix::bt709 : ColorMatrix::bt601;
}

}  // namespace

VideoRenderer::~VideoRenderer() { destroy(); }

bool VideoRenderer::initialize() {
    if (program_) return true;
    return compile_program();
}

bool VideoRenderer::compile_program() {
    auto vertex = compile(GL_VERTEX_SHADER, kVertexShader, snapshot_.error);
    if (!vertex) return false;
    auto fragment = compile(GL_FRAGMENT_SHADER, kFragmentShader, snapshot_.error);
    if (!fragment) { glDeleteShader(vertex); return false; }
    program_ = glCreateProgram();
    glAttachShader(program_, vertex);
    glAttachShader(program_, fragment);
    glLinkProgram(program_);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    int ok{};
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (!ok) {
        std::array<char, 1024> log{};
        glGetProgramInfoLog(program_, static_cast<int>(log.size()), nullptr, log.data());
        snapshot_.error = log.data();
        glDeleteProgram(program_); program_ = 0;
        return false;
    }
    glGenVertexArrays(1, &vertex_array_);
    glGenFramebuffers(1, &framebuffer_);
    glGenTextures(static_cast<int>(planes_.size()), planes_.data());
    glUseProgram(program_);
    glUniform1i(glGetUniformLocation(program_, "plane0"), 0);
    glUniform1i(glGetUniformLocation(program_, "plane1"), 1);
    glUniform1i(glGetUniformLocation(program_, "plane2"), 2);
    glUseProgram(0);
    return true;
}

bool VideoRenderer::ensure_output(int width, int height) {
    if (output_texture_ && width_ == width && height_ == height) return true;
    if (output_texture_) glDeleteTextures(1, &output_texture_);
    glGenTextures(1, &output_texture_);
    glBindTexture(GL_TEXTURE_2D, output_texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output_texture_, 0);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    width_ = width; height_ = height;
    if (!complete) snapshot_.error = "OpenGL framebuffer is incomplete";
    return complete;
}

bool VideoRenderer::upload_plane(int index, int width, int height, unsigned int format,
                                 const std::uint8_t* data, int stride, int row_bytes) {
    if (!data || width <= 0 || height <= 0 || std::abs(stride) < row_bytes) return false;
    const std::uint8_t* upload = data;
    int upload_stride = stride;
    if (stride < 0) {
        auto& stage = staging_[static_cast<std::size_t>(index)];
        stage.resize(static_cast<std::size_t>(row_bytes) * static_cast<std::size_t>(height));
        for (int row = 0; row < height; ++row) {
            std::memcpy(stage.data() + static_cast<std::size_t>(row * row_bytes),
                        data + static_cast<std::ptrdiff_t>(row) * stride,
                        static_cast<std::size_t>(row_bytes));
        }
        upload = stage.data(); upload_stride = row_bytes;
    }
    glActiveTexture(GL_TEXTURE0 + static_cast<unsigned int>(index));
    glBindTexture(GL_TEXTURE_2D, planes_[static_cast<std::size_t>(index)]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    const int channels = format == GL_RGBA || format == GL_BGRA ? 4 : (format == GL_RG ? 2 : 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, upload_stride / channels);
    const unsigned int internal = channels == 4 ? GL_RGBA8 : (channels == 2 ? GL_RG8 : GL_R8);
    if (plane_widths_[static_cast<std::size_t>(index)] != width ||
        plane_heights_[static_cast<std::size_t>(index)] != height) {
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<int>(internal), width, height, 0,
                     format, GL_UNSIGNED_BYTE, upload);
        plane_widths_[static_cast<std::size_t>(index)] = width;
        plane_heights_[static_cast<std::size_t>(index)] = height;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, GL_UNSIGNED_BYTE, upload);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    return true;
}

bool VideoRenderer::upload(const VideoFrame& input, ColorOverride color_override) {
    snapshot_.error.clear(); snapshot_.cpu_fallback = false;
    if (!input.frame || !initialize() || !ensure_output(input.frame->width, input.frame->height)) return false;
    const AVFrame* frame = input.frame.get();
    int kind = -1;
    switch (static_cast<AVPixelFormat>(frame->format)) {
    case AV_PIX_FMT_YUYV422: kind = 0; snapshot_.pixel_path = "GPU YUY2"; break;
    case AV_PIX_FMT_UYVY422: kind = 1; snapshot_.pixel_path = "GPU UYVY"; break;
    case AV_PIX_FMT_NV12: kind = 2; snapshot_.pixel_path = "GPU NV12"; break;
    case AV_PIX_FMT_YUV420P: kind = 3; snapshot_.pixel_path = "GPU YUV420P"; break;
    case AV_PIX_FMT_YUV422P: kind = 4; snapshot_.pixel_path = "GPU YUV422P"; break;
    case AV_PIX_FMT_YUV444P: kind = 5; snapshot_.pixel_path = "GPU YUV444P"; break;
    case AV_PIX_FMT_BGRA: kind = 6; snapshot_.pixel_path = "GPU BGRA"; break;
    case AV_PIX_FMT_RGBA: kind = 7; snapshot_.pixel_path = "GPU RGBA"; break;
    default: break;
    }
    if (kind < 0) {
        sws_ = sws_getCachedContext(sws_, frame->width, frame->height,
            static_cast<AVPixelFormat>(frame->format), frame->width, frame->height,
            AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        AVFrame* converted = av_frame_alloc();
        fallback_frame_ = AvFramePtr(converted, [](AVFrame* value) { av_frame_free(&value); });
        if (!sws_ || !converted) { snapshot_.error = "Could not create CPU pixel fallback"; return false; }
        converted->format = AV_PIX_FMT_RGBA; converted->width = frame->width; converted->height = frame->height;
        if (av_frame_get_buffer(converted, 32) < 0 ||
            sws_scale(sws_, frame->data, frame->linesize, 0, frame->height,
                      converted->data, converted->linesize) <= 0) {
            snapshot_.error = "CPU pixel fallback failed"; return false;
        }
        frame = converted; kind = 7; snapshot_.pixel_path = "CPU swscale to RGBA"; snapshot_.cpu_fallback = true;
    }

    bool uploaded{};
    if (kind <= 1) {
        if ((frame->width & 1) != 0) { snapshot_.error = "Packed 4:2:2 width must be even"; return false; }
        uploaded = upload_plane(0, frame->width / 2, frame->height, GL_RGBA,
                                frame->data[0], frame->linesize[0], frame->width * 2);
    } else if (kind == 2) {
        uploaded = upload_plane(0, frame->width, frame->height, GL_RED,
                                frame->data[0], frame->linesize[0], frame->width) &&
                   upload_plane(1, (frame->width + 1) / 2, (frame->height + 1) / 2, GL_RG,
                                frame->data[1], frame->linesize[1], ((frame->width + 1) / 2) * 2);
    } else if (kind >= 3 && kind <= 5) {
        const int cw = kind == 5 ? frame->width : (frame->width + 1) / 2;
        const int ch = kind == 3 ? (frame->height + 1) / 2 : frame->height;
        uploaded = upload_plane(0, frame->width, frame->height, GL_RED, frame->data[0], frame->linesize[0], frame->width) &&
                   upload_plane(1, cw, ch, GL_RED, frame->data[1], frame->linesize[1], cw) &&
                   upload_plane(2, cw, ch, GL_RED, frame->data[2], frame->linesize[2], cw);
    } else {
        uploaded = upload_plane(0, frame->width, frame->height, kind == 6 ? GL_BGRA : GL_RGBA,
                                frame->data[0], frame->linesize[0], frame->width * 4);
    }
    if (!uploaded) { snapshot_.error = "Invalid video plane layout"; return false; }

    const auto matrix = effective_matrix(input, color_override);
    const auto range = effective_range(input, color_override);
    snapshot_.color_choice = std::string(matrix == ColorMatrix::bt709 ? "BT.709 " : "BT.601 ") +
        (range == ColorRange::full ? "full" : "limited") +
        (color_override == ColorOverride::automatic ?
            ((input.color_matrix == ColorMatrix::unknown || input.color_range == ColorRange::unknown) ? " (inferred)" : " (metadata)") : " (override)");
    const std::array<float, 9> m = matrix == ColorMatrix::bt709 ?
        std::array<float, 9>{1, 1, 1, 0, -.1873F, 1.8556F, 1.5748F, -.4681F, 0} :
        std::array<float, 9>{1, 1, 1, 0, -.344136F, 1.772F, 1.402F, -.714136F, 0};
    const std::array<float, 3> offset = range == ColorRange::full ?
        std::array<float, 3>{0, -.5F, -.5F} : std::array<float, 3>{-16.F/255.F, -128.F/255.F, -128.F/255.F};
    const std::array<float, 3> scale = range == ColorRange::full ?
        std::array<float, 3>{1, 1, 1} : std::array<float, 3>{255.F/219.F, 255.F/224.F, 255.F/224.F};

    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glViewport(0, 0, width_, height_);
    glUseProgram(program_);
    glUniform1i(glGetUniformLocation(program_, "pixel_format"), kind);
    glUniform2i(glGetUniformLocation(program_, "source_size"), width_, height_);
    glUniformMatrix3fv(glGetUniformLocation(program_, "yuv_matrix"), 1, GL_FALSE, m.data());
    glUniform3fv(glGetUniformLocation(program_, "yuv_offset"), 1, offset.data());
    glUniform3fv(glGetUniformLocation(program_, "yuv_scale"), 1, scale.data());
    glBindVertexArray(vertex_array_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0); glUseProgram(0); glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (glGetError() != GL_NO_ERROR) { snapshot_.error = "OpenGL video conversion failed"; return false; }
    return true;
}

void VideoRenderer::destroy() noexcept {
    if (sws_) sws_freeContext(sws_);
    sws_ = nullptr; fallback_frame_.reset();
    if (output_texture_) glDeleteTextures(1, &output_texture_);
    if (planes_[0]) glDeleteTextures(static_cast<int>(planes_.size()), planes_.data());
    if (framebuffer_) glDeleteFramebuffers(1, &framebuffer_);
    if (vertex_array_) glDeleteVertexArrays(1, &vertex_array_);
    if (program_) glDeleteProgram(program_);
    output_texture_ = framebuffer_ = vertex_array_ = program_ = 0;
    planes_.fill(0); width_ = height_ = 0;
}

}  // namespace kvmux
