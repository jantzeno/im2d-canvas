#include "../canvas/im2d_canvas.h"

#include <SDL3/SDL.h>
#include <backends/imgui_impl_opengl3.h>
#include <backends/imgui_impl_sdl3.h>
#include <glad/glad.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace {

using im2d::CanvasState;
using im2d::MeasurementUnit;

const char *GetSdlError() {
  const char *error = SDL_GetError();
  return error == nullptr || error[0] == '\0' ? "unknown SDL error" : error;
}

void Require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void DrawInspector(CanvasState &state) {
  ImGui::Begin("Canvas Controls");

  static std::array<char, 64> name_buffer = {'W', 'o', 'r', 'k', 'i',
                                             'n', 'g', ' ', 'A', 'r',
                                             'e', 'a', ' ', '2', '\0'};
  static float width_value = 210.0f;
  static float height_value = 297.0f;
  static MeasurementUnit area_unit = MeasurementUnit::Millimeters;
  static bool area_movable = true;
  static bool area_resizable = true;

  ImGui::TextUnformatted("Working Areas");
  ImGui::InputText("Name", name_buffer.data(), name_buffer.size());
  ImGui::InputFloat("Width", &width_value, 1.0f, 10.0f, "%.2f");
  ImGui::InputFloat("Height", &height_value, 1.0f, 10.0f, "%.2f");

  if (ImGui::BeginCombo("Area Unit", im2d::MeasurementUnitLabel(area_unit))) {
    for (MeasurementUnit unit :
         {MeasurementUnit::Millimeters, MeasurementUnit::Inches,
          MeasurementUnit::Pixels}) {
      const bool selected = area_unit == unit;
      if (ImGui::Selectable(im2d::MeasurementUnitLabel(unit), selected)) {
        area_unit = unit;
      }
      if (selected) {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }

  ImGui::Checkbox("New Areas Movable", &area_movable);
  ImGui::Checkbox("New Areas Resizable", &area_resizable);

  if (ImGui::Button("Add Working Area")) {
    uint32_t flags = im2d::WorkingAreaFlagNone;
    if (area_movable) {
      flags |= im2d::WorkingAreaFlagMovable;
    }
    if (area_resizable) {
      flags |= im2d::WorkingAreaFlagResizable;
    }

    im2d::WorkingAreaCreateInfo create_info;
    create_info.name =
        name_buffer[0] == '\0'
            ? "Working Area " + std::to_string(state.next_working_area_id)
            : std::string(name_buffer.data());
    create_info.size_pixels =
        ImVec2(im2d::UnitsToPixels(width_value, area_unit, state.calibration),
               im2d::UnitsToPixels(height_value, area_unit, state.calibration));
    create_info.flags = flags;
    const std::string name =
        create_info.name.empty()
            ? "Working Area " + std::to_string(state.next_working_area_id)
            : create_info.name;
    create_info.name = name;
    im2d::AddWorkingArea(state, create_info);
  }

  ImGui::Separator();
  ImGui::Text("Areas: %d", static_cast<int>(state.working_areas.size()));
  ImGui::Text("Exports prepared: %d",
              static_cast<int>(state.export_areas.size()));
  ImGui::Text("Guides: %d", static_cast<int>(state.guides.size()));

  ImGui::Separator();
  ImGui::TextUnformatted("Grid");
  ImGui::Checkbox("Show Grid", &state.grid.visible);
  ImGui::InputFloat("Grid Spacing", &state.grid.spacing, 1.0f, 10.0f, "%.2f");
  ImGui::SliderInt("Grid Subdivisions", &state.grid.subdivisions, 1, 10);
  if (ImGui::BeginCombo("Grid Unit",
                        im2d::MeasurementUnitLabel(state.grid.unit))) {
    for (MeasurementUnit unit :
         {MeasurementUnit::Millimeters, MeasurementUnit::Inches,
          MeasurementUnit::Pixels}) {
      const bool selected = state.grid.unit == unit;
      if (ImGui::Selectable(im2d::MeasurementUnitLabel(unit), selected)) {
        state.grid.unit = unit;
      }
      if (selected) {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }

  ImGui::Separator();
  ImGui::TextUnformatted("Physical Calibration");
  ImGui::Checkbox("Use Calibrated Physical Units", &state.calibration.enabled);
  ImGui::InputFloat("Reference Pixels", &state.calibration.reference_pixels,
                    1.0f, 10.0f, "%.2f");
  ImGui::InputFloat("Measured Length", &state.calibration.measured_length, 1.0f,
                    10.0f, "%.2f");
  if (ImGui::BeginCombo(
          "Measured Unit",
          im2d::MeasurementUnitLabel(state.calibration.measured_unit))) {
    for (MeasurementUnit unit :
         {MeasurementUnit::Millimeters, MeasurementUnit::Inches}) {
      const bool selected = state.calibration.measured_unit == unit;
      if (ImGui::Selectable(im2d::MeasurementUnitLabel(unit), selected)) {
        state.calibration.measured_unit = unit;
      }
      if (selected) {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }
  if (ImGui::Button("Apply Calibration")) {
    im2d::ApplyCalibration(state.calibration);
  }
  ImGui::SameLine();
  if (ImGui::Button("Reset Calibration")) {
    state.calibration.enabled = false;
    state.calibration.calibrated_pixels_per_mm =
        state.calibration.default_pixels_per_mm;
  }
  ImGui::Text("Pixels/mm: %.4f",
              im2d::GetPixelsPerMillimeter(state.calibration));
  ImGui::Text("Pixels/in: %.2f",
              im2d::GetPixelsPerMillimeter(state.calibration) * 25.4f);

  ImGui::Separator();
  ImGui::TextUnformatted("Snapping");
  ImGui::Checkbox("Snap to Guides", &state.snapping.to_guides);
  ImGui::Checkbox("Snap to Main Grid", &state.snapping.to_grid_major);
  ImGui::Checkbox("Snap to Subgrid", &state.snapping.to_grid_minor);
  ImGui::SliderFloat("Snap Threshold", &state.snapping.screen_threshold, 2.0f,
                     20.0f, "%.1f px");

  ImGui::Separator();
  ImGui::TextUnformatted("Theme");
  ImGui::ColorEdit4("Canvas Background", &state.theme.canvas_background.x);
  ImGui::ColorEdit4("Ruler Background", &state.theme.ruler_background.x);
  ImGui::ColorEdit4("Ruler Text", &state.theme.ruler_text.x);
  ImGui::ColorEdit4("Ruler Ticks", &state.theme.ruler_ticks.x);
  ImGui::ColorEdit4("Grid Major", &state.theme.grid_major.x);
  ImGui::ColorEdit4("Grid Minor", &state.theme.grid_minor.x);
  ImGui::ColorEdit4("Guide", &state.theme.guide.x);
  ImGui::ColorEdit4("Guide Hovered", &state.theme.guide_hovered.x);
  ImGui::ColorEdit4("Guide Locked", &state.theme.guide_locked.x);
  ImGui::ColorEdit4("Working Area Fill", &state.theme.working_area_fill.x);
  ImGui::ColorEdit4("Working Area Border", &state.theme.working_area_border.x);
  ImGui::ColorEdit4("Working Area Selected",
                    &state.theme.working_area_selected.x);
  ImGui::ColorEdit4("Export Area Outline", &state.theme.export_area_outline.x);

  ImGui::Separator();
  ImGui::TextUnformatted("Navigation");
  ImGui::BulletText("Middle mouse: pan");
  ImGui::BulletText("Mouse wheel: zoom");
  ImGui::BulletText("Left drag from rulers: create guides");
  ImGui::BulletText("Left drag guide: move guide");
  ImGui::BulletText("Left drag work area: move when movable");
  ImGui::BulletText("Bottom-right handle: resize when resizable");
  ImGui::BulletText("Right click guide: lock or delete");
  ImGui::BulletText("Right click ruler: change units");

  ImGui::End();
}

int RunApp() {
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

  SDL_Window *window = SDL_CreateWindow(
      "im2d demo", 1600, 960, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
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

  CanvasState canvas_state;
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

    DrawInspector(canvas_state);

    ImGui::SetNextWindowPos(ImVec2(360.0f, 16.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(1200.0f, 900.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Canvas Demo");
    im2d::DrawCanvas(canvas_state);
    ImGui::End();

    ImGui::Render();
    int framebuffer_width = static_cast<int>(io.DisplaySize.x);
    int framebuffer_height = static_cast<int>(io.DisplaySize.y);
    glViewport(0, 0, framebuffer_width, framebuffer_height);
    glClearColor(0.07f, 0.08f, 0.09f, 1.0f);
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

} // namespace

int main(int, char **) {
  try {
    return RunApp();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
  }

  SDL_Quit();
  return 1;
}
