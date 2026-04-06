#include "demo_imported_artwork_windows.h"

#include "../canvas/im2d_canvas_document.h"
#include "../canvas/im2d_canvas_imported_artwork_ops.h"
#include "../export/im2d_export_svg.h"
#include "../operations/im2d_operations.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <future>

#include <imgui.h>

namespace demo {

namespace {

enum class ImportedInspectorFilterMode {
  Hierarchy,
  FlaggedOnly,
  SkippedOnly,
  FlaggedOrSkipped,
};

ImportedInspectorFilterMode &GetImportedInspectorFilterMode() {
  static ImportedInspectorFilterMode mode =
      ImportedInspectorFilterMode::Hierarchy;
  return mode;
}

im2d::exporter::SvgExportResult &GetSvgExportPreview() {
  static im2d::exporter::SvgExportResult result;
  return result;
}

struct SvgExportUiState {
  int configured_artwork_id = 0;
  int configured_export_area_id = 0;
  bool allow_placeholder_text = false;
  std::array<char, 256> output_directory = {};
  std::array<char, 128> selection_filename = {};
  std::array<char, 128> export_area_filename = {};
};

struct PrepareWorkflowUiState {
  bool auto_close_before_prepare = false;
};

struct AutoCutPreviewUiState {
  im2d::AutoCutPreviewAxisMode axis_mode = im2d::AutoCutPreviewAxisMode::Both;
  float minimum_gap = 5.0f;
};

struct CutOperationUiState {
  bool auto_group_cut_outputs = false;
};

SvgExportUiState &GetSvgExportUiState() {
  static SvgExportUiState state;
  return state;
}

PrepareWorkflowUiState &GetPrepareWorkflowUiState() {
  static PrepareWorkflowUiState state;
  return state;
}

AutoCutPreviewUiState &GetAutoCutPreviewUiState() {
  static AutoCutPreviewUiState state;
  return state;
}

CutOperationUiState &GetCutOperationUiState() {
  static CutOperationUiState state;
  return state;
}

bool HasActiveSeparationPreview(const im2d::CanvasState &state,
                                int artwork_id) {
  return state.imported_artwork_separation_preview.active &&
         state.imported_artwork_separation_preview.artwork_id == artwork_id;
}

bool HasActiveAutoCutPreview(const im2d::CanvasState &state, int artwork_id) {
  return state.imported_artwork_auto_cut_preview.active &&
         state.imported_artwork_auto_cut_preview.artwork_id == artwork_id;
}

void ClearCutOperationPreviews(im2d::CanvasState &state) {
  im2d::ClearImportedArtworkSeparationPreview(state);
  im2d::ClearImportedArtworkAutoCutPreview(state);
}

const char *AutoCutAxisModeLabel(im2d::AutoCutPreviewAxisMode axis_mode) {
  switch (axis_mode) {
  case im2d::AutoCutPreviewAxisMode::VerticalOnly:
    return "Vertical Only";
  case im2d::AutoCutPreviewAxisMode::HorizontalOnly:
    return "Horizontal Only";
  case im2d::AutoCutPreviewAxisMode::Both:
    return "Both Axes";
  }
  return "Both Axes";
}

int GetPreviewAnchorGuideId(const im2d::CanvasState &state, int artwork_id) {
  if (state.selected_guide_id != 0 &&
      im2d::FindGuide(state, state.selected_guide_id) != nullptr) {
    return state.selected_guide_id;
  }
  if (HasActiveSeparationPreview(state, artwork_id) &&
      state.imported_artwork_separation_preview.guide_id != 0) {
    return state.imported_artwork_separation_preview.guide_id;
  }
  if (!state.guides.empty()) {
    return state.guides.front().id;
  }
  return 0;
}

void DrawPreviewAnchorGuideSelector(im2d::CanvasState &state,
                                    int current_guide_id) {
  const im2d::Guide *current_guide = im2d::FindGuide(state, current_guide_id);
  std::string preview_label =
      current_guide == nullptr
          ? std::string("None")
          : std::string(current_guide->orientation ==
                                im2d::GuideOrientation::Vertical
                            ? "Vertical"
                            : "Horizontal") +
                " Guide " + std::to_string(current_guide->id);

  if (ImGui::BeginCombo("Preview Anchor Guide", preview_label.c_str())) {
    for (const im2d::Guide &guide : state.guides) {
      const bool selected = guide.id == current_guide_id;
      const std::string guide_label =
          std::string(guide.orientation == im2d::GuideOrientation::Vertical
                          ? "Vertical"
                          : "Horizontal") +
          " Guide " + std::to_string(guide.id);
      if (ImGui::Selectable(guide_label.c_str(), selected)) {
        state.selected_guide_id = guide.id;
      }
      if (selected) {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }
}

int CountPreviewPartsByClassification(
    const im2d::ImportedArtworkSeparationPreview &preview,
    im2d::ImportedSeparationPreviewClassification classification) {
  return static_cast<int>(std::count_if(
      preview.parts.begin(), preview.parts.end(),
      [classification](const im2d::ImportedSeparationPreviewPart &part) {
        return part.classification == classification;
      }));
}

int CountPreviewPartsByClassification(
    const im2d::ImportedArtworkAutoCutPreview &preview,
    im2d::ImportedSeparationPreviewClassification classification) {
  return static_cast<int>(std::count_if(
      preview.parts.begin(), preview.parts.end(),
      [classification](const im2d::ImportedSeparationPreviewPart &part) {
        return part.classification == classification;
      }));
}

constexpr const char *kPrepareForCuttingPopupId =
    "Prepare For Cutting##Confirm";

struct AsyncPrepareForCuttingResult {
  im2d::ImportedArtwork prepared_artwork;
  im2d::ImportedArtworkOperationResult operation;
  std::vector<im2d::ImportedElementSelection> issue_elements;
  std::string error_message;
};

struct PrepareForCuttingUiState {
  bool open_requested = false;
  bool running = false;
  bool close_requested = false;
  bool auto_close_to_polyline = false;
  int artwork_id = 0;
  float weld_tolerance = 0.5f;
  im2d::ImportedArtworkPrepareMode mode =
      im2d::ImportedArtworkPrepareMode::FidelityFirst;
  std::string artwork_name;
  std::future<AsyncPrepareForCuttingResult> future;
};

PrepareForCuttingUiState &GetPrepareForCuttingUiState() {
  static PrepareForCuttingUiState state;
  return state;
}

void ResetPrepareForCuttingUiState(PrepareForCuttingUiState *ui_state) {
  if (ui_state == nullptr) {
    return;
  }
  ui_state->open_requested = false;
  ui_state->running = false;
  ui_state->close_requested = false;
  ui_state->artwork_id = 0;
  ui_state->weld_tolerance = 0.5f;
  ui_state->mode = im2d::ImportedArtworkPrepareMode::FidelityFirst;
  ui_state->artwork_name.clear();
  ui_state->future = std::future<AsyncPrepareForCuttingResult>();
}

std::future<AsyncPrepareForCuttingResult> StartPrepareForCuttingAsync(
    const im2d::ImportedArtwork &artwork, float weld_tolerance,
    im2d::ImportedArtworkPrepareMode mode, bool auto_close_to_polyline) {
  return std::async(std::launch::async, [artwork, weld_tolerance, mode,
                                         auto_close_to_polyline]() mutable {
    AsyncPrepareForCuttingResult async_result;
    try {
      im2d::CanvasState worker_state;
      worker_state.imported_artwork.push_back(artwork);
      worker_state.next_imported_artwork_id = std::max(artwork.id + 1, 1);
      worker_state.next_imported_part_id =
          std::max(artwork.part.part_id + 1, 1);

      async_result.operation = im2d::PrepareImportedArtworkForCutting(
          worker_state, artwork.id, weld_tolerance, mode,
          auto_close_to_polyline);
      async_result.issue_elements =
          worker_state.last_imported_operation_issue_elements;

      const im2d::ImportedArtwork *prepared_artwork =
          im2d::FindImportedArtwork(worker_state, artwork.id);
      if (prepared_artwork == nullptr) {
        async_result.error_message = "Prepared artwork was not available after "
                                     "background processing.";
        return async_result;
      }

      async_result.prepared_artwork = *prepared_artwork;
    } catch (const std::exception &error) {
      async_result.error_message = error.what();
    } catch (...) {
      async_result.error_message =
          "Unknown error while preparing imported artwork.";
    }
    return async_result;
  });
}

void ApplyPrepareForCuttingResult(im2d::CanvasState &state, int artwork_id,
                                  AsyncPrepareForCuttingResult async_result) {
  im2d::ImportedArtwork *artwork = im2d::FindImportedArtwork(state, artwork_id);
  if (artwork == nullptr) {
    state.last_imported_artwork_operation = {};
    state.last_imported_artwork_operation.artwork_id = artwork_id;
    state.last_imported_artwork_operation.message =
        "Prepare-for-cutting result was discarded because the artwork no "
        "longer exists.";
    state.last_imported_operation_issue_artwork_id = 0;
    state.last_imported_operation_issue_elements.clear();
    return;
  }

  if (!async_result.error_message.empty()) {
    state.last_imported_artwork_operation = {};
    state.last_imported_artwork_operation.artwork_id = artwork_id;
    state.last_imported_artwork_operation.message =
        "Prepare-for-cutting failed: " + async_result.error_message;
    state.last_imported_operation_issue_artwork_id = 0;
    state.last_imported_operation_issue_elements.clear();
    return;
  }

  *artwork = std::move(async_result.prepared_artwork);
  state.selected_imported_artwork_id = artwork_id;
  state.selected_imported_debug = {im2d::ImportedDebugSelectionKind::Artwork,
                                   artwork_id, 0};
  im2d::ClearSelectedImportedElements(state);
  state.last_imported_artwork_operation = std::move(async_result.operation);
  state.last_imported_operation_issue_artwork_id = artwork_id;
  state.last_imported_operation_issue_elements =
      std::move(async_result.issue_elements);
}

void QueuePrepareForCuttingDialog(const im2d::ImportedArtwork &artwork,
                                  float weld_tolerance,
                                  im2d::ImportedArtworkPrepareMode mode) {
  PrepareForCuttingUiState &ui_state = GetPrepareForCuttingUiState();
  if (ui_state.running) {
    return;
  }

  ui_state.open_requested = true;
  ui_state.close_requested = false;
  ui_state.artwork_id = artwork.id;
  ui_state.weld_tolerance = weld_tolerance;
  ui_state.mode = mode;
  ui_state.auto_close_to_polyline =
      GetPrepareWorkflowUiState().auto_close_before_prepare;
  ui_state.artwork_name = artwork.name;
}

void DrawPrepareForCuttingModal(im2d::CanvasState &state) {
  PrepareForCuttingUiState &ui_state = GetPrepareForCuttingUiState();
  if (ui_state.open_requested) {
    ImGui::OpenPopup(kPrepareForCuttingPopupId);
    ui_state.open_requested = false;
  }

  if (ui_state.running && ui_state.future.valid() &&
      ui_state.future.wait_for(std::chrono::milliseconds(0)) ==
          std::future_status::ready) {
    ApplyPrepareForCuttingResult(state, ui_state.artwork_id,
                                 ui_state.future.get());
    ui_state.running = false;
    ui_state.close_requested = true;
  }

  ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_Appearing);
  if (!ImGui::BeginPopupModal(kPrepareForCuttingPopupId, nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize)) {
    return;
  }

  const char *mode_label =
      ui_state.mode == im2d::ImportedArtworkPrepareMode::AggressiveCleanup
          ? "Prepare + Weld Cleanup"
          : "Prepare For Cutting";
  ImGui::TextUnformatted(mode_label);
  ImGui::Separator();
  ImGui::TextWrapped("Artwork: %s", ui_state.artwork_name.c_str());

  if (!ui_state.running) {
    ImGui::TextWrapped(
        "This operation can take a while on dense DXF artwork. Start it in "
        "the background and keep the UI responsive.");
    ImGui::Text("Weld Tolerance: %.2f", ui_state.weld_tolerance);
    ImGui::Checkbox("Auto Close To Polyline First",
                    &ui_state.auto_close_to_polyline);
    ImGui::Spacing();
    if (ImGui::Button("Start", ImVec2(120.0f, 0.0f))) {
      if (const im2d::ImportedArtwork *artwork =
              im2d::FindImportedArtwork(state, ui_state.artwork_id);
          artwork != nullptr) {
        ui_state.artwork_name = artwork->name;
        ui_state.future = StartPrepareForCuttingAsync(
            *artwork, ui_state.weld_tolerance, ui_state.mode,
            ui_state.auto_close_to_polyline);
        ui_state.running = true;
      } else {
        state.last_imported_artwork_operation = {};
        state.last_imported_artwork_operation.message =
            "Prepare-for-cutting could not start because the selected "
            "artwork was not found.";
        ui_state.close_requested = true;
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) {
      ui_state.close_requested = true;
    }
  } else {
    ImGui::TextWrapped("Preparing imported artwork in the background...");
    ImGui::Text("Auto Close To Polyline: %s",
                ui_state.auto_close_to_polyline ? "On" : "Off");
    const double phase = ImGui::GetTime() * 0.35;
    const float progress = static_cast<float>(phase - std::floor(phase));
    ImGui::ProgressBar(progress, ImVec2(320.0f, 0.0f), "Working...");
    ImGui::TextUnformatted("The popup will close automatically when done.");
  }

  if (ui_state.close_requested) {
    ImGui::CloseCurrentPopup();
    ResetPrepareForCuttingUiState(&ui_state);
  }
  ImGui::EndPopup();
}

template <size_t N>
void CopyTextToBuffer(const std::string &text, std::array<char, N> *buffer) {
  if (buffer == nullptr || N == 0) {
    return;
  }
  std::snprintf(buffer->data(), buffer->size(), "%s", text.c_str());
}

template <size_t N> std::string BufferText(const std::array<char, N> &buffer) {
  return std::string(buffer.data());
}

std::string SanitizeExportStem(const std::string &text) {
  std::string stem;
  stem.reserve(text.size());
  for (const char character : text) {
    if ((character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9')) {
      stem.push_back(static_cast<char>(std::tolower(character)));
      continue;
    }
    if (!stem.empty() && stem.back() != '-') {
      stem.push_back('-');
    }
  }
  while (!stem.empty() && stem.back() == '-') {
    stem.pop_back();
  }
  return stem.empty() ? "artwork" : stem;
}

std::filesystem::path
DefaultSelectionExportPath(const im2d::ImportedArtwork &artwork) {
  return std::filesystem::path("build") / "exports" /
         (SanitizeExportStem(artwork.name) + "-selection.svg");
}

std::filesystem::path DefaultExportAreaPath(const im2d::CanvasState &state) {
  const int export_area_id =
      state.export_areas.empty() ? 0 : state.export_areas.front().id;
  return std::filesystem::path("build") / "exports" /
         ("export-area-" + std::to_string(export_area_id) + ".svg");
}

void SyncSvgExportUiState(SvgExportUiState *ui_state,
                          const im2d::CanvasState &state,
                          const im2d::ImportedArtwork &artwork) {
  if (ui_state == nullptr) {
    return;
  }

  const int export_area_id =
      state.export_areas.empty() ? 0 : state.export_areas.front().id;
  if (ui_state->configured_artwork_id == artwork.id &&
      ui_state->configured_export_area_id == export_area_id &&
      ui_state->output_directory[0] != '\0' &&
      ui_state->selection_filename[0] != '\0' &&
      ui_state->export_area_filename[0] != '\0') {
    return;
  }

  const std::filesystem::path selection_path =
      DefaultSelectionExportPath(artwork);
  const std::filesystem::path export_area_path = DefaultExportAreaPath(state);
  const std::filesystem::path output_directory =
      selection_path.has_parent_path() ? selection_path.parent_path()
                                       : std::filesystem::path(".");

  CopyTextToBuffer(output_directory.string(), &ui_state->output_directory);
  CopyTextToBuffer(selection_path.filename().string(),
                   &ui_state->selection_filename);
  CopyTextToBuffer(export_area_path.filename().string(),
                   &ui_state->export_area_filename);
  ui_state->configured_artwork_id = artwork.id;
  ui_state->configured_export_area_id = export_area_id;
}

std::filesystem::path BuildExportPath(const std::array<char, 256> &directory,
                                      const std::array<char, 128> &filename,
                                      const std::filesystem::path &fallback) {
  const std::string directory_text = BufferText(directory);
  const std::string filename_text = BufferText(filename);
  if (directory_text.empty() || filename_text.empty()) {
    return fallback;
  }
  return std::filesystem::path(directory_text) / filename_text;
}

std::string SvgExportSummary(const im2d::exporter::SvgExportResult &result) {
  if (result.message.empty()) {
    return {};
  }

  std::string summary = result.message;
  if (!result.success) {
    return summary;
  }

  summary +=
      " Bounds: " + std::to_string(result.bounds_max.x - result.bounds_min.x) +
      " x " + std::to_string(result.bounds_max.y - result.bounds_min.y);
  summary += ", lines=" + std::to_string(result.line_segment_count);
  summary += ", cubics=" + std::to_string(result.cubic_segment_count);
  if (result.placeholder_text_count > 0) {
    summary +=
        ", placeholder-text=" + std::to_string(result.placeholder_text_count);
  }
  if (result.substituted_font_text_count > 0) {
    summary += ", substituted-font=" +
               std::to_string(result.substituted_font_text_count);
  }
  if (result.open_geometry_item_count > 0) {
    summary +=
        ", open-items=" + std::to_string(result.open_geometry_item_count);
  }
  summary += ", bytes=" + std::to_string(result.svg.size());
  if (!result.output_path.empty()) {
    summary += ", file=" + result.output_path;
  }
  return summary;
}

void DrawSvgExportWarnings(const im2d::exporter::SvgExportResult &result) {
  if (result.warnings.empty()) {
    return;
  }

  ImGui::TextUnformatted("Export Warnings");
  for (const std::string &warning : result.warnings) {
    ImGui::BulletText("%s", warning.c_str());
  }
}

void SelectImportedArtwork(im2d::CanvasState &state, int artwork_id) {
  state.selected_imported_artwork_id = artwork_id;
  state.selected_imported_debug = {im2d::ImportedDebugSelectionKind::Artwork,
                                   artwork_id, 0};
  im2d::ClearSelectedImportedElements(state);
}

void SelectImportedGroup(im2d::CanvasState &state, int artwork_id,
                         int group_id) {
  state.selected_imported_artwork_id = artwork_id;
  state.selected_imported_debug = {im2d::ImportedDebugSelectionKind::Group,
                                   artwork_id, group_id};
  im2d::ClearSelectedImportedElements(state);
}

void SelectImportedPath(im2d::CanvasState &state, int artwork_id, int path_id) {
  state.selected_imported_artwork_id = artwork_id;
  state.selected_imported_debug = {im2d::ImportedDebugSelectionKind::Path,
                                   artwork_id, path_id};
  state.selected_imported_elements = {
      {im2d::ImportedElementKind::Path, path_id}};
}

void SelectImportedDxfText(im2d::CanvasState &state, int artwork_id,
                           int text_id) {
  state.selected_imported_artwork_id = artwork_id;
  state.selected_imported_debug = {im2d::ImportedDebugSelectionKind::DxfText,
                                   artwork_id, text_id};
  state.selected_imported_elements = {
      {im2d::ImportedElementKind::DxfText, text_id}};
}

bool IsImportedElementSelectedInVector(
    const std::vector<im2d::ImportedElementSelection> &selections,
    im2d::ImportedElementKind kind, int item_id) {
  return std::any_of(
      selections.begin(), selections.end(),
      [kind, item_id](const im2d::ImportedElementSelection &selection) {
        return selection.kind == kind && selection.item_id == item_id;
      });
}

bool IsImportedMultiSelectModifierDown() {
  const ImGuiIO &io = ImGui::GetIO();
  return io.KeyCtrl || io.KeyShift;
}

void ToggleImportedElementSelection(
    im2d::CanvasState &state, int artwork_id, im2d::ImportedElementKind kind,
    int item_id, im2d::ImportedDebugSelectionKind debug_selection_kind) {
  if (state.selected_imported_artwork_id != artwork_id) {
    im2d::ClearSelectedImportedElements(state);
  }

  state.selected_imported_artwork_id = artwork_id;
  state.selected_imported_debug = {debug_selection_kind, artwork_id, item_id};

  auto it = std::find_if(
      state.selected_imported_elements.begin(),
      state.selected_imported_elements.end(),
      [kind, item_id](const im2d::ImportedElementSelection &selection) {
        return selection.kind == kind && selection.item_id == item_id;
      });
  if (it != state.selected_imported_elements.end()) {
    state.selected_imported_elements.erase(it);
    return;
  }

  state.selected_imported_elements.push_back({kind, item_id});
}

void HandleImportedPathTreeSelection(im2d::CanvasState &state, int artwork_id,
                                     int path_id) {
  if (!IsImportedMultiSelectModifierDown()) {
    SelectImportedPath(state, artwork_id, path_id);
    return;
  }
  ToggleImportedElementSelection(state, artwork_id,
                                 im2d::ImportedElementKind::Path, path_id,
                                 im2d::ImportedDebugSelectionKind::Path);
}

void HandleImportedDxfTextTreeSelection(im2d::CanvasState &state,
                                        int artwork_id, int text_id) {
  if (!IsImportedMultiSelectModifierDown()) {
    SelectImportedDxfText(state, artwork_id, text_id);
    return;
  }
  ToggleImportedElementSelection(state, artwork_id,
                                 im2d::ImportedElementKind::DxfText, text_id,
                                 im2d::ImportedDebugSelectionKind::DxfText);
}

int CountGroupableImportedRootItems(const im2d::ImportedArtwork &artwork) {
  const im2d::ImportedGroup *root_group =
      im2d::FindImportedGroup(artwork, artwork.root_group_id);
  if (root_group == nullptr) {
    return static_cast<int>(artwork.paths.size() + artwork.dxf_text.size());
  }

  return static_cast<int>(root_group->child_group_ids.size() +
                          root_group->path_ids.size() +
                          root_group->dxf_text_ids.size());
}

void SelectLastOperationIssueElements(im2d::CanvasState &state,
                                      int artwork_id) {
  if (state.last_imported_operation_issue_artwork_id != artwork_id) {
    return;
  }

  state.selected_imported_artwork_id = artwork_id;
  state.selected_imported_debug = {im2d::ImportedDebugSelectionKind::Artwork,
                                   artwork_id, 0};
  state.selected_imported_elements =
      state.last_imported_operation_issue_elements;
}

bool IsSelectedImportedDebugItem(const im2d::CanvasState &state, int artwork_id,
                                 im2d::ImportedDebugSelectionKind kind,
                                 int item_id) {
  return state.selected_imported_debug.artwork_id == artwork_id &&
         state.selected_imported_debug.kind == kind &&
         state.selected_imported_debug.item_id == item_id;
}

bool IsSkippedImportedElement(const im2d::CanvasState &state, int artwork_id,
                              im2d::ImportedElementKind kind, int item_id) {
  if (state.last_imported_operation_issue_artwork_id != artwork_id) {
    return false;
  }

  return std::any_of(
      state.last_imported_operation_issue_elements.begin(),
      state.last_imported_operation_issue_elements.end(),
      [kind, item_id](const im2d::ImportedElementSelection &selection) {
        return selection.kind == kind && selection.item_id == item_id;
      });
}

const char *ExtractActionLabel(const im2d::CanvasState &state,
                               const im2d::ImportedArtwork &artwork) {
  if (!state.selected_imported_elements.empty() &&
      state.selected_imported_artwork_id == artwork.id) {
    return "Extract Selection";
  }

  switch (state.selected_imported_debug.kind) {
  case im2d::ImportedDebugSelectionKind::Group:
    return "Extract Selected Group";
  case im2d::ImportedDebugSelectionKind::Path:
    return "Extract Selected Path";
  case im2d::ImportedDebugSelectionKind::DxfText:
    return "Extract Selected DXF Text";
  case im2d::ImportedDebugSelectionKind::Artwork:
  case im2d::ImportedDebugSelectionKind::None:
    return "Extract Selection";
  }
  return "Extract Selection";
}

const char *GroupActionLabel(const im2d::CanvasState &state,
                             const im2d::ImportedArtwork &artwork) {
  if (im2d::HasGroupableImportedElementSelection(state, artwork)) {
    return "Group Selection";
  }
  if (im2d::HasGroupableImportedRootSelection(state, artwork)) {
    return "Group Artwork Contents";
  }
  return "Group Selection";
}

enum class ImportedDebugTreeActionKind {
  None,
  Extract,
  GroupSelection,
  GroupArtworkContents,
  Ungroup,
};

struct ImportedDebugTreeAction {
  ImportedDebugTreeActionKind kind = ImportedDebugTreeActionKind::None;
  im2d::ImportedDebugSelectionKind selection_kind =
      im2d::ImportedDebugSelectionKind::None;
  int artwork_id = 0;
  int item_id = 0;
};

void QueueImportedDebugTreeAction(
    ImportedDebugTreeAction *action, ImportedDebugTreeActionKind kind,
    im2d::ImportedDebugSelectionKind selection_kind, int artwork_id,
    int item_id) {
  if (action == nullptr) {
    return;
  }
  action->kind = kind;
  action->selection_kind = selection_kind;
  action->artwork_id = artwork_id;
  action->item_id = item_id;
}

bool ApplyImportedDebugTreeAction(im2d::CanvasState &state,
                                  const im2d::ImportedArtwork &artwork,
                                  const ImportedDebugTreeAction &action) {
  if (action.kind == ImportedDebugTreeActionKind::None ||
      action.artwork_id != artwork.id) {
    return false;
  }

  switch (action.selection_kind) {
  case im2d::ImportedDebugSelectionKind::Artwork:
    if (action.kind != ImportedDebugTreeActionKind::GroupSelection) {
      SelectImportedArtwork(state, action.artwork_id);
    }
    break;
  case im2d::ImportedDebugSelectionKind::Group:
    SelectImportedGroup(state, action.artwork_id, action.item_id);
    break;
  case im2d::ImportedDebugSelectionKind::Path:
    SelectImportedPath(state, action.artwork_id, action.item_id);
    break;
  case im2d::ImportedDebugSelectionKind::DxfText:
    SelectImportedDxfText(state, action.artwork_id, action.item_id);
    break;
  case im2d::ImportedDebugSelectionKind::None:
    break;
  }

  switch (action.kind) {
  case ImportedDebugTreeActionKind::Extract:
    im2d::operations::ExtractSelectedImportedElements(state, artwork.id);
    return true;
  case ImportedDebugTreeActionKind::GroupSelection:
    im2d::operations::GroupSelectedImportedElements(state, artwork.id);
    return true;
  case ImportedDebugTreeActionKind::GroupArtworkContents:
    im2d::operations::GroupImportedArtworkRootContents(state, artwork.id);
    return true;
  case ImportedDebugTreeActionKind::Ungroup:
    im2d::operations::UngroupSelectedImportedGroup(state, artwork.id);
    return true;
  case ImportedDebugTreeActionKind::None:
    return false;
  }
  return false;
}

bool DrawImportedGroupingControls(im2d::CanvasState &state,
                                  im2d::ImportedArtwork &artwork) {
  const bool can_group_selection =
      im2d::HasGroupableImportedElementSelection(state, artwork);
  const bool can_group_root =
      im2d::HasGroupableImportedRootSelection(state, artwork);
  const bool can_group = can_group_selection || can_group_root;
  const bool can_ungroup =
      im2d::HasUngroupableImportedDebugSelection(state, artwork);

  ImGui::Separator();
  ImGui::TextUnformatted("Grouping");
  if (!can_group) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button(GroupActionLabel(state, artwork))) {
    if (can_group_selection) {
      im2d::operations::GroupSelectedImportedElements(state, artwork.id);
    } else {
      im2d::operations::GroupImportedArtworkRootContents(state, artwork.id);
    }
    return true;
  }
  if (!can_group) {
    ImGui::EndDisabled();
  }
  ImGui::SameLine();
  if (!can_ungroup) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button("Ungroup Selected Group")) {
    im2d::operations::UngroupSelectedImportedGroup(state, artwork.id);
    return true;
  }
  if (!can_ungroup) {
    ImGui::EndDisabled();
  }
  if (!can_group && !can_ungroup) {
    ImGui::TextUnformatted(
        "Select two or more imported elements to group them, select the "
        "artwork root to group its contents, or select a non-root group to "
        "ungroup it.");
  }
  return false;
}

bool DrawDebugTreeExtractionControls(im2d::CanvasState &state,
                                     im2d::ImportedArtwork &artwork) {
  const bool has_selected_elements =
      !state.selected_imported_elements.empty() &&
      state.selected_imported_artwork_id == artwork.id;
  const bool has_debug_target =
      im2d::HasExtractableImportedDebugSelection(state, artwork);
  const bool can_extract = has_selected_elements || has_debug_target;

  ImGui::Separator();
  ImGui::TextUnformatted("Extraction");
  if (!can_extract) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button(ExtractActionLabel(state, artwork))) {
    im2d::operations::ExtractSelectedImportedElements(state, artwork.id);
    if (!can_extract) {
      ImGui::EndDisabled();
    }
    return true;
  }
  if (!can_extract) {
    ImGui::EndDisabled();
    ImGui::TextUnformatted("Select a group, path, or DXF text in the debug "
                           "tree, or use a marquee selection.");
  }
  return false;
}

std::string ImportedPathSummary(const im2d::ImportedPath &path) {
  std::string summary = path.label.empty() ? "Path" : path.label;
  summary += " (" + std::to_string(path.segments.size()) + " seg";
  if (path.segments.size() != 1) {
    summary += 's';
  }
  summary += path.closed ? ", closed" : ", open";
  if (im2d::HasImportedPathFlag(path.flags,
                                im2d::ImportedPathFlagTextPlaceholder)) {
    summary += ", text";
  }
  if (im2d::HasImportedPathFlag(path.flags, im2d::ImportedPathFlagFilledText)) {
    summary += ", filled";
  }
  if (im2d::HasImportedPathFlag(path.flags,
                                im2d::ImportedPathFlagHoleContour)) {
    summary += ", hole";
  }
  summary += ')';
  if (path.issue_flags != im2d::ImportedElementIssueFlagNone) {
    summary += " [issue]";
  }
  return summary;
}

std::string ImportedDxfTextSummary(const im2d::ImportedDxfText &text) {
  std::string summary = text.label.empty() ? "DXF Text" : text.label;
  if (text.placeholder_only) {
    summary += " (placeholder";
  } else {
    summary += " (glyphs=" + std::to_string(text.glyphs.size());
  }
  if (!text.source_text.empty()) {
    summary += ", \"" + text.source_text + "\"";
  }
  summary += ')';
  if (text.issue_flags != im2d::ImportedElementIssueFlagNone) {
    summary += " [issue]";
  }
  return summary;
}

std::string FormatImportedElementIssues(uint32_t issue_flags) {
  if (issue_flags == im2d::ImportedElementIssueFlagNone) {
    return "None";
  }

  std::string summary;
  const auto append_issue = [&](const char *label) {
    if (!summary.empty()) {
      summary += ", ";
    }
    summary += label;
  };

  if (im2d::HasImportedElementIssueFlag(
          issue_flags, im2d::ImportedElementIssueFlagOpenGeometry)) {
    append_issue("Open geometry");
  }
  if (im2d::HasImportedElementIssueFlag(
          issue_flags, im2d::ImportedElementIssueFlagPlaceholderText)) {
    append_issue("Placeholder text");
  }
  if (im2d::HasImportedElementIssueFlag(
          issue_flags, im2d::ImportedElementIssueFlagOrphanHole)) {
    append_issue("Orphan hole");
  }
  if (im2d::HasImportedElementIssueFlag(
          issue_flags, im2d::ImportedElementIssueFlagAmbiguousCleanup)) {
    append_issue("Ambiguous cleanup");
  }
  return summary;
}

std::string FormatContourReference(const im2d::ImportedContourReference &ref) {
  std::string summary =
      ref.kind == im2d::ImportedElementKind::Path ? "Path " : "DXF Text ";
  summary += std::to_string(ref.item_id);
  summary += " contour ";
  summary += std::to_string(ref.contour_index);
  return summary;
}

bool MatchesFilter(bool flagged, bool skipped,
                   ImportedInspectorFilterMode mode) {
  switch (mode) {
  case ImportedInspectorFilterMode::Hierarchy:
    return true;
  case ImportedInspectorFilterMode::FlaggedOnly:
    return flagged;
  case ImportedInspectorFilterMode::SkippedOnly:
    return skipped;
  case ImportedInspectorFilterMode::FlaggedOrSkipped:
    return flagged || skipped;
  }
  return true;
}

std::string DecorateImportedItemSummary(const std::string &summary,
                                        bool skipped) {
  if (!skipped) {
    return summary;
  }
  return summary + " [skipped]";
}

void DrawSelectedImportedOwnershipDetails(const im2d::ImportedArtwork &artwork,
                                          im2d::ImportedElementKind kind,
                                          int item_id) {
  std::vector<std::string> owned_holes;
  std::vector<std::string> attached_outers;
  std::vector<std::string> orphan_holes;

  for (const im2d::ImportedHoleOwnership &ownership :
       artwork.part.hole_attachments) {
    if (ownership.outer.kind == kind && ownership.outer.item_id == item_id) {
      owned_holes.push_back(FormatContourReference(ownership.hole));
    }
    if (ownership.hole.kind == kind && ownership.hole.item_id == item_id) {
      attached_outers.push_back(FormatContourReference(ownership.outer));
    }
  }
  for (const im2d::ImportedContourReference &hole_ref :
       artwork.part.orphan_holes) {
    if (hole_ref.kind == kind && hole_ref.item_id == item_id) {
      orphan_holes.push_back(FormatContourReference(hole_ref));
    }
  }

  if (!owned_holes.empty()) {
    std::string summary = "Owned Holes: ";
    for (size_t index = 0; index < owned_holes.size(); ++index) {
      if (index != 0) {
        summary += ", ";
      }
      summary += owned_holes[index];
    }
    ImGui::TextWrapped("%s", summary.c_str());
  }
  if (!attached_outers.empty()) {
    std::string summary = "Attached To: ";
    for (size_t index = 0; index < attached_outers.size(); ++index) {
      if (index != 0) {
        summary += ", ";
      }
      summary += attached_outers[index];
    }
    ImGui::TextWrapped("%s", summary.c_str());
  }
  if (!orphan_holes.empty()) {
    std::string summary = "Orphan Holes: ";
    for (size_t index = 0; index < orphan_holes.size(); ++index) {
      if (index != 0) {
        summary += ", ";
      }
      summary += orphan_holes[index];
    }
    ImGui::TextWrapped("%s", summary.c_str());
  }
}

std::string FormatImportedSourceReferences(
    const std::vector<im2d::ImportedSourceReference> &references) {
  if (references.empty()) {
    return "None";
  }

  std::string summary;
  for (size_t index = 0; index < references.size(); ++index) {
    if (index != 0) {
      summary += ", ";
    }
    summary += "A" + std::to_string(references[index].source_artwork_id);
    summary +=
        references[index].kind == im2d::ImportedElementKind::Path ? ":P" : ":T";
    summary += std::to_string(references[index].item_id);
  }
  return summary;
}

void DrawSelectedImportedItemDetails(const im2d::ImportedArtwork &artwork,
                                     const im2d::CanvasState &state) {
  switch (state.selected_imported_debug.kind) {
  case im2d::ImportedDebugSelectionKind::Path: {
    const im2d::ImportedPath *path =
        im2d::FindImportedPath(artwork, state.selected_imported_debug.item_id);
    if (path == nullptr) {
      return;
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Selected Path");
    ImGui::TextWrapped("%s", ImportedPathSummary(*path).c_str());
    ImGui::TextWrapped("Issues: %s",
                       FormatImportedElementIssues(path->issue_flags).c_str());
    ImGui::Text("Skipped By Last Operation: %s",
                IsSkippedImportedElement(state, artwork.id,
                                         im2d::ImportedElementKind::Path,
                                         path->id)
                    ? "Yes"
                    : "No");
    DrawSelectedImportedOwnershipDetails(
        artwork, im2d::ImportedElementKind::Path, path->id);
    ImGui::TextWrapped(
        "Provenance: %s",
        FormatImportedSourceReferences(path->provenance).c_str());
    return;
  }
  case im2d::ImportedDebugSelectionKind::DxfText: {
    const im2d::ImportedDxfText *text = im2d::FindImportedDxfText(
        artwork, state.selected_imported_debug.item_id);
    if (text == nullptr) {
      return;
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Selected DXF Text");
    ImGui::TextWrapped("%s", ImportedDxfTextSummary(*text).c_str());
    ImGui::TextWrapped("Issues: %s",
                       FormatImportedElementIssues(text->issue_flags).c_str());
    ImGui::Text("Skipped By Last Operation: %s",
                IsSkippedImportedElement(state, artwork.id,
                                         im2d::ImportedElementKind::DxfText,
                                         text->id)
                    ? "Yes"
                    : "No");
    DrawSelectedImportedOwnershipDetails(
        artwork, im2d::ImportedElementKind::DxfText, text->id);
    ImGui::TextWrapped(
        "Provenance: %s",
        FormatImportedSourceReferences(text->provenance).c_str());
    return;
  }
  default:
    break;
  }
}

void DrawImportedPathNode(im2d::CanvasState &state,
                          const im2d::ImportedArtwork &artwork,
                          const im2d::ImportedPath &path,
                          ImportedDebugTreeAction *pending_action) {
  const bool skipped = IsSkippedImportedElement(
      state, artwork.id, im2d::ImportedElementKind::Path, path.id);
  const ImGuiTreeNodeFlags flags =
      ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
      ImGuiTreeNodeFlags_SpanAvailWidth |
      ((IsSelectedImportedDebugItem(state, artwork.id,
                                    im2d::ImportedDebugSelectionKind::Path,
                                    path.id) ||
        IsImportedElementSelectedInVector(state.selected_imported_elements,
                                          im2d::ImportedElementKind::Path,
                                          path.id))
           ? ImGuiTreeNodeFlags_Selected
           : 0);
  const std::string label =
      DecorateImportedItemSummary(ImportedPathSummary(path), skipped);
  ImGui::TreeNodeEx(reinterpret_cast<void *>(static_cast<intptr_t>(path.id)),
                    flags, "%s", label.c_str());
  if (ImGui::IsItemClicked()) {
    HandleImportedPathTreeSelection(state, artwork.id, path.id);
  }
  if (ImGui::BeginPopupContextItem()) {
    if (ImGui::MenuItem("Select Path")) {
      SelectImportedPath(state, artwork.id, path.id);
    }
    if (ImGui::MenuItem("Extract Path")) {
      QueueImportedDebugTreeAction(
          pending_action, ImportedDebugTreeActionKind::Extract,
          im2d::ImportedDebugSelectionKind::Path, artwork.id, path.id);
    }
    ImGui::EndPopup();
  }
}

void DrawImportedDxfTextNode(im2d::CanvasState &state,
                             const im2d::ImportedArtwork &artwork,
                             const im2d::ImportedDxfText &text,
                             ImportedDebugTreeAction *pending_action) {
  const bool skipped = IsSkippedImportedElement(
      state, artwork.id, im2d::ImportedElementKind::DxfText, text.id);
  const ImGuiTreeNodeFlags flags =
      ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
      ImGuiTreeNodeFlags_SpanAvailWidth |
      ((IsSelectedImportedDebugItem(state, artwork.id,
                                    im2d::ImportedDebugSelectionKind::DxfText,
                                    text.id) ||
        IsImportedElementSelectedInVector(state.selected_imported_elements,
                                          im2d::ImportedElementKind::DxfText,
                                          text.id))
           ? ImGuiTreeNodeFlags_Selected
           : 0);
  const std::string label =
      DecorateImportedItemSummary(ImportedDxfTextSummary(text), skipped);
  ImGui::TreeNodeEx(
      reinterpret_cast<void *>(static_cast<intptr_t>(1000000 + text.id)), flags,
      "%s", label.c_str());
  if (ImGui::IsItemClicked()) {
    HandleImportedDxfTextTreeSelection(state, artwork.id, text.id);
  }
  if (ImGui::BeginPopupContextItem()) {
    if (ImGui::MenuItem("Select DXF Text")) {
      SelectImportedDxfText(state, artwork.id, text.id);
    }
    if (ImGui::MenuItem("Extract DXF Text")) {
      QueueImportedDebugTreeAction(
          pending_action, ImportedDebugTreeActionKind::Extract,
          im2d::ImportedDebugSelectionKind::DxfText, artwork.id, text.id);
    }
    ImGui::EndPopup();
  }
}

void DrawImportedGroupNode(im2d::CanvasState &state,
                           const im2d::ImportedArtwork &artwork,
                           const im2d::ImportedGroup &group,
                           ImportedDebugTreeAction *pending_action) {
  std::string label = group.label.empty() ? "Group" : group.label;
  label += " (" + std::to_string(group.path_ids.size()) + " paths, " +
           std::to_string(group.child_group_ids.size()) + " groups)";
  const ImGuiTreeNodeFlags flags =
      ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
      (IsSelectedImportedDebugItem(
           state, artwork.id, im2d::ImportedDebugSelectionKind::Group, group.id)
           ? ImGuiTreeNodeFlags_Selected
           : 0);
  const bool open = ImGui::TreeNodeEx(
      reinterpret_cast<void *>(static_cast<intptr_t>(group.id)), flags, "%s",
      label.c_str());
  if (ImGui::IsItemClicked()) {
    SelectImportedGroup(state, artwork.id, group.id);
  }
  if (ImGui::BeginPopupContextItem()) {
    if (ImGui::MenuItem("Select Group")) {
      SelectImportedGroup(state, artwork.id, group.id);
    }
    if (ImGui::MenuItem("Extract Group")) {
      QueueImportedDebugTreeAction(
          pending_action, ImportedDebugTreeActionKind::Extract,
          im2d::ImportedDebugSelectionKind::Group, artwork.id, group.id);
    }
    if (group.id != artwork.root_group_id && ImGui::MenuItem("Ungroup Group")) {
      QueueImportedDebugTreeAction(
          pending_action, ImportedDebugTreeActionKind::Ungroup,
          im2d::ImportedDebugSelectionKind::Group, artwork.id, group.id);
    }
    ImGui::EndPopup();
  }
  if (!open) {
    return;
  }

  for (const int child_group_id : group.child_group_ids) {
    const im2d::ImportedGroup *child_group =
        im2d::FindImportedGroup(artwork, child_group_id);
    if (child_group != nullptr) {
      DrawImportedGroupNode(state, artwork, *child_group, pending_action);
    }
  }

  for (const int path_id : group.path_ids) {
    const im2d::ImportedPath *path = im2d::FindImportedPath(artwork, path_id);
    if (path != nullptr) {
      DrawImportedPathNode(state, artwork, *path, pending_action);
    }
  }

  for (const int text_id : group.dxf_text_ids) {
    const im2d::ImportedDxfText *text =
        im2d::FindImportedDxfText(artwork, text_id);
    if (text != nullptr) {
      DrawImportedDxfTextNode(state, artwork, *text, pending_action);
    }
  }

  ImGui::TreePop();
}

bool DrawImportedDebugTree(im2d::CanvasState &state,
                           const im2d::ImportedArtwork &artwork) {
  ImGui::Separator();
  ImGui::TextUnformatted("Debug Tree");
  ImportedDebugTreeAction pending_action;

  const ImGuiTreeNodeFlags root_flags =
      ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow |
      ImGuiTreeNodeFlags_SpanAvailWidth |
      (IsSelectedImportedDebugItem(state, artwork.id,
                                   im2d::ImportedDebugSelectionKind::Artwork, 0)
           ? ImGuiTreeNodeFlags_Selected
           : 0);
  const bool root_open = ImGui::TreeNodeEx(
      reinterpret_cast<void *>(static_cast<intptr_t>(artwork.id * 100000)),
      root_flags, "%s", artwork.name.c_str());
  if (ImGui::IsItemClicked()) {
    SelectImportedArtwork(state, artwork.id);
  }
  if (ImGui::BeginPopupContextItem()) {
    if (ImGui::MenuItem("Select Artwork")) {
      SelectImportedArtwork(state, artwork.id);
    }
    if (im2d::HasGroupableImportedElementSelection(state, artwork) &&
        ImGui::MenuItem("Group Selection")) {
      QueueImportedDebugTreeAction(
          &pending_action, ImportedDebugTreeActionKind::GroupSelection,
          im2d::ImportedDebugSelectionKind::None, artwork.id, 0);
    }
    if (CountGroupableImportedRootItems(artwork) >= 2 &&
        ImGui::MenuItem("Group Artwork Contents")) {
      QueueImportedDebugTreeAction(
          &pending_action, ImportedDebugTreeActionKind::GroupArtworkContents,
          im2d::ImportedDebugSelectionKind::Artwork, artwork.id, 0);
    }
    ImGui::EndPopup();
  }
  if (!root_open) {
    return ApplyImportedDebugTreeAction(state, artwork, pending_action);
  }

  const im2d::ImportedGroup *root_group =
      im2d::FindImportedGroup(artwork, artwork.root_group_id);
  if (root_group != nullptr) {
    for (const int child_group_id : root_group->child_group_ids) {
      const im2d::ImportedGroup *child_group =
          im2d::FindImportedGroup(artwork, child_group_id);
      if (child_group != nullptr) {
        DrawImportedGroupNode(state, artwork, *child_group, &pending_action);
      }
    }
    for (const int path_id : root_group->path_ids) {
      const im2d::ImportedPath *path = im2d::FindImportedPath(artwork, path_id);
      if (path != nullptr) {
        DrawImportedPathNode(state, artwork, *path, &pending_action);
      }
    }
    for (const int text_id : root_group->dxf_text_ids) {
      const im2d::ImportedDxfText *text =
          im2d::FindImportedDxfText(artwork, text_id);
      if (text != nullptr) {
        DrawImportedDxfTextNode(state, artwork, *text, &pending_action);
      }
    }
  } else {
    for (const im2d::ImportedPath &path : artwork.paths) {
      DrawImportedPathNode(state, artwork, path, &pending_action);
    }
    for (const im2d::ImportedDxfText &text : artwork.dxf_text) {
      DrawImportedDxfTextNode(state, artwork, text, &pending_action);
    }
  }

  ImGui::TreePop();
  return ApplyImportedDebugTreeAction(state, artwork, pending_action);
}

void DrawFilteredImportedItems(im2d::CanvasState &state,
                               const im2d::ImportedArtwork &artwork,
                               ImportedInspectorFilterMode mode) {
  ImGui::Separator();
  ImGui::TextUnformatted("Filtered Items");

  bool drew_any = false;
  for (const im2d::ImportedPath &path : artwork.paths) {
    const bool flagged = path.issue_flags != im2d::ImportedElementIssueFlagNone;
    const bool skipped = IsSkippedImportedElement(
        state, artwork.id, im2d::ImportedElementKind::Path, path.id);
    if (!MatchesFilter(flagged, skipped, mode)) {
      continue;
    }
    DrawImportedPathNode(state, artwork, path, nullptr);
    drew_any = true;
  }

  for (const im2d::ImportedDxfText &text : artwork.dxf_text) {
    const bool flagged = text.issue_flags != im2d::ImportedElementIssueFlagNone;
    const bool skipped = IsSkippedImportedElement(
        state, artwork.id, im2d::ImportedElementKind::DxfText, text.id);
    if (!MatchesFilter(flagged, skipped, mode)) {
      continue;
    }
    DrawImportedDxfTextNode(state, artwork, text, nullptr);
    drew_any = true;
  }

  if (!drew_any) {
    ImGui::TextUnformatted("No imported items match the current filter.");
  }
}

} // namespace

void DrawImportedArtworkTransientUi(im2d::CanvasState &state) {
  DrawPrepareForCuttingModal(state);
}

namespace {

im2d::ImportedArtwork *
ResolveSelectedImportedArtworkForUi(im2d::CanvasState &state) {
  im2d::ImportedArtwork *artwork =
      im2d::FindImportedArtwork(state, state.selected_imported_artwork_id);
  if (artwork != nullptr) {
    return artwork;
  }
  if (state.selected_imported_artwork_id != 0) {
    state.selected_imported_artwork_id = 0;
    im2d::ClearImportedDebugSelection(state);
    im2d::ClearSelectedImportedElements(state);
  }
  return nullptr;
}

void DrawImportedArtworkCanvasOverviewContents(im2d::CanvasState &state,
                                               im2d::ImportedArtwork &artwork) {
  ImGui::Text("ID: %d", artwork.id);
  ImGui::TextUnformatted(artwork.name.c_str());
  ImGui::Separator();
  ImGui::Text("Format: %s", artwork.source_format.c_str());
  ImGui::TextWrapped("Source: %s", artwork.source_path.c_str());
  ImGui::Checkbox("Visible", &artwork.visible);
  if (artwork.source_format == "DXF") {
    ImGui::Checkbox("Show Imported DXF Text", &state.show_imported_dxf_text);
  }

  float position[2] = {artwork.origin.x, artwork.origin.y};
  if (ImGui::InputFloat2("Position", position, "%.2f")) {
    artwork.origin = ImVec2(position[0], position[1]);
  }
  ImGui::SameLine();
  if (ImGui::Button("Reset Position")) {
    artwork.origin = ImVec2(0.0f, 0.0f);
  }

  float scale[2] = {artwork.scale.x, artwork.scale.y};
  if (ImGui::InputFloat2("Scale", scale, "%.3f")) {
    artwork.scale =
        ImVec2(std::max(scale[0], 0.01f), std::max(scale[1], 0.01f));
  }
  ImGui::SameLine();
  if (ImGui::Button("Reset Scale")) {
    artwork.scale = ImVec2(1.0f, 1.0f);
  }

  bool lock_scale_ratio =
      im2d::operations::IsImportedArtworkScaleRatioLocked(artwork);
  if (ImGui::Checkbox("Lock Scale Ratio", &lock_scale_ratio)) {
    im2d::operations::SetImportedArtworkScaleRatioLocked(artwork,
                                                         lock_scale_ratio);
  }

  float scale_x = artwork.scale.x;
  if (ImGui::InputFloat("Scale X", &scale_x, 0.0f, 0.0f, "%.3f")) {
    im2d::operations::UpdateImportedArtworkScaleAxis(artwork, 0, scale_x);
  }

  float scale_y = artwork.scale.y;
  if (ImGui::InputFloat("Scale Y", &scale_y, 0.0f, 0.0f, "%.3f")) {
    im2d::operations::UpdateImportedArtworkScaleAxis(artwork, 1, scale_y);
  }

  ImGui::DragFloat2("Adjust Position", &artwork.origin.x, 1.0f, 0.0f, 0.0f,
                    "%.2f");
  float drag_scale_x = artwork.scale.x;
  if (ImGui::DragFloat("Adjust Scale X", &drag_scale_x, 0.01f, 0.01f, 100.0f,
                       "%.3f")) {
    im2d::operations::UpdateImportedArtworkScaleAxis(artwork, 0, drag_scale_x);
  }

  float drag_scale_y = artwork.scale.y;
  if (ImGui::DragFloat("Adjust Scale Y", &drag_scale_y, 0.01f, 0.01f, 100.0f,
                       "%.3f")) {
    im2d::operations::UpdateImportedArtworkScaleAxis(artwork, 1, drag_scale_y);
  }

  ImGui::Text("Bounds: %.1f x %.1f",
              artwork.bounds_max.x - artwork.bounds_min.x,
              artwork.bounds_max.y - artwork.bounds_min.y);
  ImGui::Text("Part ID: %d", artwork.part.part_id);
  ImGui::Text("Source Artwork ID: %d", artwork.part.source_artwork_id);
  ImGui::Text("Cut Ready: %s", artwork.part.cut_ready ? "Yes" : "No");
  ImGui::Text("Nest Ready: %s", artwork.part.nest_ready ? "Yes" : "No");
  ImGui::Text("Islands: %d", artwork.part.island_count);
  ImGui::Text("Outer Boundaries: %d", artwork.part.outer_contour_count);
  ImGui::Text("Hole Contours: %d", artwork.part.hole_contour_count);
  ImGui::Text("Attached Holes: %d", artwork.part.attached_hole_count);
  ImGui::Text("Orphan Holes: %d", artwork.part.orphan_hole_count);
  ImGui::Text("Ambiguous Cleanup: %d", artwork.part.ambiguous_contour_count);
  ImGui::Text("Closed Contours: %d", artwork.part.closed_contour_count);
  ImGui::Text("Open Contours: %d", artwork.part.open_contour_count);
  ImGui::Text("Placeholder Text: %d", artwork.part.placeholder_count);
  if (!artwork.part.contributing_source_artwork_ids.empty()) {
    std::string provenance = "Provenance: ";
    for (size_t index = 0;
         index < artwork.part.contributing_source_artwork_ids.size(); ++index) {
      if (index != 0) {
        provenance += ", ";
      }
      provenance +=
          std::to_string(artwork.part.contributing_source_artwork_ids[index]);
    }
    ImGui::TextWrapped("%s", provenance.c_str());
  }
  ImGui::Text("Groups: %d",
              std::max(static_cast<int>(artwork.groups.size()) - 1, 0));
  ImGui::Text("Paths: %d", static_cast<int>(artwork.paths.size()));
  ImGui::Text("DXF Text: %d", static_cast<int>(artwork.dxf_text.size()));
  ImGui::Text("Selected Elements: %d",
              static_cast<int>(state.selected_imported_elements.size()));
  if (state.last_imported_operation_issue_artwork_id == artwork.id &&
      !state.last_imported_operation_issue_elements.empty()) {
    ImGui::Text(
        "Skipped By Last Op: %d",
        static_cast<int>(state.last_imported_operation_issue_elements.size()));
  }
  ImVec4 outline_color =
      !artwork.paths.empty()
          ? artwork.paths.front().stroke_color
          : (!artwork.dxf_text.empty() ? artwork.dxf_text.front().stroke_color
                                       : ImVec4(0.92f, 0.94f, 0.97f, 1.0f));
  if (ImGui::ColorEdit4("Outline Color", &outline_color.x,
                        ImGuiColorEditFlags_Float)) {
    im2d::operations::UpdateImportedArtworkOutlineColor(state, artwork.id,
                                                        outline_color);
  }
}

bool DrawImportedArtworkObjectInspectorContentsInternal(
    im2d::CanvasState &state, im2d::ImportedArtwork &artwork) {
  ImportedInspectorFilterMode &filter_mode = GetImportedInspectorFilterMode();
  ImGui::Text("Artwork: %s", artwork.name.c_str());
  ImGui::TextUnformatted("Inspector Filter");
  if (ImGui::RadioButton(
          "Hierarchy", filter_mode == ImportedInspectorFilterMode::Hierarchy)) {
    filter_mode = ImportedInspectorFilterMode::Hierarchy;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton(
          "Flagged", filter_mode == ImportedInspectorFilterMode::FlaggedOnly)) {
    filter_mode = ImportedInspectorFilterMode::FlaggedOnly;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton(
          "Skipped", filter_mode == ImportedInspectorFilterMode::SkippedOnly)) {
    filter_mode = ImportedInspectorFilterMode::SkippedOnly;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("Flagged Or Skipped",
                         filter_mode ==
                             ImportedInspectorFilterMode::FlaggedOrSkipped)) {
    filter_mode = ImportedInspectorFilterMode::FlaggedOrSkipped;
  }

  DrawSelectedImportedItemDetails(artwork, state);
  if (DrawDebugTreeExtractionControls(state, artwork)) {
    return true;
  }
  if (DrawImportedGroupingControls(state, artwork)) {
    return true;
  }

  if (filter_mode == ImportedInspectorFilterMode::Hierarchy) {
    if (DrawImportedDebugTree(state, artwork)) {
      return true;
    }
  } else {
    DrawFilteredImportedItems(state, artwork, filter_mode);
  }
  return false;
}

void DrawImportedArtworkSvgExportContentsInternal(
    im2d::CanvasState &state, im2d::ImportedArtwork &artwork) {
  SvgExportUiState &export_ui = GetSvgExportUiState();
  SyncSvgExportUiState(&export_ui, state, artwork);
  const std::filesystem::path selection_export_path =
      BuildExportPath(export_ui.output_directory, export_ui.selection_filename,
                      DefaultSelectionExportPath(artwork));
  const std::filesystem::path export_area_path = BuildExportPath(
      export_ui.output_directory, export_ui.export_area_filename,
      DefaultExportAreaPath(state));
  const bool has_selected_elements = !state.selected_imported_elements.empty();

  ImGui::TextUnformatted("SVG Export");
  ImGui::Checkbox("Allow Placeholder DXF Export (Diagnostics)",
                  &export_ui.allow_placeholder_text);
  ImGui::InputText("Output Directory", export_ui.output_directory.data(),
                   export_ui.output_directory.size());
  ImGui::InputText("Selection File", export_ui.selection_filename.data(),
                   export_ui.selection_filename.size());
  ImGui::InputText("Export Area File", export_ui.export_area_filename.data(),
                   export_ui.export_area_filename.size());
  if (ImGui::Button("Reset Export Paths")) {
    export_ui.configured_artwork_id = 0;
    export_ui.configured_export_area_id = 0;
    SyncSvgExportUiState(&export_ui, state, artwork);
  }
  ImGui::TextWrapped("Selection Save Path: %s",
                     selection_export_path.lexically_normal().string().c_str());
  ImGui::TextWrapped("Export-Area Save Path: %s",
                     export_area_path.lexically_normal().string().c_str());

  const bool has_debug_export_target =
      state.selected_imported_debug.artwork_id == artwork.id &&
      state.selected_imported_debug.kind !=
          im2d::ImportedDebugSelectionKind::None;
  const bool has_export_selection =
      has_selected_elements || has_debug_export_target;

  if (!has_export_selection) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button("Preview Selection SVG")) {
    im2d::exporter::SvgExportRequest request;
    request.scope = im2d::exporter::SvgExportScope::ActiveSelection;
    request.imported_artwork_id = artwork.id;
    request.allow_placeholder_text = export_ui.allow_placeholder_text;
    GetSvgExportPreview() = im2d::exporter::ExportSvg(state, request);
  }
  if (!has_export_selection) {
    ImGui::EndDisabled();
  }
  ImGui::SameLine();
  if (!has_export_selection) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button("Save Selection SVG")) {
    im2d::exporter::SvgExportRequest request;
    request.scope = im2d::exporter::SvgExportScope::ActiveSelection;
    request.imported_artwork_id = artwork.id;
    request.allow_placeholder_text = export_ui.allow_placeholder_text;
    GetSvgExportPreview() =
        im2d::exporter::ExportSvgToFile(state, request, selection_export_path);
  }
  if (!has_export_selection) {
    ImGui::EndDisabled();
  }

  const bool has_export_area = !state.export_areas.empty();
  if (!has_export_area) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button("Preview Export Area SVG")) {
    im2d::exporter::SvgExportRequest request;
    request.scope = im2d::exporter::SvgExportScope::ExportArea;
    request.allow_placeholder_text = export_ui.allow_placeholder_text;
    GetSvgExportPreview() = im2d::exporter::ExportSvg(state, request);
  }
  if (!has_export_area) {
    ImGui::EndDisabled();
  }
  ImGui::SameLine();
  if (!has_export_area) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button("Save Export Area SVG")) {
    im2d::exporter::SvgExportRequest request;
    request.scope = im2d::exporter::SvgExportScope::ExportArea;
    request.allow_placeholder_text = export_ui.allow_placeholder_text;
    GetSvgExportPreview() =
        im2d::exporter::ExportSvgToFile(state, request, export_area_path);
  }
  if (!has_export_area) {
    ImGui::EndDisabled();
  }
  ImGui::SameLine();
  if (GetSvgExportPreview().svg.empty()) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button("Copy Preview SVG")) {
    ImGui::SetClipboardText(GetSvgExportPreview().svg.c_str());
  }
  if (GetSvgExportPreview().svg.empty()) {
    ImGui::EndDisabled();
  }

  if (!GetSvgExportPreview().message.empty()) {
    ImGui::TextWrapped("%s", SvgExportSummary(GetSvgExportPreview()).c_str());
    DrawSvgExportWarnings(GetSvgExportPreview());
  }
}

void DrawImportedArtworkOperationStatusContents(
    im2d::CanvasState &state, const im2d::ImportedArtwork &artwork) {
  const im2d::ImportedArtworkOperationResult &operation =
      state.last_imported_artwork_operation;
  if (operation.message.empty()) {
    return;
  }

  ImGui::TextWrapped("%s", operation.message.c_str());
  if (operation.artwork_id != artwork.id &&
      operation.created_artwork_id != artwork.id) {
    return;
  }

  if (operation.selected_count > 0) {
    ImGui::Text("Selected: %d", operation.selected_count);
  }
  if (operation.moved_count > 0) {
    ImGui::Text("Moved: %d", operation.moved_count);
  }
  if (operation.skipped_count > 0) {
    ImGui::Text("Skipped: %d", operation.skipped_count);
    if (state.last_imported_operation_issue_artwork_id == artwork.id &&
        !state.last_imported_operation_issue_elements.empty()) {
      if (ImGui::Button("Select Skipped Elements")) {
        SelectLastOperationIssueElements(state, artwork.id);
      }
    }
  }
  if (operation.stitched_count > 0 || operation.cleaned_count > 0 ||
      operation.ambiguous_count > 0 || operation.closed_count > 0 ||
      operation.open_count > 0 || operation.placeholder_count > 0 ||
      operation.preserved_count > 0) {
    const bool operation_is_prepare =
        operation.message.rfind("Prepare", 0) == 0;
    if (operation_is_prepare) {
      ImGui::Text("Mode: %s",
                  operation.prepare_mode ==
                          im2d::ImportedArtworkPrepareMode::AggressiveCleanup
                      ? "Prepare + Weld Cleanup"
                      : "Prepare For Cutting");
    }
    ImGui::Text("Part ID: %d", operation.part_id);
    ImGui::Text("Cut Ready: %s", operation.cut_ready ? "Yes" : "No");
    ImGui::Text("Nest Ready: %s", operation.nest_ready ? "Yes" : "No");
    ImGui::Text("Islands: %d", operation.island_count);
    ImGui::Text("Outer: %d", operation.outer_count);
    ImGui::Text("Holes: %d", operation.hole_count);
    ImGui::Text("Attached Holes: %d", operation.attached_hole_count);
    ImGui::Text("Orphan Holes: %d", operation.orphan_hole_count);
    ImGui::Text("Preserved: %d", operation.preserved_count);
    ImGui::Text("Stitched: %d", operation.stitched_count);
    ImGui::Text("Cleaned: %d", operation.cleaned_count);
    ImGui::Text("Ambiguous Cleanup: %d", operation.ambiguous_count);
    ImGui::Text("Closed: %d", operation.closed_count);
    ImGui::Text("Open: %d", operation.open_count);
    ImGui::Text("Placeholder: %d", operation.placeholder_count);
  }
}

void DrawImportedArtworkTransformContents(im2d::CanvasState &state,
                                          im2d::ImportedArtwork &artwork) {
  ImGui::TextUnformatted("Transforms");
  if (ImGui::Button("Flip Horizontal")) {
    im2d::operations::FlipImportedArtworkHorizontal(state, artwork.id);
  }
  ImGui::SameLine();
  if (ImGui::Button("Flip Vertical")) {
    im2d::operations::FlipImportedArtworkVertical(state, artwork.id);
  }
  if (ImGui::Button("Rotate 90 CW")) {
    im2d::operations::RotateImportedArtworkClockwise(state, artwork.id);
  }
  ImGui::SameLine();
  if (ImGui::Button("Rotate 90 CCW")) {
    im2d::operations::RotateImportedArtworkCounterClockwise(state, artwork.id);
  }
}

} // namespace

void DrawImportedArtworkListContents(im2d::CanvasState &state) {
  ImGui::Text("Imported Objects: %d",
              static_cast<int>(state.imported_artwork.size()));
  ImGui::SameLine();
  if (state.imported_artwork.empty()) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button("Delete All")) {
    im2d::ClearImportedArtwork(state);
  }
  if (state.imported_artwork.empty()) {
    ImGui::EndDisabled();
  }
  ImGui::Separator();

  if (state.imported_artwork.empty()) {
    ImGui::TextUnformatted("No imported objects on the canvas.");
    return;
  }

  int pending_delete_artwork_id = 0;
  for (im2d::ImportedArtwork &artwork : state.imported_artwork) {
    ImGui::PushID(artwork.id);
    ImGui::Checkbox("##visible", &artwork.visible);
    ImGui::SameLine();
    const std::string label = artwork.name + " [" + artwork.source_format + "]";
    const bool selected = state.selected_imported_artwork_id == artwork.id;
    if (ImGui::Selectable(label.c_str(), selected)) {
      SelectImportedArtwork(state, artwork.id);
    }
    if (ImGui::BeginPopupContextItem("canvas_object_row_menu")) {
      if (ImGui::MenuItem("Select", nullptr, selected)) {
        SelectImportedArtwork(state, artwork.id);
      }
      if (ImGui::MenuItem("Delete")) {
        pending_delete_artwork_id = artwork.id;
      }
      ImGui::EndPopup();
    }
    ImGui::PopID();
  }

  if (pending_delete_artwork_id != 0) {
    im2d::operations::DeleteImportedArtwork(state, pending_delete_artwork_id);
  }
}

void DrawImportedArtworkListWindow(im2d::CanvasState &state,
                                   const char *window_title) {
  ImGui::Begin(window_title);
  DrawImportedArtworkListContents(state);
  ImGui::End();
}

void DrawImportedArtworkSvgExportContents(im2d::CanvasState &state) {
  im2d::ImportedArtwork *artwork = ResolveSelectedImportedArtworkForUi(state);
  if (artwork == nullptr) {
    ImGui::TextUnformatted(
        "Select an imported object to preview or save SVG exports.");
    return;
  }
  DrawImportedArtworkSvgExportContentsInternal(state, *artwork);
}

void DrawImportedArtworkCanvasOperationsContents(im2d::CanvasState &state) {
  im2d::ImportedArtwork *artwork = ResolveSelectedImportedArtworkForUi(state);
  if (artwork == nullptr) {
    ImGui::TextUnformatted(
        "Select an imported object to access canvas operations.");
    return;
  }

  DrawImportedArtworkCanvasOverviewContents(state, *artwork);
  ImGui::Separator();
  DrawImportedArtworkTransformContents(state, *artwork);
}

void DrawImportedArtworkCutOperationsContents(im2d::CanvasState &state) {
  im2d::ImportedArtwork *artwork = ResolveSelectedImportedArtworkForUi(state);
  if (artwork == nullptr) {
    ImGui::TextUnformatted("Select an imported object to access extraction, "
                           "cut, and separation operations.");
    return;
  }

  if (DrawImportedArtworkWorkflowContents(state)) {
    return;
  }
  ImGui::Separator();
  DrawImportedArtworkOperationStatusContents(state, *artwork);
}

void DrawImportedArtworkObjectInspectorSectionContents(
    im2d::CanvasState &state) {
  im2d::ImportedArtwork *artwork = ResolveSelectedImportedArtworkForUi(state);
  if (artwork == nullptr) {
    ImGui::TextUnformatted("Select an imported object to inspect it.");
    return;
  }

  DrawImportedArtworkObjectInspectorContentsInternal(state, *artwork);
}

void DrawImportedArtworkInspectorContents(im2d::CanvasState &state) {
  im2d::ImportedArtwork *artwork = ResolveSelectedImportedArtworkForUi(state);
  if (artwork == nullptr) {
    ImGui::TextUnformatted("Select an imported object to inspect it.");
    return;
  }

  DrawImportedArtworkCanvasOverviewContents(state, *artwork);
  ImGui::Separator();
  if (DrawImportedArtworkObjectInspectorContentsInternal(state, *artwork)) {
    return;
  }
  ImGui::Separator();
  DrawImportedArtworkSvgExportContentsInternal(state, *artwork);
  ImGui::Separator();
  DrawImportedArtworkOperationStatusContents(state, *artwork);
}

void DrawImportedArtworkInspectorWindow(im2d::CanvasState &state,
                                        const char *window_title) {
  ImGui::Begin(window_title);
  DrawPrepareForCuttingModal(state);
  DrawImportedArtworkInspectorContents(state);
  ImGui::End();
}

bool DrawImportedArtworkWorkflowContents(im2d::CanvasState &state) {
  im2d::ImportedArtwork *artwork =
      im2d::FindImportedArtwork(state, state.selected_imported_artwork_id);
  if (artwork == nullptr) {
    ImGui::TextUnformatted("Select an imported object to access marquee, "
                           "cutting, and extraction controls.");
    return false;
  }

  ImGui::Text("Workflow For: %s", artwork->name.c_str());
  ImGui::Text("Selected Elements: %d",
              static_cast<int>(state.selected_imported_elements.size()));
  ImGui::Separator();

  ImGui::TextUnformatted("Marquee Selection");
  if (ImGui::RadioButton("None", state.imported_artwork_edit_mode ==
                                     im2d::ImportedArtworkEditMode::None)) {
    state.imported_artwork_edit_mode = im2d::ImportedArtworkEditMode::None;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("Rect Marquee",
                         state.imported_artwork_edit_mode ==
                             im2d::ImportedArtworkEditMode::SelectRectangle)) {
    state.imported_artwork_edit_mode =
        im2d::ImportedArtworkEditMode::SelectRectangle;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("Oval Marquee",
                         state.imported_artwork_edit_mode ==
                             im2d::ImportedArtworkEditMode::SelectOval)) {
    state.imported_artwork_edit_mode =
        im2d::ImportedArtworkEditMode::SelectOval;
  }

  ImGui::Separator();
  ImGui::TextUnformatted("Extraction");
  const bool has_selected_elements =
      !state.selected_imported_elements.empty() &&
      state.selected_imported_artwork_id == artwork->id;
  const bool has_debug_target =
      im2d::HasExtractableImportedDebugSelection(state, *artwork);
  if (!has_selected_elements && !has_debug_target) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button(ExtractActionLabel(state, *artwork))) {
    im2d::operations::ExtractSelectedImportedElements(state, artwork->id);
    if (!has_selected_elements && !has_debug_target) {
      ImGui::EndDisabled();
    }
    return true;
  }
  if (!has_selected_elements && !has_debug_target) {
    ImGui::EndDisabled();
  }
  if (!has_selected_elements && !has_debug_target) {
    ImGui::TextUnformatted("Use a marquee mode on the canvas or select a "
                           "group, path, or DXF text in the debug tree first.");
  }

  if (DrawImportedGroupingControls(state, *artwork)) {
    return true;
  }

  ImGui::Separator();
  ImGui::TextUnformatted("Cut Preparation");
  if (ImGui::Button("Auto Close To Polyline")) {
    im2d::AutoCloseImportedArtworkToPolyline(state, artwork->id);
  }
  ImGui::SameLine();
  PrepareWorkflowUiState &prepare_workflow = GetPrepareWorkflowUiState();
  ImGui::Checkbox("Auto Close Before Prepare",
                  &prepare_workflow.auto_close_before_prepare);

  if (ImGui::Button("Prepare For Cutting")) {
    QueuePrepareForCuttingDialog(
        *artwork, 0.5f, im2d::ImportedArtworkPrepareMode::FidelityFirst);
  }
  ImGui::SameLine();
  if (ImGui::Button("Prepare + Weld Cleanup")) {
    QueuePrepareForCuttingDialog(
        *artwork, 0.5f, im2d::ImportedArtworkPrepareMode::AggressiveCleanup);
  }

  ImGui::Separator();
  ImGui::TextUnformatted("Auto Cut Preview");
  ImGui::TextWrapped(
      "Infer temporary cut bands from the selected artwork bounds and show the "
      "same overlay style as the guide-band preview.");
  AutoCutPreviewUiState &auto_cut_preview = GetAutoCutPreviewUiState();
  CutOperationUiState &cut_operations = GetCutOperationUiState();
  ImGui::Checkbox("Auto Group Cut Outputs",
                  &cut_operations.auto_group_cut_outputs);
  ImGui::TextWrapped(
      "When enabled, each artwork created by Apply Cut will wrap its root "
      "contents into a new group automatically.");
  if (ImGui::BeginCombo("Axis Mode",
                        AutoCutAxisModeLabel(auto_cut_preview.axis_mode))) {
    constexpr im2d::AutoCutPreviewAxisMode kAxisModes[] = {
        im2d::AutoCutPreviewAxisMode::VerticalOnly,
        im2d::AutoCutPreviewAxisMode::HorizontalOnly,
        im2d::AutoCutPreviewAxisMode::Both};
    for (im2d::AutoCutPreviewAxisMode axis_mode : kAxisModes) {
      const bool selected = auto_cut_preview.axis_mode == axis_mode;
      if (ImGui::Selectable(AutoCutAxisModeLabel(axis_mode), selected)) {
        auto_cut_preview.axis_mode = axis_mode;
      }
      if (selected) {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }
  ImGui::SetNextItemWidth(140.0f);
  ImGui::DragFloat("Minimum Gap", &auto_cut_preview.minimum_gap, 0.25f, 0.25f,
                   1000.0f, "%.2f");
  if (ImGui::Button("Preview Auto Cut")) {
    ClearCutOperationPreviews(state);
    im2d::PreviewImportedArtworkAutoCut(state, artwork->id,
                                        auto_cut_preview.axis_mode,
                                        auto_cut_preview.minimum_gap);
  }

  const bool has_auto_cut_preview = HasActiveAutoCutPreview(state, artwork->id);
  if (has_auto_cut_preview) {
    ImGui::Separator();
    const im2d::ImportedArtworkAutoCutPreview &preview =
        state.imported_artwork_auto_cut_preview;
    const int assigned_count = CountPreviewPartsByClassification(
        preview, im2d::ImportedSeparationPreviewClassification::Assigned);
    const int crossing_count = CountPreviewPartsByClassification(
        preview, im2d::ImportedSeparationPreviewClassification::Crossing);
    const int orphan_count = CountPreviewPartsByClassification(
        preview, im2d::ImportedSeparationPreviewClassification::Orphan);
    ImGui::TextUnformatted("Auto Cut Preview");
    ImGui::Text("Axis Mode: %s", AutoCutAxisModeLabel(preview.axis_mode));
    ImGui::Text("Minimum Gap: %.2f", preview.minimum_gap);
    ImGui::Text("Vertical Cuts: %d",
                static_cast<int>(preview.vertical_positions.size()));
    ImGui::Text("Horizontal Cuts: %d",
                static_cast<int>(preview.horizontal_positions.size()));
    ImGui::Text("Bands: %d", preview.future_band_count);
    ImGui::Text("Skipped: %d", preview.skipped_count);
    ImGui::Text("Assigned Parts: %d", assigned_count);
    ImGui::Text("Crossing Parts: %d", crossing_count);
    ImGui::Text("Orphan Parts: %d", orphan_count);
    if (!preview.message.empty()) {
      ImGui::TextWrapped("%s", preview.message.c_str());
    }
  }

  ImGui::Separator();
  ImGui::TextUnformatted("Guide Separation");
  ImGui::TextWrapped(
      "Preview and apply multi-guide cuts for the selected artwork. Parts that "
      "cross a guide stay skipped until the guides cleanly separate them.");
  const int preview_anchor_guide_id =
      GetPreviewAnchorGuideId(state, artwork->id);
  const bool has_guides = preview_anchor_guide_id != 0;
  if (!has_guides) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button("Preview Multi-Guide Split")) {
    ClearCutOperationPreviews(state);
    im2d::PreviewSeparateImportedArtworkByGuide(state, artwork->id,
                                                preview_anchor_guide_id);
  }
  if (!has_guides) {
    ImGui::EndDisabled();
  }
  if (!has_guides) {
    ImGui::TextUnformatted(
        "Add at least one guide on the canvas to enable split preview.");
  } else {
    ImGui::Text("Active Guides: %d", static_cast<int>(state.guides.size()));
    DrawPreviewAnchorGuideSelector(state, preview_anchor_guide_id);
  }

  const bool has_separation_preview =
      HasActiveSeparationPreview(state, artwork->id);
  if (has_separation_preview) {
    ImGui::Separator();
    const im2d::ImportedArtworkSeparationPreview &preview =
        state.imported_artwork_separation_preview;
    const int assigned_count = CountPreviewPartsByClassification(
        preview, im2d::ImportedSeparationPreviewClassification::Assigned);
    const int crossing_count = CountPreviewPartsByClassification(
        preview, im2d::ImportedSeparationPreviewClassification::Crossing);
    const int orphan_count = CountPreviewPartsByClassification(
        preview, im2d::ImportedSeparationPreviewClassification::Orphan);
    ImGui::TextUnformatted("Guide Split Preview");
    ImGui::Text("Guides: %d", static_cast<int>(preview.guide_ids.size()));
    ImGui::Text("Future Objects: %d", preview.future_object_count);
    ImGui::Text("Skipped: %d", preview.skipped_count);
    ImGui::Text("Assigned Parts: %d", assigned_count);
    ImGui::Text("Crossing Parts: %d", crossing_count);
    ImGui::Text("Orphan Parts: %d", orphan_count);
    if (!preview.message.empty()) {
      ImGui::TextWrapped("%s", preview.message.c_str());
    }
  }

  if (has_auto_cut_preview || has_separation_preview) {
    ImGui::Separator();
    if (has_auto_cut_preview) {
      const im2d::ImportedArtworkAutoCutPreview &preview =
          state.imported_artwork_auto_cut_preview;
      if (ImGui::Button("Apply Cut")) {
        im2d::ApplyImportedArtworkAutoCut(
            state, artwork->id, preview.axis_mode, preview.minimum_gap,
            cut_operations.auto_group_cut_outputs);
      }
      ImGui::SameLine();
    } else if (has_separation_preview) {
      const im2d::ImportedArtworkSeparationPreview &preview =
          state.imported_artwork_separation_preview;
      if (ImGui::Button("Apply Cut")) {
        im2d::SeparateImportedArtworkByGuide(
            state, artwork->id, preview.guide_id,
            cut_operations.auto_group_cut_outputs);
      }
      ImGui::SameLine();
    }
    if (ImGui::Button("Clear Preview")) {
      ClearCutOperationPreviews(state);
    }
  }
  return false;
}

void DrawImportedArtworkWorkflowWindow(im2d::CanvasState &state,
                                       const char *window_title) {
  ImGui::Begin(window_title);
  DrawPrepareForCuttingModal(state);
  DrawImportedArtworkWorkflowContents(state);
  ImGui::End();
}

void DrawImportResultSummary(const im2d::importer::ImportResult &result) {
  if (result.message.empty()) {
    return;
  }

  ImGui::TextWrapped("%s", result.message.c_str());
  if (!result.success || result.warnings_count > 0 ||
      result.skipped_items_count > 0) {
    ImGui::Text("Warnings: %d", result.warnings_count);
    ImGui::Text("Skipped: %d", result.skipped_items_count);
  }
  for (const std::string &note : result.notes) {
    ImGui::BulletText("%s", note.c_str());
  }
}

} // namespace demo