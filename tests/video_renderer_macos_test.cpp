#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <glad/gl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "app/kvm_session.hpp"
#include "network/relay_client.hpp"
#include "relay_test_fakes.hpp"
#include "render/video_renderer.hpp"

int reconnect_test(GLuint ui_framebuffer, const char* jpeg_path) {
  FakeSerial serial;
  kvmux::Ch9329ControlSink hardware(serial.io());
  hardware.connect("fake", 57600);
  FakeCapture capture;
  std::ifstream input(jpeg_path, std::ios::binary);
  capture.jpeg.assign(std::istreambuf_iterator<char>(input), {});
  kvmux::relay::RelayServer server(capture, hardware);
  std::string error;
  if (!server.start({"127.0.0.1", 0, 0}, error)) return 1;
  kvmux::VideoRenderer renderer;
  std::unique_ptr<kvmux::KvmSession> session;
  std::optional<kvmux::VideoFrame> current;
  int failures = 0;
  auto begin = [&] {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGui::Begin("KVMuxRoot");
    ImGui::TextUnformatted("Connect relay");
  };
  auto end = [&] {
    if (renderer.texture_id())
      ImGui::GetBackgroundDrawList()->AddImage(
          static_cast<ImTextureID>(renderer.texture_id()), ImVec2(0, 0),
          ImVec2(100, 100));
    ImGui::End();
    ImGui::Render();
    glBindFramebuffer(GL_FRAMEBUFFER, ui_framebuffer);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  };
  for (int cycle = 0; cycle < 5; ++cycle) {
    // Same order as main: retained frame upload, NewFrame, Connect destroys
    // renderer/session, then ImGui renders this frame without video.
    if (current && !renderer.upload(*current, kvmux::ColorOverride::automatic))
      ++failures;
    begin();
    session.reset();
    current.reset();
    renderer.destroy();
    auto client =
        std::make_shared<kvmux::relay::RelayClient>(kvmux::relay::ClientOptions{
            "127.0.0.1", server.control_port(), server.video_port()});
    auto source = std::make_unique<kvmux::relay::NetworkCaptureSource>(client);
    const auto device = source->enumerate_devices().front();
    const auto mode = source->enumerate_modes(device.stable_id).front();
    session = std::make_unique<kvmux::KvmSession>(
        std::move(source),
        std::make_unique<kvmux::relay::NetworkControlSink>(client));
    (void)session->select_capture(device, mode);
    end();
    int frames = 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (frames < 20 && std::chrono::steady_clock::now() < deadline) {
      session->tick();
      if (auto frame = session->take_latest_frame()) {
        current = std::move(frame);
        ++frames;
      }
      if (current) {
        if (!renderer.upload(*current, kvmux::ColorOverride::automatic))
          ++failures;
        else
          session->video_presented(current->generation, current->sequence);
        GLint binding{};
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
        glBindTexture(GL_TEXTURE_2D,
                      static_cast<GLuint>(renderer.texture_id()));
        unsigned char rgba[16 * 16 * 4];
        GLint internal{};
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT,
                                 &internal);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(binding));
        if (rgba[0] < 245 || rgba[1] > 10 || rgba[2] > 10 ||
            internal != GL_RGBA8) {
          std::fprintf(stderr,
                       "reconnect=%d frames=%d RGB=%d,%d,%d internal=%x\n",
                       cycle, frames, rgba[0], rgba[1], rgba[2], internal);
          ++failures;
        }
      }
      begin();
      end();
      if (current) {
        unsigned char pixel[4]{};
        glBindFramebuffer(GL_FRAMEBUFFER, ui_framebuffer);
        glReadPixels(5, 235, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (pixel[0] < 245 || pixel[1] > 10 || pixel[2] > 10) {
          std::fprintf(stderr, "UI reconnect=%d RGB=%d,%d,%d\n", cycle,
                       pixel[0], pixel[1], pixel[2]);
          ++failures;
        }
      }
    }
    if (frames < 20) {
      std::fprintf(stderr, "reconnect=%d only %d frames\n", cycle, frames);
      ++failures;
    }
    if (cycle == 2) {
      // UDP absence suspends freshness, not the separately bounded 10s
      // session. Reconnect remains possible without restarting the GUI.
      server.stop();
      const auto fault_deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (session->snapshot().video_state !=
                 kvmux::SessionVideoState::stale &&
             std::chrono::steady_clock::now() < fault_deadline) {
        session->tick();
        begin();
        end();
      }
      if (session->snapshot().video_state != kvmux::SessionVideoState::stale ||
          session->snapshot().capture.state == kvmux::CaptureState::fault)
        ++failures;
      if (!server.start({"127.0.0.1", 0, 0}, error)) ++failures;
    } else {
      session->release_control();
      (void)session->stop_capture();
      (void)session->disconnect_control();
    }
  }
  session.reset();
  renderer.destroy();
  server.stop();
  hardware.disconnect();
  std::printf("actual relay / session / MJPEG / ImGui reconnect: %d failures\n",
              failures);
  return failures;
}

// Real macOS SDL/OpenGL storage and reconnect regression; no capture hardware
// needed.
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  if (!SDL_Init(SDL_INIT_VIDEO)) return 2;
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  auto* window = SDL_CreateWindow("KVMux reconnect test", 320, 240,
                                  SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
  auto context = SDL_GL_CreateContext(window);
  if (!window || !context || !SDL_GL_MakeCurrent(window, context)) return 2;
  if (!gladLoadGL([](const char* name) {
        return reinterpret_cast<GLADapiproc>(SDL_GL_GetProcAddress(name));
      }))
    return 2;
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  ImGui::GetIO().DisplaySize = ImVec2(320, 240);
  ImGui_ImplSDL3_InitForOpenGL(window, context);
  if (!ImGui_ImplOpenGL3_Init("#version 150")) return 2;
  // Read back ImGui output without depending on a visible window or swap.
  GLuint ui_texture{}, ui_framebuffer{};
  glGenTextures(1, &ui_texture);
  glBindTexture(GL_TEXTURE_2D, ui_texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 320, 240, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, nullptr);
  glGenFramebuffers(1, &ui_framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, ui_framebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         ui_texture, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  int failures = reconnect_test(ui_framebuffer, argv[1]);
  {
    kvmux::VideoRenderer renderer;
    for (int cycle = 0; cycle < 3; ++cycle) {
      for (auto format :
           {AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_YUVJ444P,
            AV_PIX_FMT_RGBA, AV_PIX_FMT_YUVJ420P}) {
        for (int size : {16, 16, 32, 16}) {
          kvmux::VideoFrame frame;
          frame.frame = kvmux::AvFramePtr(
              av_frame_alloc(), [](AVFrame* f) { av_frame_free(&f); });
          auto* f = frame.frame.get();
          f->width = size;
          f->height = size;
          f->format = format;
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
          if (!renderer.upload(frame, kvmux::ColorOverride::automatic)) {
            std::fprintf(stderr, "cycle=%d format=%d size=%d: %s\n", cycle,
                         format, size, renderer.snapshot().error.c_str());
            ++failures;
          }
          std::vector<unsigned char> rgba(
              static_cast<std::size_t>(size * size * 4));
          GLint binding{};
          glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
          glBindTexture(GL_TEXTURE_2D,
                        static_cast<GLuint>(renderer.texture_id()));
          glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                        rgba.data());
          glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(binding));
          auto bottom = static_cast<std::size_t>((size - 1) * size * 4);
          for (int c = 0; c < 3; ++c)
            if (rgba[static_cast<std::size_t>(c)] > 2 ||
                rgba[bottom + static_cast<std::size_t>(c)] < 253)
              ++failures;
          // Exercise the real backend between uploads, including its texture
          // updates.
          ImGui_ImplOpenGL3_NewFrame();
          ImGui_ImplSDL3_NewFrame();
          ImGui::NewFrame();
          ImGui::GetBackgroundDrawList()->AddImage(
              static_cast<ImTextureID>(renderer.texture_id()), ImVec2(0, 0),
              ImVec2(100, 100));
          ImGui::Render();
          glBindFramebuffer(GL_FRAMEBUFFER, ui_framebuffer);
          ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
          glBindFramebuffer(GL_FRAMEBUFFER, 0);
          if (glGetError() != GL_NO_ERROR) ++failures;
        }
      }
      // Local/Remote and Connect relay destroy the renderer, then reuse this
      // object.
      ImGui_ImplOpenGL3_NewFrame();
      ImGui_ImplSDL3_NewFrame();
      ImGui::NewFrame();
      ImGui::Begin("KVMuxRoot");
      ImGui::TextUnformatted("Connect relay");
      renderer.destroy();
      ImGui::End();
      ImGui::Render();
      glBindFramebuffer(GL_FRAMEBUFFER, ui_framebuffer);
      ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
  }
  glDeleteFramebuffers(1, &ui_framebuffer);
  glDeleteTextures(1, &ui_texture);
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_GL_DestroyContext(context);
  SDL_DestroyWindow(window);
  SDL_Quit();
  std::printf(
      "renderer lifecycle: 60 uploads, format/size switches, ImGui interleave: "
      "%d failures\n",
      failures);
  return failures ? 1 : 0;
}
