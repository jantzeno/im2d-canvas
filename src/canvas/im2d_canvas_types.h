#pragma once

#include <imgui.h>

#include <array>
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
    static_cast<uint32_t>(ImportedArtworkFlagResizable) |
    static_cast<uint32_t>(ImportedArtworkFlagLockScaleRatio);

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
  ImVec4 imported_issue_ambiguous_cleanup = ImVec4(0.24f, 0.69f, 0.65f, 1.0f);
  ImVec4 imported_issue_orphan_hole = ImVec4(0.91f, 0.33f, 0.24f, 1.0f);
  ImVec4 imported_issue_placeholder_text = ImVec4(0.91f, 0.67f, 0.24f, 1.0f);
  ImVec4 imported_issue_default = ImVec4(0.94f, 0.57f, 0.24f, 1.0f);
  ImVec4 operation_issue = ImVec4(0.35f, 0.67f, 1.0f, 1.0f);
  ImVec4 preview_assigned_stroke = ImVec4(0.35f, 0.63f, 1.0f, 0.92f);
  ImVec4 preview_assigned_fill = ImVec4(0.35f, 0.63f, 1.0f, 0.20f);
  ImVec4 preview_crossing_stroke = ImVec4(0.95f, 0.69f, 0.20f, 0.92f);
  ImVec4 preview_crossing_fill = ImVec4(0.95f, 0.69f, 0.20f, 0.20f);
  ImVec4 preview_orphan_stroke = ImVec4(0.91f, 0.33f, 0.24f, 0.92f);
  ImVec4 preview_orphan_fill = ImVec4(0.91f, 0.33f, 0.24f, 0.17f);
  ImVec4 preview_default_stroke = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
  ImVec4 preview_default_fill = ImVec4(1.0f, 1.0f, 1.0f, 0.13f);
  std::array<ImVec4, 6> preview_bucket_strokes = {
      ImVec4(0.35f, 0.63f, 1.0f, 0.92f), ImVec4(0.36f, 0.79f, 0.43f, 0.92f),
      ImVec4(0.76f, 0.47f, 1.0f, 0.92f), ImVec4(0.24f, 0.79f, 0.76f, 0.92f),
      ImVec4(1.0f, 0.53f, 0.36f, 0.92f), ImVec4(0.94f, 0.82f, 0.29f, 0.92f),
  };
  std::array<ImVec4, 6> preview_bucket_fills = {
      ImVec4(0.35f, 0.63f, 1.0f, 0.11f), ImVec4(0.36f, 0.79f, 0.43f, 0.11f),
      ImVec4(0.76f, 0.47f, 1.0f, 0.11f), ImVec4(0.24f, 0.79f, 0.76f, 0.11f),
      ImVec4(1.0f, 0.53f, 0.36f, 0.11f), ImVec4(0.94f, 0.82f, 0.29f, 0.11f),
  };
  ImVec4 preview_banner_background = ImVec4(0.08f, 0.09f, 0.13f, 0.88f);
  ImVec4 preview_banner_border = ImVec4(0.55f, 0.67f, 0.86f, 0.86f);
  ImVec4 preview_banner_title = ImVec4(0.90f, 0.93f, 0.97f, 1.0f);
  ImVec4 preview_banner_summary = ImVec4(0.71f, 0.77f, 0.89f, 1.0f);
  ImVec4 preview_label_background = ImVec4(0.06f, 0.07f, 0.10f, 0.86f);
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

enum ImportedElementIssueFlags : uint32_t {
  ImportedElementIssueFlagNone = 0,
  ImportedElementIssueFlagOpenGeometry = 1u << 0,
  ImportedElementIssueFlagPlaceholderText = 1u << 1,
  ImportedElementIssueFlagOrphanHole = 1u << 2,
  ImportedElementIssueFlagAmbiguousCleanup = 1u << 3,
};

constexpr bool HasImportedElementIssueFlag(uint32_t flags,
                                           ImportedElementIssueFlags flag) {
  return (flags & static_cast<uint32_t>(flag)) != 0;
}

enum class ImportedDebugSelectionKind {
  None,
  Artwork,
  Group,
  Path,
  DxfText,
};

struct ImportedDebugSelection {
  ImportedDebugSelectionKind kind = ImportedDebugSelectionKind::None;
  int artwork_id = 0;
  int item_id = 0;
};

enum class ImportedElementKind {
  Path,
  DxfText,
};

enum class ImportedArtworkEditMode {
  None,
  SelectRectangle,
  SelectOval,
};

struct ImportedElementSelection {
  ImportedElementKind kind = ImportedElementKind::Path;
  int item_id = 0;
};

enum class ImportedSeparationPreviewClassification {
  Assigned,
  Crossing,
  Orphan,
};

enum class AutoCutPreviewAxisMode {
  VerticalOnly,
  HorizontalOnly,
  Both,
};

struct ImportedSeparationPreviewPart {
  ImportedSeparationPreviewClassification classification =
      ImportedSeparationPreviewClassification::Assigned;
  int bucket_index = -1;
  int bucket_column = 0;
  int bucket_row = 0;
  ImVec2 world_bounds_min = ImVec2(0.0f, 0.0f);
  ImVec2 world_bounds_max = ImVec2(0.0f, 0.0f);
  std::vector<ImportedElementSelection> elements;
};

struct ImportedArtworkSeparationPreview {
  bool active = false;
  int artwork_id = 0;
  int guide_id = 0;
  std::vector<int> guide_ids;
  int future_object_count = 0;
  int skipped_count = 0;
  std::vector<ImportedElementSelection> skipped_elements;
  std::vector<ImportedSeparationPreviewPart> parts;
  std::string message;
};

struct ImportedArtworkAutoCutPreview {
  bool active = false;
  int artwork_id = 0;
  AutoCutPreviewAxisMode axis_mode = AutoCutPreviewAxisMode::Both;
  float minimum_gap = 5.0f;
  std::vector<float> vertical_positions;
  std::vector<float> horizontal_positions;
  int future_band_count = 0;
  int skipped_count = 0;
  std::vector<ImportedElementSelection> skipped_elements;
  std::vector<ImportedSeparationPreviewPart> parts;
  std::string message;
};

struct ImportedSourceReference {
  int source_artwork_id = 0;
  ImportedElementKind kind = ImportedElementKind::Path;
  int item_id = 0;
};

struct ImportedContourReference {
  ImportedElementKind kind = ImportedElementKind::Path;
  int item_id = 0;
  int contour_index = 0;
};

struct ImportedHoleOwnership {
  ImportedContourReference outer;
  ImportedContourReference hole;
};

enum class ImportedArtworkPrepareMode {
  FidelityFirst,
  AggressiveCleanup,
};

struct ImportedArtworkOperationResult {
  bool success = false;
  int artwork_id = 0;
  int created_artwork_id = 0;
  int part_id = 0;
  int selected_count = 0;
  int moved_count = 0;
  int skipped_count = 0;
  int preserved_count = 0;
  int stitched_count = 0;
  int cleaned_count = 0;
  int ambiguous_count = 0;
  int placeholder_count = 0;
  int outer_count = 0;
  int hole_count = 0;
  int island_count = 0;
  int attached_hole_count = 0;
  int orphan_hole_count = 0;
  int closed_count = 0;
  int open_count = 0;
  int auto_close_endpoint_count = 0;
  int auto_close_cluster_count = 0;
  int auto_close_group_count = 0;
  int auto_close_component_count = 0;
  int auto_close_pass_count = 0;
  float auto_close_elapsed_ms = 0.0f;
  bool cut_ready = false;
  bool nest_ready = false;
  ImportedArtworkPrepareMode prepare_mode =
      ImportedArtworkPrepareMode::FidelityFirst;
  std::string message;
};

struct ImportedArtworkPartMetadata {
  int part_id = 0;
  int source_artwork_id = 0;
  std::vector<int> contributing_source_artwork_ids;
  std::vector<ImportedContourReference> outer_contours;
  std::vector<ImportedHoleOwnership> hole_attachments;
  std::vector<ImportedContourReference> orphan_holes;
  int outer_contour_count = 0;
  int hole_contour_count = 0;
  int island_count = 0;
  int attached_hole_count = 0;
  int orphan_hole_count = 0;
  int ambiguous_contour_count = 0;
  int closed_contour_count = 0;
  int open_contour_count = 0;
  int placeholder_count = 0;
  bool cut_ready = false;
  bool nest_ready = false;
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
  std::vector<int> dxf_text_ids;
};

enum class ImportedTextHorizontalAlignment {
  Left,
  Center,
  Right,
};

enum class ImportedTextVerticalAlignment {
  Baseline,
  Bottom,
  Middle,
  Top,
};

enum class ImportedTextFillRule {
  NonZero,
};

enum class ImportedTextContourRole {
  Outline,
  Hole,
  Guide,
};

struct ImportedTextContour {
  std::string label;
  ImVec2 bounds_min = ImVec2(0.0f, 0.0f);
  ImVec2 bounds_max = ImVec2(0.0f, 0.0f);
  std::vector<ImportedPathSegment> segments;
  bool closed = true;
  ImportedTextContourRole role = ImportedTextContourRole::Outline;
};

struct ImportedTextGlyph {
  std::string label;
  ImVec2 bounds_min = ImVec2(0.0f, 0.0f);
  ImVec2 bounds_max = ImVec2(0.0f, 0.0f);
  std::vector<ImportedTextContour> contours;
};

struct ImportedDxfText {
  int id = 0;
  int parent_group_id = 0;
  std::string label;
  ImVec2 bounds_min = ImVec2(0.0f, 0.0f);
  ImVec2 bounds_max = ImVec2(0.0f, 0.0f);
  std::string source_text;
  bool multiline = false;
  ImVec2 anchor_point = ImVec2(0.0f, 0.0f);
  float text_height = 1.0f;
  float width_scale = 1.0f;
  float rotation_degrees = 0.0f;
  ImportedTextHorizontalAlignment horizontal_alignment =
      ImportedTextHorizontalAlignment::Left;
  ImportedTextVerticalAlignment vertical_alignment =
      ImportedTextVerticalAlignment::Baseline;
  float line_spacing_factor = 1.0f;
  ImportedTextFillRule fill_rule = ImportedTextFillRule::NonZero;
  std::string source_style_name;
  std::string requested_font_file;
  std::string resolved_font_path;
  bool substituted_font = false;
  bool placeholder_only = false;
  ImVec4 stroke_color = ImVec4(0.92f, 0.94f, 0.97f, 1.0f);
  float stroke_width = 1.0f;
  uint32_t issue_flags = ImportedElementIssueFlagNone;
  std::vector<ImportedSourceReference> provenance;
  std::vector<ImportedTextGlyph> glyphs;
  std::vector<ImportedTextContour> placeholder_contours;
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
  uint32_t issue_flags = ImportedElementIssueFlagNone;
  std::vector<ImportedSourceReference> provenance;
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
  int next_dxf_text_id = 1;
  std::vector<ImportedGroup> groups;
  std::vector<ImportedPath> paths;
  std::vector<ImportedDxfText> dxf_text;
  ImportedArtworkPartMetadata part;
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
  int selected_guide_id = 0;
  ImportedDebugSelection selected_imported_debug;
  ImportedArtworkEditMode imported_artwork_edit_mode =
      ImportedArtworkEditMode::None;
  std::vector<ImportedElementSelection> selected_imported_elements;
  ImportedArtworkSeparationPreview imported_artwork_separation_preview;
  ImportedArtworkAutoCutPreview imported_artwork_auto_cut_preview;
  ImportedArtworkOperationResult last_imported_artwork_operation;
  int last_imported_operation_issue_artwork_id = 0;
  std::vector<ImportedElementSelection> last_imported_operation_issue_elements;
  int next_guide_id = 1;
  int next_working_area_id = 1;
  int next_export_area_id = 1;
  int next_layer_id = 1;
  int next_imported_artwork_id = 1;
  int next_imported_part_id = 1;
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