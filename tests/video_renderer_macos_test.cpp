#include "render/video_renderer.hpp"
#include <glad/gl.h>
#include <OpenGL/OpenGL.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <vector>

// Real macOS GL storage/lifetime regression; no capture hardware is needed.
int main() {
    CGLPixelFormatAttribute attrs[] = {kCGLPFAOpenGLProfile,
        static_cast<CGLPixelFormatAttribute>(kCGLOGLPVersion_3_2_Core),
        static_cast<CGLPixelFormatAttribute>(0)};
    CGLPixelFormatObj pixel{};
    CGLContextObj context{};
    GLint count{};
    if (CGLChoosePixelFormat(attrs, &pixel, &count) ||
        CGLCreateContext(pixel, nullptr, &context) || CGLSetCurrentContext(context)) return 2;
    if (!gladLoadGL([](const char* name) { return reinterpret_cast<GLADapiproc>(dlsym(RTLD_DEFAULT, name)); })) return 2;
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().DisplaySize = ImVec2(320, 240);
    if (!ImGui_ImplOpenGL3_Init("#version 150")) return 2;
    // CGL has no window drawable. Give the ImGui pass a real render target.
    GLuint ui_texture{}, ui_framebuffer{};
    glGenTextures(1, &ui_texture); glBindTexture(GL_TEXTURE_2D, ui_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 320, 240, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &ui_framebuffer); glBindFramebuffer(GL_FRAMEBUFFER, ui_framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ui_texture, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    int failures = 0;
    {
        kvmux::VideoRenderer renderer;
        for (int cycle = 0; cycle < 3; ++cycle) {
            for (auto format : {AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUVJ422P,
                                AV_PIX_FMT_YUVJ444P, AV_PIX_FMT_RGBA, AV_PIX_FMT_YUVJ420P}) {
                for (int size : {16, 16, 32, 16}) {
                    kvmux::VideoFrame frame;
                    frame.frame = kvmux::AvFramePtr(av_frame_alloc(), [](AVFrame* f) { av_frame_free(&f); });
                    auto* f = frame.frame.get();
                    f->width = size; f->height = size; f->format = format;
                    if (av_frame_get_buffer(f, 32) < 0) return 2;
                    frame.color_range = kvmux::ColorRange::full;
                    for (int y = 0; y < size; ++y)
                        std::memset(f->data[0] + y * f->linesize[0], y < size / 2 ? 0 : 255,
                                    format == AV_PIX_FMT_RGBA ? size * 4 : size);
                    if (format != AV_PIX_FMT_RGBA) {
                        int cw = format == AV_PIX_FMT_YUVJ444P ? size : size / 2;
                        int ch = format == AV_PIX_FMT_YUVJ420P ? size / 2 : size;
                        for (int p = 1; p < 3; ++p)
                            for (int y = 0; y < ch; ++y)
                                std::memset(f->data[p] + y * f->linesize[p], 128, cw);
                    }
                    // Leave unrelated bindings on all input units before each upload.
                    for (int unit = 0; unit < 3; ++unit) {
                        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
                        glBindTexture(GL_TEXTURE_2D, ui_texture);
                    }
                    if (!renderer.upload(frame, kvmux::ColorOverride::automatic)) {
                        std::fprintf(stderr, "cycle=%d format=%d size=%d: %s\n", cycle, format, size,
                                     renderer.snapshot().error.c_str());
                        ++failures;
                    }
                    std::vector<unsigned char> rgba(static_cast<std::size_t>(size * size * 4));
                    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(renderer.texture_id()));
                    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
                    auto bottom = static_cast<std::size_t>((size - 1) * size * 4);
                    for (int c = 0; c < 3; ++c)
                        if (rgba[static_cast<std::size_t>(c)] > 2 || rgba[bottom + static_cast<std::size_t>(c)] < 253)
                            ++failures;
                    // Exercise the real backend between uploads, including its texture updates.
                    ImGui_ImplOpenGL3_NewFrame(); ImGui::NewFrame();
                    ImGui::GetBackgroundDrawList()->AddImage(static_cast<ImTextureID>(renderer.texture_id()),
                        ImVec2(0, 0), ImVec2(100, 100));
                    ImGui::Render(); glBindFramebuffer(GL_FRAMEBUFFER, ui_framebuffer);
                    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    if (glGetError() != GL_NO_ERROR) ++failures;
                }
            }
            // Local/Remote and Connect relay destroy the renderer, then reuse this object.
            renderer.destroy();
        }
    }
    glDeleteFramebuffers(1, &ui_framebuffer); glDeleteTextures(1, &ui_texture);
    ImGui_ImplOpenGL3_Shutdown(); ImGui::DestroyContext();
    CGLSetCurrentContext(nullptr); CGLDestroyContext(context); CGLDestroyPixelFormat(pixel);
    std::printf("renderer lifecycle: 60 uploads, format/size switches, ImGui interleave: %d failures\n", failures);
    return failures ? 1 : 0;
}
