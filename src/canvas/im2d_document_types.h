#pragma once

#include <imgui.h>

#include <cstdint>
#include <string>
#include <vector>

namespace im2d {

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

enum class ImportedArtworkSelectionScope {
  Canvas,
  Object,
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
  int repaired_hole_count = 0;
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

} // namespace im2d
