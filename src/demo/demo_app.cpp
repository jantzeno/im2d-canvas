#include "demo_app.h"

#include <SDL3/SDL.h>
#include <backends/imgui_impl_opengl3.h>
#include <backends/imgui_impl_sdl3.h>
#include <glad/glad.h>
#include <imgui.h>

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace demo {
namespace {

const char *GetSdlError() {
  const char *error = SDL_GetError();
  return error == nullptr || error[0] == '\0' ? "unknown SDL error" : error;
}

void Require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

} // namespace

int RunDemoApp(const DemoConfig &config) {
  setenv("SDL_VIDEODRIVER", "wayland", 1);

  Require(SDL_Init(SDL_INIT_VIDEO),
          std::string("SDL_Init failed: ") + GetSdlError());
  const std::string video_driver =
      SDL_GetCurrentVideoDriver() == nullptr ? "" : SDL_GetCurrentVideoDriver();
  Require(video_driver == "wayland",
          video_driver.empty()
              ? "Wayland is required but SDL did not select a video driver"
              : std::string("Wayland is required but SDL selected ") +
                    video_driver);

  Require(SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3),
          std::string("SDL_GL_SetAttribute failed: ") + GetSdlError());
  Require(SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3),
          std::string("SDL_GL_SetAttribute failed: ") + GetSdlError());
  Require(SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                              SDL_GL_CONTEXT_PROFILE_CORE),
          std::string("SDL_GL_SetAttribute failed: ") + GetSdlError());
  Require(SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1),
          std::string("SDL_GL_SetAttribute failed: ") + GetSdlError());

  SDL_Window *window =
      SDL_CreateWindow(config.app_title.c_str(), 1600, 960,
                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
  Require(window != nullptr,
          std::string("SDL_CreateWindow failed: ") + GetSdlError());

  SDL_GLContext gl_context = SDL_GL_CreateContext(window);
  Require(gl_context != nullptr,
          std::string("SDL_GL_CreateContext failed: ") + GetSdlError());
  Require(SDL_GL_MakeCurrent(window, gl_context),
          std::string("SDL_GL_MakeCurrent failed: ") + GetSdlError());
  Require(SDL_GL_SetSwapInterval(1),
          std::string("SDL_GL_SetSwapInterval failed: ") + GetSdlError());
  Require(gladLoadGL() != 0, "gladLoadGL failed");

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  ImGui::StyleColorsDark();

  Require(ImGui_ImplSDL3_InitForOpenGL(window, gl_context),
          "ImGui SDL3 backend init failed");
  Require(ImGui_ImplOpenGL3_Init("#version 330"),
          "ImGui OpenGL3 backend init failed");

  im2d::CanvasState canvas_state;
  im2d::InitializeDefaultDocument(canvas_state);
  if (config.initialize_state) {
    config.initialize_state(canvas_state);
  }

  bool running = true;
  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT ||
          event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        running = false;
      }
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    if (config.draw_inspector) {
      config.draw_inspector(canvas_state);
    }

    ImGui::SetNextWindowPos(ImVec2(360.0f, 16.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(1200.0f, 900.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin(config.canvas_window_title.c_str());
    im2d::DrawCanvas(canvas_state);
    ImGui::End();

    ImGui::Render();
    int framebuffer_width = static_cast<int>(io.DisplaySize.x);
    int framebuffer_height = static_cast<int>(io.DisplaySize.y);
    glViewport(0, 0, framebuffer_width, framebuffer_height);
    glClearColor(config.clear_color.x, config.clear_color.y,
                 config.clear_color.z, config.clear_color.w);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(window);
  }

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_GL_DestroyContext(gl_context);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}

} // namespace demo