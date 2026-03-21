#include "demo_app.h"

#include "../canvas/im2d_canvas.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <cstdio>
#include <stdexcept>

namespace {

using im2d::CanvasState;
using im2d::MeasurementUnit;

void InitializeDxfState(CanvasState &state) {
  state.grid.unit = MeasurementUnit::Millimeters;
  state.grid.spacing = 25.0f;
  state.grid.subdivisions = 5;
  state.ruler_unit = MeasurementUnit::Millimeters;
  state.view.zoom = 0.75f;
  state.view.pan = ImVec2(140.0f, 120.0f);
  state.theme.working_area_fill = ImVec4(0.20f, 0.18f, 0.16f, 0.94f);
  state.theme.working_area_border = ImVec4(0.87f, 0.60f, 0.29f, 1.0f);
  state.theme.export_area_outline = ImVec4(0.52f, 0.84f, 0.78f, 0.75f);

  if (!state.working_areas.empty()) {
    state.working_areas.front().name = "Machine Bed";
    state.working_areas.front().size =
        ImVec2(im2d::UnitsToPixels(1220.0f, MeasurementUnit::Millimeters,
                                   state.calibration),
               im2d::UnitsToPixels(2440.0f, MeasurementUnit::Millimeters,
                                   state.calibration));
    state.export_areas.front().size = state.working_areas.front().size;
  }

  if (state.working_areas.size() == 1) {
    im2d::WorkingAreaCreateInfo create_info;
    create_info.name = "Stock Preview";
    create_info.size_pixels =
        ImVec2(im2d::UnitsToPixels(610.0f, MeasurementUnit::Millimeters,
                                   state.calibration),
               im2d::UnitsToPixels(610.0f, MeasurementUnit::Millimeters,
                                   state.calibration));
    create_info.flags = im2d::kDefaultWorkingAreaFlags;
    im2d::AddWorkingArea(state, create_info);
  }

  if (state.layers.size() == 1) {
    state.layers.push_back(
        im2d::Layer{state.next_layer_id++, "Cut", true, false});
    state.layers.push_back(
        im2d::Layer{state.next_layer_id++, "Etch", true, false});
  }
}

void DrawDxfInspector(CanvasState &state) {
  ImGui::Begin("DXF Import Demo");
  ImGui::TextUnformatted(
      "Prepared for machine-bed and drawing-unit workflows.");
  ImGui::TextUnformatted("Sample assets live under samples/dxf.");
  ImGui::Separator();

  ImGui::Checkbox("Show Grid", &state.grid.visible);
  ImGui::InputFloat("Grid Spacing", &state.grid.spacing, 5.0f, 25.0f,
                    "%.1f mm");
  ImGui::SliderInt("Grid Subdivisions", &state.grid.subdivisions, 1, 10);
  ImGui::Checkbox("Snap to Guides", &state.snapping.to_guides);
  ImGui::Checkbox("Snap to Main Grid", &state.snapping.to_grid_major);
  ImGui::Checkbox("Snap to Subgrid", &state.snapping.to_grid_minor);

  if (ImGui::Button("Router Bed")) {
    if (!state.working_areas.empty()) {
      state.working_areas.front().size =
          ImVec2(im2d::UnitsToPixels(1220.0f, MeasurementUnit::Millimeters,
                                     state.calibration),
                 im2d::UnitsToPixels(2440.0f, MeasurementUnit::Millimeters,
                                     state.calibration));
      state.export_areas.front().size = state.working_areas.front().size;
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Bench Plate")) {
    if (!state.working_areas.empty()) {
      state.working_areas.front().size =
          ImVec2(im2d::UnitsToPixels(450.0f, MeasurementUnit::Millimeters,
                                     state.calibration),
                 im2d::UnitsToPixels(450.0f, MeasurementUnit::Millimeters,
                                     state.calibration));
      state.export_areas.front().size = state.working_areas.front().size;
    }
  }

  ImGui::Separator();
  ImGui::Text("Working Areas: %d",
              static_cast<int>(state.working_areas.size()));
  ImGui::Text("Export Areas: %d", static_cast<int>(state.export_areas.size()));
  ImGui::Text("Layers: %d", static_cast<int>(state.layers.size()));
  ImGui::End();
}

} // namespace

int main(int, char **) {
  try {
    demo::DemoConfig config;
    config.app_title = "im2d dxf_demo";
    config.canvas_window_title = "DXF Demo";
    config.initialize_state = InitializeDxfState;
    config.draw_inspector = DrawDxfInspector;
    config.clear_color = ImVec4(0.08f, 0.06f, 0.05f, 1.0f);
    return demo::RunDemoApp(config);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
  }

  SDL_Quit();
  return 1;
}