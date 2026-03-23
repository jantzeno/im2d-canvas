#include "demo_app.h"

#include "demo_imported_artwork_windows.h"

#include "demo_sample_browser.h"

#include "../common/im2d_log.h"
#include "../import/im2d_import.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <exception>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

using im2d::CanvasState;
using im2d::MeasurementUnit;

enum class DemoImportFormat {
  None,
  Svg,
  Dxf,
};

DemoImportFormat DetectImportFormat(const fs::path &path) {
  const std::string extension = path.extension().string();
  if (extension == ".dxf" || extension == ".DXF") {
    return DemoImportFormat::Dxf;
  }
  return DemoImportFormat::Svg;
}

void EnsureDxfLayers(CanvasState &state) {
  if (state.layers.size() != 1) {
    return;
  }

  state.layers.push_back(
      im2d::Layer{state.next_layer_id++, "Cut", true, false});
  state.layers.push_back(
      im2d::Layer{state.next_layer_id++, "Etch", true, false});
}

void ActivateSvgWorkflow(CanvasState &state) {
  state.grid.unit = MeasurementUnit::Pixels;
  state.grid.spacing = 32.0f;
  state.grid.subdivisions = 4;
  state.ruler_unit = MeasurementUnit::Pixels;
}

void ActivateDxfWorkflow(CanvasState &state) {
  state.grid.unit = MeasurementUnit::Millimeters;
  state.grid.spacing = 25.0f;
  state.grid.subdivisions = 5;
  state.ruler_unit = MeasurementUnit::Millimeters;
  EnsureDxfLayers(state);
}

void SetWorkingAreaSizeMillimeters(CanvasState &state, float width_mm,
                                   float height_mm) {
  if (state.working_areas.empty()) {
    return;
  }

  state.working_areas.front().size =
      ImVec2(im2d::UnitsToPixels(width_mm, MeasurementUnit::Millimeters,
                                 state.calibration),
             im2d::UnitsToPixels(height_mm, MeasurementUnit::Millimeters,
                                 state.calibration));
  state.export_areas.front().size = state.working_areas.front().size;
}

void InitializeImportState(CanvasState &state) {
  ActivateSvgWorkflow(state);
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

void DrawImportInspector(CanvasState &state) {
  static demo::SampleBrowserState browser_state;
  static im2d::importer::ImportResult last_import_result;
  static DemoImportFormat active_format = DemoImportFormat::None;
  fs::path clicked_path;
  if (demo::DrawSampleBrowserWindow("Samples", fs::path("samples"),
                                    {".svg", ".dxf"}, browser_state,
                                    &clicked_path)) {
    active_format = DetectImportFormat(clicked_path);
    if (active_format == DemoImportFormat::Dxf) {
      ActivateDxfWorkflow(state);
      last_import_result = im2d::importer::ImportDxfFile(state, clicked_path);
    } else {
      ActivateSvgWorkflow(state);
      last_import_result = im2d::importer::ImportSvgFile(state, clicked_path);
    }
  }

  ImGui::Begin("Import Demo");
  ImGui::TextUnformatted(
      "Prepared for viewport, artboard, and machine-bed experiments.");
  ImGui::TextUnformatted("Sample assets live under samples/ with collapsible "
                         "svg and dxf folders.");
  ImGui::Separator();

  const bool using_millimeters =
      state.grid.unit == MeasurementUnit::Millimeters;
  ImGui::Checkbox("Show Grid", &state.grid.visible);
  ImGui::InputFloat("Grid Spacing", &state.grid.spacing,
                    using_millimeters ? 5.0f : 4.0f,
                    using_millimeters ? 25.0f : 16.0f,
                    using_millimeters ? "%.1f mm" : "%.0f px");
  ImGui::SliderInt("Grid Subdivisions", &state.grid.subdivisions, 1,
                   using_millimeters ? 10 : 8);
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

  if (active_format == DemoImportFormat::Dxf) {
    ImGui::Separator();
    ImGui::TextUnformatted("DXF Workflow");
    ImGui::Checkbox("Snap to Guides", &state.snapping.to_guides);

    if (ImGui::Button("Router Bed")) {
      SetWorkingAreaSizeMillimeters(state, 1220.0f, 2440.0f);
    }
    ImGui::SameLine();
    if (ImGui::Button("Bench Plate")) {
      SetWorkingAreaSizeMillimeters(state, 450.0f, 450.0f);
    }
  }

  ImGui::Separator();
  ImGui::Text("Active Format: %s",
              active_format == DemoImportFormat::Dxf   ? "DXF"
              : active_format == DemoImportFormat::Svg ? "SVG"
                                                       : "SVG defaults");
  ImGui::Text("Working Areas: %d",
              static_cast<int>(state.working_areas.size()));
  ImGui::Text("Export Areas: %d", static_cast<int>(state.export_areas.size()));
  ImGui::Text("Layers: %d", static_cast<int>(state.layers.size()));
  ImGui::Text("Guides: %d", static_cast<int>(state.guides.size()));
  ImGui::Text("Artwork: %d", static_cast<int>(state.imported_artwork.size()));
  if (!last_import_result.message.empty()) {
    ImGui::Separator();
    demo::DrawImportResultSummary(last_import_result);
  }
  ImGui::End();

  demo::DrawImportedArtworkListWindow(state, "Canvas Objects");
  demo::DrawImportedArtworkInspectorWindow(state, "Object Inspector");
}

} // namespace

int main(int, char **) {
  try {
    demo::DemoConfig config;
    config.app_title = "im2d import_demo";
    config.canvas_window_title = "Import Demo";
    config.initialize_state = InitializeImportState;
    config.draw_inspector = DrawImportInspector;
    config.clear_color = ImVec4(0.05f, 0.07f, 0.10f, 1.0f);
    return demo::RunDemoApp(config);
  } catch (const std::exception &error) {
    im2d::log::InitializeLogger();
    im2d::log::GetLogger()->critical("{}", error.what());
  }

  SDL_Quit();
  return 1;
}