#pragma once

#include <imgui.h>

#include <cstdint>
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

enum class SnapTargetKind {
  None,
  Guide,
  GridMajor,
  GridMinor,
};

enum WorkingAreaFlags : uint32_t {
  WorkingAreaFlagNone = 0,
  WorkingAreaFlagMovable = 1u << 0,
  WorkingAreaFlagResizable = 1u << 1,
};

constexpr uint32_t kDefaultWorkingAreaFlags =
    static_cast<uint32_t>(WorkingAreaFlagMovable) |
    static_cast<uint32_t>(WorkingAreaFlagResizable);

constexpr bool HasWorkingAreaFlag(uint32_t flags, WorkingAreaFlags flag) {
  return (flags & static_cast<uint32_t>(flag)) != 0;
}

enum ImportedArtworkFlags : uint32_t {
  ImportedArtworkFlagNone = 0,
  ImportedArtworkFlagMovable = 1u << 0,
  ImportedArtworkFlagResizable = 1u << 1,
  ImportedArtworkFlagLockScaleRatio = 1u << 2,
};

constexpr uint32_t kDefaultImportedArtworkFlags =
    static_cast<uint32_t>(ImportedArtworkFlagMovable) |
    static_cast<uint32_t>(ImportedArtworkFlagResizable);

constexpr bool HasImportedArtworkFlag(uint32_t flags,
                                      ImportedArtworkFlags flag) {
  return (flags & static_cast<uint32_t>(flag)) != 0;
}

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
  ImVec4 working_area_selected = ImVec4(0.97f, 0.82f, 0.36f, 1.0f);
  ImVec4 export_area_outline = ImVec4(0.43f, 0.77f, 0.92f, 0.65f);
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
  float screen_threshold = 10.0f;
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
  uint32_t flags = kDefaultWorkingAreaFlags;
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

enum class ImportedPathSegmentKind {
  Line,
  CubicBezier,
};

struct ImportedPathSegment {
  ImportedPathSegmentKind kind = ImportedPathSegmentKind::Line;
  ImVec2 start = ImVec2(0.0f, 0.0f);
  ImVec2 control1 = ImVec2(0.0f, 0.0f);
  ImVec2 control2 = ImVec2(0.0f, 0.0f);
  ImVec2 end = ImVec2(0.0f, 0.0f);
};

enum ImportedPathFlags : uint32_t {
  ImportedPathFlagNone = 0,
  ImportedPathFlagTextPlaceholder = 1u << 0,
  ImportedPathFlagFilledText = 1u << 1,
  ImportedPathFlagHoleContour = 1u << 2,
};

constexpr bool HasImportedPathFlag(uint32_t flags, ImportedPathFlags flag) {
  return (flags & static_cast<uint32_t>(flag)) != 0;
}

enum class ImportedDebugSelectionKind {
  None,
  Artwork,
  Group,
  Path,
};

struct ImportedDebugSelection {
  ImportedDebugSelectionKind kind = ImportedDebugSelectionKind::None;
  int artwork_id = 0;
  int item_id = 0;
};

struct ImportedGroup {
  int id = 0;
  int parent_group_id = 0;
  std::string label;
  std::string source_id;
  ImVec2 bounds_min = ImVec2(0.0f, 0.0f);
  ImVec2 bounds_max = ImVec2(0.0f, 0.0f);
  std::vector<int> child_group_ids;
  std::vector<int> path_ids;
};

struct ImportedPath {
  int id = 0;
  int parent_group_id = 0;
  std::string label;
  ImVec2 bounds_min = ImVec2(0.0f, 0.0f);
  ImVec2 bounds_max = ImVec2(0.0f, 0.0f);
  std::vector<ImportedPathSegment> segments;
  ImVec4 stroke_color = ImVec4(0.92f, 0.94f, 0.97f, 1.0f);
  float stroke_width = 1.0f;
  bool closed = false;
  uint32_t flags = ImportedPathFlagNone;
};

struct ImportedArtwork {
  int id = 0;
  std::string name;
  std::string source_path;
  std::string source_format;
  ImVec2 origin = ImVec2(0.0f, 0.0f);
  ImVec2 bounds_min = ImVec2(0.0f, 0.0f);
  ImVec2 bounds_max = ImVec2(0.0f, 0.0f);
  ImVec2 scale = ImVec2(1.0f, 1.0f);
  int root_group_id = 0;
  int next_group_id = 1;
  int next_path_id = 1;
  std::vector<ImportedGroup> groups;
  std::vector<ImportedPath> paths;
  bool visible = true;
  uint32_t flags = kDefaultImportedArtworkFlags;
};

struct CanvasState {
  CanvasTheme theme;
  GridSettings grid;
  PhysicalCalibration calibration;
  ViewTransform view;
  SnapSettings snapping;
  MeasurementUnit ruler_unit = MeasurementUnit::Millimeters;
  std::vector<Guide> guides;
  std::vector<WorkingArea> working_areas;
  std::vector<ExportArea> export_areas;
  std::vector<Layer> layers;
  std::vector<ImportedArtwork> imported_artwork;
  bool show_imported_dxf_text = true;
  int selected_imported_artwork_id = 0;
  ImportedDebugSelection selected_imported_debug;
  int next_guide_id = 1;
  int next_working_area_id = 1;
  int next_export_area_id = 1;
  int next_layer_id = 1;
  int next_imported_artwork_id = 1;
};

struct CanvasWidgetOptions {
  float ruler_thickness = 28.0f;
  float min_working_area_size = 16.0f;
  float resize_handle_size = 10.0f;
};

struct WorkingAreaCreateInfo {
  std::string name;
  ImVec2 size_pixels = ImVec2(0.0f, 0.0f);
  uint32_t flags = kDefaultWorkingAreaFlags;
};

struct SnapResult {
  float value = 0.0f;
  bool snapped = false;
  SnapTargetKind target = SnapTargetKind::None;
  int guide_id = 0;
};

} // namespace im2d