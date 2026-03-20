#pragma once

#include <imgui.h>

#include <string>
#include <vector>

namespace im2d {

enum class MeasurementUnit {
  Pixels,
  Millimeters,
  Inches,
};

enum class GuideOrientation {
  Vertical,
  Horizontal,
};

struct CanvasTheme {
  ImVec4 canvas_background = ImVec4(0.12f, 0.13f, 0.15f, 1.0f);
  ImVec4 ruler_background = ImVec4(0.16f, 0.17f, 0.20f, 1.0f);
  ImVec4 ruler_text = ImVec4(0.88f, 0.90f, 0.93f, 1.0f);
  ImVec4 ruler_ticks = ImVec4(0.52f, 0.56f, 0.63f, 1.0f);
  ImVec4 grid_major = ImVec4(0.30f, 0.35f, 0.41f, 0.60f);
  ImVec4 grid_minor = ImVec4(0.22f, 0.26f, 0.31f, 0.30f);
  ImVec4 guide = ImVec4(0.90f, 0.38f, 0.29f, 1.0f);
  ImVec4 guide_hovered = ImVec4(1.00f, 0.62f, 0.22f, 1.0f);
  ImVec4 guide_locked = ImVec4(0.72f, 0.72f, 0.76f, 1.0f);
  ImVec4 working_area_fill = ImVec4(0.19f, 0.22f, 0.19f, 0.92f);
  ImVec4 working_area_border = ImVec4(0.58f, 0.74f, 0.58f, 1.0f);
  ImVec4 export_area_outline = ImVec4(0.43f, 0.77f, 0.92f, 0.65f);
};

struct GridSettings {
  bool visible = true;
  float spacing = 10.0f;
  int subdivisions = 5;
  MeasurementUnit unit = MeasurementUnit::Millimeters;
};

struct ViewTransform {
  ImVec2 pan = ImVec2(96.0f, 96.0f);
  float zoom = 1.0f;
};

struct Guide {
  int id = 0;
  GuideOrientation orientation = GuideOrientation::Vertical;
  float position = 0.0f;
  bool locked = false;
};

struct WorkingArea {
  int id = 0;
  std::string name;
  ImVec2 origin = ImVec2(0.0f, 0.0f);
  ImVec2 size = ImVec2(0.0f, 0.0f);
  bool visible = true;
};

struct ExportArea {
  int id = 0;
  int source_working_area_id = 0;
  ImVec2 origin = ImVec2(0.0f, 0.0f);
  ImVec2 size = ImVec2(0.0f, 0.0f);
  bool visible = true;
};

struct Layer {
  int id = 0;
  std::string name;
  bool visible = true;
  bool locked = false;
};

struct CanvasState {
  CanvasTheme theme;
  GridSettings grid;
  ViewTransform view;
  MeasurementUnit ruler_unit = MeasurementUnit::Millimeters;
  float pixels_per_mm = 96.0f / 25.4f;
  std::vector<Guide> guides;
  std::vector<WorkingArea> working_areas;
  std::vector<ExportArea> export_areas;
  std::vector<Layer> layers;
  int next_guide_id = 1;
  int next_working_area_id = 1;
  int next_export_area_id = 1;
  int next_layer_id = 1;
};

struct CanvasWidgetOptions {
  float ruler_thickness = 28.0f;
};

void InitializeDefaultDocument(CanvasState &state);
int AddWorkingArea(CanvasState &state, const std::string &name,
                   ImVec2 size_pixels);
bool DrawCanvas(CanvasState &state, const CanvasWidgetOptions &options = {});
const char *MeasurementUnitLabel(MeasurementUnit unit);
float UnitsToPixels(float value, MeasurementUnit unit, float pixels_per_mm);
float PixelsToUnits(float value, MeasurementUnit unit, float pixels_per_mm);

} // namespace im2d