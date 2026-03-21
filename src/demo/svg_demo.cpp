#include "demo_app.h"

#include "../canvas/im2d_canvas.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <cstdio>
#include <stdexcept>

namespace {

using im2d::CanvasState;
using im2d::MeasurementUnit;

void InitializeSvgState(CanvasState &state) {
  state.grid.unit = MeasurementUnit::Pixels;
  state.grid.spacing = 32.0f;
  state.grid.subdivisions = 4;
  state.ruler_unit = MeasurementUnit::Pixels;
  state.view.zoom = 0.9f;
  state.view.pan = ImVec2(128.0f, 128.0f);
  state.theme.working_area_fill = ImVec4(0.18f, 0.20f, 0.26f, 0.95f);
  state.theme.working_area_border = ImVec4(0.44f, 0.77f, 0.92f, 1.0f);
  state.theme.export_area_outline = ImVec4(0.95f, 0.74f, 0.28f, 0.75f);

  if (!state.working_areas.empty()) {
    state.working_areas.front().name = "SVG ViewBox";
    state.working_areas.front().size = ImVec2(512.0f, 512.0f);
    state.export_areas.front().size = state.working_areas.front().size;
  }

  if (state.working_areas.size() == 1) {
    im2d::WorkingAreaCreateInfo create_info;
    create_info.name = "Poster Bounds";
    create_info.size_pixels = ImVec2(768.0f, 1080.0f);
    create_info.flags = im2d::kDefaultWorkingAreaFlags;
    im2d::AddWorkingArea(state, create_info);
  }
}

void DrawSvgInspector(CanvasState &state) {
  ImGui::Begin("SVG Import Demo");
  ImGui::TextUnformatted("Prepared for viewport and artboard experiments.");
  ImGui::TextUnformatted("Sample assets live under samples/svg.");
  ImGui::Separator();

  ImGui::Checkbox("Show Grid", &state.grid.visible);
  ImGui::InputFloat("Grid Spacing", &state.grid.spacing, 4.0f, 16.0f,
                    "%.0f px");
  ImGui::SliderInt("Grid Subdivisions", &state.grid.subdivisions, 1, 8);
  ImGui::Checkbox("Snap to Main Grid", &state.snapping.to_grid_major);
  ImGui::Checkbox("Snap to Subgrid", &state.snapping.to_grid_minor);
  ImGui::SliderFloat("Snap Threshold", &state.snapping.screen_threshold, 2.0f,
                     20.0f, "%.1f px");

  if (ImGui::Button("Square ViewBox")) {
    if (!state.working_areas.empty()) {
      state.working_areas.front().size = ImVec2(512.0f, 512.0f);
      state.export_areas.front().size = state.working_areas.front().size;
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Poster ViewBox")) {
    if (!state.working_areas.empty()) {
      state.working_areas.front().size = ImVec2(1080.0f, 1350.0f);
      state.export_areas.front().size = state.working_areas.front().size;
    }
  }

  ImGui::Separator();
  ImGui::Text("Working Areas: %d",
              static_cast<int>(state.working_areas.size()));
  ImGui::Text("Export Areas: %d", static_cast<int>(state.export_areas.size()));
  ImGui::Text("Guides: %d", static_cast<int>(state.guides.size()));
  ImGui::End();
}

} // namespace

int main(int, char **) {
  try {
    demo::DemoConfig config;
    config.app_title = "im2d svg_demo";
    config.canvas_window_title = "SVG Demo";
    config.initialize_state = InitializeSvgState;
    config.draw_inspector = DrawSvgInspector;
    config.clear_color = ImVec4(0.05f, 0.07f, 0.10f, 1.0f);
    return demo::RunDemoApp(config);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
  }

  SDL_Quit();
  return 1;
}