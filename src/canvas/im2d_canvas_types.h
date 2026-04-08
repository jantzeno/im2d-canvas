#pragma once

// Aggregator header — includes the decomposed type headers so that existing
// consumers can continue to #include "im2d_canvas_types.h" unchanged.

#include "im2d_canvas_geometry.h"
#include "im2d_canvas_settings.h"
#include "im2d_canvas_theme.h"
#include "im2d_document_types.h"

#include <imgui.h>

#include <cstdint>
#include <string>
#include <vector>

namespace im2d {

struct CanvasRuntimeInfo {
  bool valid = false;
  ImVec2 total_min = ImVec2(0.0f, 0.0f);
  ImVec2 total_max = ImVec2(0.0f, 0.0f);
  ImVec2 canvas_min = ImVec2(0.0f, 0.0f);
  ImVec2 canvas_max = ImVec2(0.0f, 0.0f);
  bool has_cursor_world = false;
  ImVec2 cursor_world = ImVec2(0.0f, 0.0f);
};

enum class CanvasNotificationDismissMode {
  CallerManaged,
  UserClosable,
};

using CanvasNotificationId = uint8_t;

struct CanvasNotificationState {
  bool active = false;
  CanvasNotificationId id = 0;
  CanvasNotificationDismissMode dismiss_mode =
      CanvasNotificationDismissMode::CallerManaged;
  std::string title;
  std::string summary;
};

struct CanvasClipboard {
  std::vector<ImportedArtwork> artworks;
  int paste_generation = 0;

  [[nodiscard]] bool has_content() const { return !artworks.empty(); }
};

struct CanvasUndoSnapshot {
  std::vector<ImportedArtwork> imported_artwork;
  int selected_imported_artwork_id = 0;
  std::vector<int> selected_imported_artwork_ids;
  ImportedDebugSelection selected_imported_debug;
  ImportedArtworkSelectionScope selection_scope =
      ImportedArtworkSelectionScope::Canvas;
  ImportedArtworkEditMode imported_artwork_edit_mode =
      ImportedArtworkEditMode::None;
  std::vector<ImportedElementSelection> selected_imported_elements;
  int next_imported_artwork_id = 1;
  int next_imported_part_id = 1;
};

struct UndoHistory {
  std::vector<CanvasUndoSnapshot> undo_stack;
  std::vector<CanvasUndoSnapshot> redo_stack;
  std::size_t max_snapshots = 50;
  int transaction_depth = 0;
};

struct ImportedIssueOverlaySettings {
  bool show_open_geometry = true;
  bool show_placeholder_text = true;
  bool show_orphan_hole = true;
  bool show_ambiguous_cleanup = true;
};

struct CanvasState {
  CanvasTheme theme;
  GridSettings grid;
  PhysicalCalibration calibration;
  ViewTransform view;
  SnapSettings snapping;
  ImportedIssueOverlaySettings imported_issue_overlays;
  MeasurementUnit ruler_unit = MeasurementUnit::Millimeters;
  RulerReference ruler_reference;
  std::vector<Guide> guides;
  std::vector<WorkingArea> working_areas;
  std::vector<ExportArea> export_areas;
  std::vector<ExclusionArea> exclusion_areas;
  std::vector<Layer> layers;
  std::vector<ImportedArtwork> imported_artwork;
  bool show_imported_dxf_text = true;
  int selected_working_area_id = 0;
  int selected_imported_artwork_id = 0;
  std::vector<int> selected_imported_artwork_ids;
  int selected_guide_id = 0;
  ImportedDebugSelection selected_imported_debug;
  ImportedArtworkSelectionScope selection_scope =
      ImportedArtworkSelectionScope::Canvas;
  ImportedArtworkEditMode imported_artwork_edit_mode =
      ImportedArtworkEditMode::None;
  std::vector<ImportedElementSelection> selected_imported_elements;
  CanvasClipboard clipboard;
  UndoHistory undo_history;
  ImportedArtworkSeparationPreview imported_artwork_separation_preview;
  ImportedArtworkAutoCutPreview imported_artwork_auto_cut_preview;
  CanvasNotificationState canvas_notification;
  ImportedArtworkOperationResult last_imported_artwork_operation;
  int last_imported_operation_issue_artwork_id = 0;
  bool highlight_last_imported_operation_issue_elements = false;
  std::vector<ImportedElementSelection> last_imported_operation_issue_elements;
  int next_guide_id = 1;
  int next_working_area_id = 1;
  int next_export_area_id = 1;
  int next_layer_id = 1;
  int next_imported_artwork_id = 1;
  int next_imported_part_id = 1;
  CanvasRuntimeInfo runtime;
};

struct CanvasWidgetOptions {
  float ruler_thickness = 28.0f;
  float min_working_area_size = 16.0f;
  float resize_handle_size = 10.0f;
  bool ensure_default_working_area = true;
  ImVec2 context_menu_padding = ImVec2(10.0f, 8.0f);
};

} // namespace im2d
