#include "demo_app.h"

#include "../canvas/im2d_canvas.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <array>
#include <cstdio>
#include <exception>
#include <string>

namespace {

using im2d::CanvasState;
using im2d::MeasurementUnit;

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

} // namespace

int main(int, char **) {
  try {
    demo::DemoConfig config;
    config.app_title = "im2d canvas_demo";
    config.canvas_window_title = "Canvas Demo";
    config.draw_inspector = DrawInspector;
    return demo::RunDemoApp(config);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
  }

  SDL_Quit();
  return 1;
}
