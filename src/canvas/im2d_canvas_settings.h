#pragma once

#include <imgui.h>

namespace im2d {

enum class MeasurementUnit {
  Pixels,
  Millimeters,
  Inches,
};

enum class SnapTargetKind {
  None,
  Guide,
  GridMajor,
  GridMinor,
  Margin,
};

struct GridSettings {
  bool visible = true;
  float spacing = 10.0f;
  int subdivisions = 5;
  MeasurementUnit unit = MeasurementUnit::Millimeters;
};

struct PhysicalCalibration {
  bool enabled = false;
  float default_pixels_per_mm = 96.0f / 25.4f;
  float calibrated_pixels_per_mm = 96.0f / 25.4f;
  float reference_pixels = 300.0f;
  float measured_length = 80.0f;
  MeasurementUnit measured_unit = MeasurementUnit::Millimeters;
};

struct ViewTransform {
  ImVec2 pan = ImVec2(96.0f, 96.0f);
  float zoom = 1.0f;
};

struct SnapSettings {
  bool to_guides = true;
  bool to_grid_major = true;
  bool to_grid_minor = false;
  bool to_margins = false;
  float screen_threshold = 10.0f;
};

struct RulerReference {
  bool enabled = false;
  ImVec2 origin_world = ImVec2(0.0f, 0.0f);
  float horizontal_direction = 1.0f;
  float vertical_direction = 1.0f;
};

struct Outlines {
  float outline_thickness = 1.0f;
  float selected_outline_thickness = 1.5f;
};

struct SnapResult {
  float value = 0.0f;
  bool snapped = false;
  SnapTargetKind target = SnapTargetKind::None;
  int guide_id = 0;
};

} // namespace im2d
