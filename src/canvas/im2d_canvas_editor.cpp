#include "im2d_canvas_editor.h"

#include "im2d_canvas_document.h"
#include "im2d_canvas_imported_artwork_ops.h"
#include "im2d_canvas_widget_internal.h"

#include <cfloat>
#include <cmath>
#include <string>

namespace im2d::detail {

namespace {

constexpr ImVec2 kToolWindowInitialSize(420.0f, 280.0f);
constexpr float kToolWindowMargin = 12.0f;

struct WeldAnalysisSummary {
  int target_count = 0;
  int open_contour_count = 0;
  int closed_contour_count = 0;
  int orphan_hole_count = 0;
  int ambiguous_contour_count = 0;
};

float ClampToRange(const float value, const float min_value,
                   const float max_value) {
  return std::max(min_value, std::min(value, max_value));
}

ImVec2 ComputeToolWindowPosition(const CanvasState &state,
                                 const ImportedArtwork &artwork,
                                 const ImVec2 &fallback_position) {
  if (!state.runtime.valid) {
    return fallback_position;
  }

  const ImRect artwork_rect =
      ImportedArtworkScreenRect(state, state.runtime.canvas_min, artwork);
  const ImVec2 canvas_min = state.runtime.canvas_min;
  const ImVec2 canvas_max = state.runtime.canvas_max;

  float x = artwork_rect.Max.x + kToolWindowMargin;
  if (x + kToolWindowInitialSize.x > canvas_max.x) {
    x = artwork_rect.Min.x - kToolWindowInitialSize.x - kToolWindowMargin;
  }
  x = ClampToRange(
      x, canvas_min.x,
      std::max(canvas_min.x, canvas_max.x - kToolWindowInitialSize.x));

  const float preferred_y = artwork_rect.Min.y;
  const float y = ClampToRange(
      preferred_y, canvas_min.y,
      std::max(canvas_min.y, canvas_max.y - kToolWindowInitialSize.y));

  return ImVec2(x, y);
}

WeldAnalysisSummary BuildWeldAnalysisSummary(const CanvasState &state,
                                             const int fallback_artwork_id) {
  WeldAnalysisSummary summary;
  const std::vector<int> target_artwork_ids =
      ResolveImportedArtworkOperationTargets(state, fallback_artwork_id);
  for (const int artwork_id : target_artwork_ids) {
    const ImportedArtwork *target = FindImportedArtwork(state, artwork_id);
    if (target == nullptr) {
      continue;
    }

    summary.target_count += 1;
    summary.open_contour_count += target->part.open_contour_count;
    summary.closed_contour_count += target->part.closed_contour_count;
    summary.orphan_hole_count += target->part.orphan_hole_count;
    summary.ambiguous_contour_count += target->part.ambiguous_contour_count;
  }
  return summary;
}

} // namespace

std::string CanvasEditor::ImportedArtworkLabel(const ImportedArtwork &artwork) {
  if (!artwork.name.empty()) {
    return artwork.name;
  }
  return "Artwork " + std::to_string(artwork.id);
}

const char *
CanvasEditor::AutoCutDirectionLabel(const AutoCutPreviewAxisMode axis_mode) {
  switch (axis_mode) {
  case AutoCutPreviewAxisMode::VerticalOnly:
    return "Vertical";
  case AutoCutPreviewAxisMode::HorizontalOnly:
    return "Horizontal";
  case AutoCutPreviewAxisMode::Both:
    return "Both";
  }
  return "Both";
}

bool CanvasEditor::HasArtworkPreview(const CanvasState &state,
                                     const int artwork_id) {
  return (state.imported_artwork_separation_preview.active &&
          state.imported_artwork_separation_preview.artwork_id == artwork_id) ||
         (state.imported_artwork_auto_cut_preview.active &&
          state.imported_artwork_auto_cut_preview.artwork_id == artwork_id);
}

bool CanvasEditor::AutoCutPreviewMatchesToolSettings(const CanvasState &state,
                                                     const int artwork_id) {
  const ImportedArtworkToolSettings &tool_settings =
      state.imported_artwork_tool_settings;
  return state.imported_artwork_auto_cut_preview.active &&
         state.imported_artwork_auto_cut_preview.artwork_id == artwork_id &&
         state.imported_artwork_auto_cut_preview.axis_mode ==
             tool_settings.split_axis &&
         std::abs(state.imported_artwork_auto_cut_preview.minimum_gap -
                  tool_settings.minimum_gap) <= 0.001f;
}

void CanvasEditor::ClearPreviewStateForArtwork(CanvasState &state,
                                               const int artwork_id) {
  if (artwork_id == 0) {
    return;
  }

  if (state.imported_artwork_separation_preview.active &&
      state.imported_artwork_separation_preview.artwork_id == artwork_id) {
    ClearImportedArtworkSeparationPreview(state);
  }
  if (state.imported_artwork_auto_cut_preview.active &&
      state.imported_artwork_auto_cut_preview.artwork_id == artwork_id) {
    ClearImportedArtworkAutoCutPreview(state);
  }
}

void CanvasEditor::OpenWeld(const int artwork_id) {
  if (artwork_id == 0) {
    return;
  }

  if (!imported_artwork_tool_window_open_) {
    tool_window_open_position_ = ImGui::GetMousePos();
    position_tool_window_on_open_ = true;
  }
  imported_artwork_tool_popup_ = ImportedArtworkTool::Weld;
  imported_artwork_tool_popup_artwork_id_ = artwork_id;
  imported_artwork_tool_window_open_ = true;
}

void CanvasEditor::OpenSplit(const int artwork_id) {
  if (artwork_id == 0) {
    return;
  }

  if (!imported_artwork_tool_window_open_) {
    tool_window_open_position_ = ImGui::GetMousePos();
    position_tool_window_on_open_ = true;
  }
  imported_artwork_tool_popup_ = ImportedArtworkTool::Split;
  imported_artwork_tool_popup_artwork_id_ = artwork_id;
  imported_artwork_tool_window_open_ = true;
}

void CanvasEditor::ResetImportedArtworkToolPopup() {
  imported_artwork_tool_popup_ = ImportedArtworkTool::None;
  imported_artwork_tool_popup_artwork_id_ = 0;
  imported_artwork_tool_window_open_ = false;
  position_tool_window_on_open_ = false;
}

const char *CanvasEditor::WindowTitle() const {
  switch (imported_artwork_tool_popup_) {
  case ImportedArtworkTool::Weld:
    return "Weld Geometry###CanvasImportedArtworkToolWindow";
  case ImportedArtworkTool::Split:
    return "Split Geometry###CanvasImportedArtworkToolWindow";
  case ImportedArtworkTool::None:
    break;
  }
  return "Artwork Tools###CanvasImportedArtworkToolWindow";
}

void CanvasEditor::DrawWeldImportedArtworkPopup(
    CanvasState &state, const ImportedArtwork &artwork) {
  const bool canvas_scope =
      state.selection_scope == ImportedArtworkSelectionScope::Canvas;
  const WeldAnalysisSummary analysis =
      BuildWeldAnalysisSummary(state, artwork.id);
  const bool has_artwork = analysis.target_count > 0;
  const bool can_weld = canvas_scope && has_artwork;
  const std::string artwork_label = ImportedArtworkLabel(artwork);

  ImGui::Text("Artwork: %s", artwork_label.c_str());
  ImGui::TextDisabled(
      "Click another artwork on the canvas to retarget this window.");
  if (analysis.target_count > 1) {
    ImGui::TextDisabled("Weld will run on %d selected artworks.",
                        analysis.target_count);
  }
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::TextUnformatted("Analysis");
  ImGui::Text("Open Contours: %d", analysis.open_contour_count);
  ImGui::Text("Closed Contours: %d", analysis.closed_contour_count);
  ImGui::Text("Orphan Holes: %d", analysis.orphan_hole_count);
  ImGui::Text("Ambiguous Contours: %d", analysis.ambiguous_contour_count);
  if (analysis.open_contour_count > 0) {
    ImGui::TextWrapped(
        "Detected open geometry. Welding can merge nearby endpoints within "
        "the tolerance below.");
  } else {
    ImGui::TextWrapped(
        "No open contours are currently detected. Welding is probably not "
        "needed for this artwork.");
  }
  ImGui::Spacing();
  ImGui::TextUnformatted("Weld Tolerance (px)");
  ImGui::SetNextItemWidth(-FLT_MIN);
  ImGui::DragFloat("##WeldTolerance",
                   &state.imported_artwork_tool_settings.weld_tolerance, 0.01f,
                   0.01f, 20.0f, "%.2f px");

  if (!canvas_scope) {
    ImGui::TextDisabled("Canvas scope exposes artwork-level repair actions.");
  }

  if (!can_weld) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button("Weld", ImVec2(-FLT_MIN, 0.0f))) {
    const ImportedArtworkOperationResult result =
        ApplyImportedArtworkOperationToSelection(
            state, artwork.id, "Join Open Segments",
            [](CanvasState &callback_state, const int target_artwork_id) {
              return JoinImportedArtworkOpenSegments(
                  callback_state, target_artwork_id,
                  callback_state.imported_artwork_tool_settings.weld_tolerance);
            });
    (void)result;
  }
  if (!can_weld) {
    ImGui::EndDisabled();
  }

  if (!has_artwork) {
    ImGui::TextDisabled("Select artwork to inspect and repair geometry.");
    return;
  }

  const ImportedArtworkOperationResult &result =
      state.last_imported_artwork_operation;
  if (!result.message.empty()) {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextWrapped("%s", result.message.c_str());
  }
}

void CanvasEditor::DrawSplitImportedArtworkPopup(
    CanvasState &state, const ImportedArtwork &artwork) {
  const int artwork_id = artwork.id;
  const bool has_any_preview = HasArtworkPreview(state, artwork_id);
  const bool has_active_auto_split_preview =
      state.imported_artwork_auto_cut_preview.active &&
      state.imported_artwork_auto_cut_preview.artwork_id == artwork_id;
  const bool has_valid_auto_split_preview =
      has_active_auto_split_preview &&
      state.imported_artwork_auto_cut_preview.future_band_count > 1;
  const bool auto_split_preview_matches_settings =
      AutoCutPreviewMatchesToolSettings(state, artwork_id);
  const bool can_preview_auto_split = artwork_id != 0;
  const bool can_apply_auto_split = artwork_id != 0 &&
                                    auto_split_preview_matches_settings &&
                                    has_valid_auto_split_preview;
  const std::string artwork_label = ImportedArtworkLabel(artwork);

  ImGui::Text("Artwork: %s", artwork_label.c_str());
  ImGui::TextDisabled(
      "Click another artwork on the canvas to retarget this window.");
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::TextUnformatted("Direction");
  int axis_value =
      static_cast<int>(state.imported_artwork_tool_settings.split_axis);
  if (ImGui::RadioButton(
          "Horizontal", &axis_value,
          static_cast<int>(AutoCutPreviewAxisMode::HorizontalOnly))) {
    state.imported_artwork_tool_settings.split_axis =
        AutoCutPreviewAxisMode::HorizontalOnly;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton(
          "Vertical", &axis_value,
          static_cast<int>(AutoCutPreviewAxisMode::VerticalOnly))) {
    state.imported_artwork_tool_settings.split_axis =
        AutoCutPreviewAxisMode::VerticalOnly;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("Both", &axis_value,
                         static_cast<int>(AutoCutPreviewAxisMode::Both))) {
    state.imported_artwork_tool_settings.split_axis =
        AutoCutPreviewAxisMode::Both;
  }

  ImGui::Spacing();
  ImGui::TextUnformatted("Minimum Gap (px)");
  if (ImGui::BeginTable("##SplitActions", 2,
                        ImGuiTableFlags_SizingStretchSame)) {
    ImGui::TableNextRow();

    ImGui::TableSetColumnIndex(0);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::DragFloat("##MinimumGap",
                     &state.imported_artwork_tool_settings.minimum_gap, 0.25f,
                     0.25f, 1000.0f, "%.2f px");

    ImGui::TableSetColumnIndex(1);
    if (!can_preview_auto_split) {
      ImGui::BeginDisabled();
    }
    if (ImGui::Button("Preview", ImVec2(-FLT_MIN, 0.0f))) {
      PreviewImportedArtworkAutoCut(
          state, artwork_id, state.imported_artwork_tool_settings.split_axis,
          state.imported_artwork_tool_settings.minimum_gap);
    }
    if (!can_preview_auto_split) {
      ImGui::EndDisabled();
    }

    ImGui::TableNextRow();

    ImGui::TableSetColumnIndex(0);
    if (!has_any_preview) {
      ImGui::BeginDisabled();
    }
    if (ImGui::Button("Clear Preview", ImVec2(-FLT_MIN, 0.0f))) {
      ClearPreviewStateForArtwork(state, artwork_id);
    }
    if (!has_any_preview) {
      ImGui::EndDisabled();
    }

    ImGui::TableSetColumnIndex(1);
    if (!can_apply_auto_split) {
      ImGui::BeginDisabled();
    }
    if (ImGui::Button("Apply Split", ImVec2(-FLT_MIN, 0.0f))) {
      const ImportedArtworkOperationResult result = ApplyImportedArtworkAutoCut(
          state, artwork_id, state.imported_artwork_tool_settings.split_axis,
          state.imported_artwork_tool_settings.minimum_gap);
      (void)result;
    }
    if (!can_apply_auto_split) {
      ImGui::EndDisabled();
    }

    ImGui::EndTable();
  }

  if (has_active_auto_split_preview) {
    const ImportedArtworkAutoCutPreview &preview =
        state.imported_artwork_auto_cut_preview;
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Preview Direction: %s",
                AutoCutDirectionLabel(preview.axis_mode));
    ImGui::Text("Future Bands: %d", preview.future_band_count);
    ImGui::Text("Skipped Elements: %d", preview.skipped_count);
    if (!preview.message.empty()) {
      ImGui::TextWrapped("%s", preview.message.c_str());
    }
  }

  if (artwork_id != 0 && !can_apply_auto_split) {
    if (!has_active_auto_split_preview) {
      ImGui::TextDisabled("Run Preview to enable applying the split.");
    } else if (!auto_split_preview_matches_settings) {
      ImGui::TextDisabled(
          "Preview again after changing direction or minimum gap.");
    } else if (!has_valid_auto_split_preview) {
      ImGui::TextDisabled(
          "Apply Split requires a preview with more than one band.");
    }
  }
}

void CanvasEditor::Draw(CanvasState &state) {
  if (!imported_artwork_tool_window_open_) {
    return;
  }

  if (state.selected_imported_artwork_id != 0 &&
      state.selected_imported_artwork_id !=
          imported_artwork_tool_popup_artwork_id_) {
    imported_artwork_tool_popup_artwork_id_ =
        state.selected_imported_artwork_id;
  }

  if (position_tool_window_on_open_) {
    if (ImportedArtwork *artwork =
            FindImportedArtwork(state, imported_artwork_tool_popup_artwork_id_);
        artwork != nullptr) {
      tool_window_open_position_ = ComputeToolWindowPosition(
          state, *artwork, tool_window_open_position_);
    }
    ImGui::SetNextWindowPos(tool_window_open_position_, ImGuiCond_Appearing);
    position_tool_window_on_open_ = false;
  }
  ImGui::SetNextWindowSize(kToolWindowInitialSize, ImGuiCond_FirstUseEver);

  bool window_open = imported_artwork_tool_window_open_;
  if (!ImGui::Begin(WindowTitle(), &window_open)) {
    imported_artwork_tool_window_open_ = window_open;
    if (!imported_artwork_tool_window_open_) {
      ResetImportedArtworkToolPopup();
    }
    ImGui::End();
    return;
  }

  imported_artwork_tool_window_open_ = window_open;
  if (!imported_artwork_tool_window_open_) {
    ResetImportedArtworkToolPopup();
    ImGui::End();
    return;
  }

  ImportedArtwork *artwork =
      FindImportedArtwork(state, imported_artwork_tool_popup_artwork_id_);
  if (artwork == nullptr) {
    ImGui::TextDisabled("The current target is no longer available. Select "
                        "another artwork on the canvas to continue.");
    ImGui::Spacing();
    if (ImGui::Button("Close", ImVec2(-FLT_MIN, 0.0f))) {
      ResetImportedArtworkToolPopup();
    }
    ImGui::End();
    return;
  }

  switch (imported_artwork_tool_popup_) {
  case ImportedArtworkTool::Weld:
    DrawWeldImportedArtworkPopup(state, *artwork);
    break;
  case ImportedArtworkTool::Split:
    DrawSplitImportedArtworkPopup(state, *artwork);
    break;
  case ImportedArtworkTool::None:
    ResetImportedArtworkToolPopup();
    break;
  }

  ImGui::End();
}

} // namespace im2d::detail
