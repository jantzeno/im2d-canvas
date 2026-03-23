#include "demo_imported_artwork_windows.h"

#include "../canvas/im2d_canvas_document.h"
#include "../operations/im2d_operations.h"

#include <algorithm>

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

bool HasAnyDxfArtwork(const im2d::CanvasState &state) {
  return std::any_of(state.imported_artwork.begin(),
                     state.imported_artwork.end(),
                     [](const im2d::ImportedArtwork &artwork) {
                       return artwork.source_format == "DXF";
                     });
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
                          const im2d::ImportedPath &path) {
  const bool skipped = IsSkippedImportedElement(
      state, artwork.id, im2d::ImportedElementKind::Path, path.id);
  const ImGuiTreeNodeFlags flags =
      ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
      ImGuiTreeNodeFlags_SpanAvailWidth |
      (IsSelectedImportedDebugItem(
           state, artwork.id, im2d::ImportedDebugSelectionKind::Path, path.id)
           ? ImGuiTreeNodeFlags_Selected
           : 0);
  const std::string label =
      DecorateImportedItemSummary(ImportedPathSummary(path), skipped);
  ImGui::TreeNodeEx(reinterpret_cast<void *>(static_cast<intptr_t>(path.id)),
                    flags, "%s", label.c_str());
  if (ImGui::IsItemClicked()) {
    SelectImportedPath(state, artwork.id, path.id);
  }
}

void DrawImportedDxfTextNode(im2d::CanvasState &state,
                             const im2d::ImportedArtwork &artwork,
                             const im2d::ImportedDxfText &text) {
  const bool skipped = IsSkippedImportedElement(
      state, artwork.id, im2d::ImportedElementKind::DxfText, text.id);
  const ImGuiTreeNodeFlags flags =
      ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
      ImGuiTreeNodeFlags_SpanAvailWidth |
      (IsSelectedImportedDebugItem(state, artwork.id,
                                   im2d::ImportedDebugSelectionKind::DxfText,
                                   text.id)
           ? ImGuiTreeNodeFlags_Selected
           : 0);
  const std::string label =
      DecorateImportedItemSummary(ImportedDxfTextSummary(text), skipped);
  ImGui::TreeNodeEx(
      reinterpret_cast<void *>(static_cast<intptr_t>(1000000 + text.id)), flags,
      "%s", label.c_str());
  if (ImGui::IsItemClicked()) {
    SelectImportedDxfText(state, artwork.id, text.id);
  }
}

void DrawImportedGroupNode(im2d::CanvasState &state,
                           const im2d::ImportedArtwork &artwork,
                           const im2d::ImportedGroup &group) {
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
  if (!open) {
    return;
  }

  for (const int child_group_id : group.child_group_ids) {
    const im2d::ImportedGroup *child_group =
        im2d::FindImportedGroup(artwork, child_group_id);
    if (child_group != nullptr) {
      DrawImportedGroupNode(state, artwork, *child_group);
    }
  }

  for (const int path_id : group.path_ids) {
    const im2d::ImportedPath *path = im2d::FindImportedPath(artwork, path_id);
    if (path != nullptr) {
      DrawImportedPathNode(state, artwork, *path);
    }
  }

  for (const int text_id : group.dxf_text_ids) {
    const im2d::ImportedDxfText *text =
        im2d::FindImportedDxfText(artwork, text_id);
    if (text != nullptr) {
      DrawImportedDxfTextNode(state, artwork, *text);
    }
  }

  ImGui::TreePop();
}

void DrawImportedDebugTree(im2d::CanvasState &state,
                           const im2d::ImportedArtwork &artwork) {
  ImGui::Separator();
  ImGui::TextUnformatted("Debug Tree");

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
  if (!root_open) {
    return;
  }

  const im2d::ImportedGroup *root_group =
      im2d::FindImportedGroup(artwork, artwork.root_group_id);
  if (root_group != nullptr) {
    for (const int child_group_id : root_group->child_group_ids) {
      const im2d::ImportedGroup *child_group =
          im2d::FindImportedGroup(artwork, child_group_id);
      if (child_group != nullptr) {
        DrawImportedGroupNode(state, artwork, *child_group);
      }
    }
    for (const int path_id : root_group->path_ids) {
      const im2d::ImportedPath *path = im2d::FindImportedPath(artwork, path_id);
      if (path != nullptr) {
        DrawImportedPathNode(state, artwork, *path);
      }
    }
    for (const int text_id : root_group->dxf_text_ids) {
      const im2d::ImportedDxfText *text =
          im2d::FindImportedDxfText(artwork, text_id);
      if (text != nullptr) {
        DrawImportedDxfTextNode(state, artwork, *text);
      }
    }
  } else {
    for (const im2d::ImportedPath &path : artwork.paths) {
      DrawImportedPathNode(state, artwork, path);
    }
    for (const im2d::ImportedDxfText &text : artwork.dxf_text) {
      DrawImportedDxfTextNode(state, artwork, text);
    }
  }

  ImGui::TreePop();
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
    DrawImportedPathNode(state, artwork, path);
    drew_any = true;
  }

  for (const im2d::ImportedDxfText &text : artwork.dxf_text) {
    const bool flagged = text.issue_flags != im2d::ImportedElementIssueFlagNone;
    const bool skipped = IsSkippedImportedElement(
        state, artwork.id, im2d::ImportedElementKind::DxfText, text.id);
    if (!MatchesFilter(flagged, skipped, mode)) {
      continue;
    }
    DrawImportedDxfTextNode(state, artwork, text);
    drew_any = true;
  }

  if (!drew_any) {
    ImGui::TextUnformatted("No imported items match the current filter.");
  }
}

} // namespace

void DrawImportedArtworkListWindow(im2d::CanvasState &state,
                                   const char *window_title) {
  ImGui::Begin(window_title);
  ImGui::Text("Imported Objects: %d",
              static_cast<int>(state.imported_artwork.size()));
  if (HasAnyDxfArtwork(state)) {
    ImGui::Checkbox("Show Imported DXF Text", &state.show_imported_dxf_text);
  }
  ImGui::Separator();

  if (state.imported_artwork.empty()) {
    ImGui::TextUnformatted("No imported objects on the canvas.");
    ImGui::End();
    return;
  }

  for (im2d::ImportedArtwork &artwork : state.imported_artwork) {
    ImGui::PushID(artwork.id);
    ImGui::Checkbox("##visible", &artwork.visible);
    ImGui::SameLine();
    const std::string label = artwork.name + " [" + artwork.source_format + "]";
    if (ImGui::Selectable(label.c_str(),
                          state.selected_imported_artwork_id == artwork.id)) {
      SelectImportedArtwork(state, artwork.id);
    }
    ImGui::PopID();
  }

  ImGui::End();
}

void DrawImportedArtworkInspectorWindow(im2d::CanvasState &state,
                                        const char *window_title) {
  ImGui::Begin(window_title);

  im2d::ImportedArtwork *artwork =
      im2d::FindImportedArtwork(state, state.selected_imported_artwork_id);
  if (artwork == nullptr) {
    if (state.selected_imported_artwork_id != 0) {
      state.selected_imported_artwork_id = 0;
      im2d::ClearImportedDebugSelection(state);
      im2d::ClearSelectedImportedElements(state);
    }
    ImGui::TextUnformatted("Select an imported object to inspect it.");
    ImGui::End();
    return;
  }

  ImGui::Text("ID: %d", artwork->id);
  ImGui::TextUnformatted(artwork->name.c_str());
  ImGui::Separator();
  ImGui::Text("Format: %s", artwork->source_format.c_str());
  ImGui::TextWrapped("Source: %s", artwork->source_path.c_str());
  ImGui::Checkbox("Visible", &artwork->visible);
  if (artwork->source_format == "DXF") {
    ImGui::Checkbox("Show Imported DXF Text", &state.show_imported_dxf_text);
  }

  float position[2] = {artwork->origin.x, artwork->origin.y};
  if (ImGui::InputFloat2("Position", position, "%.2f")) {
    artwork->origin = ImVec2(position[0], position[1]);
  }
  ImGui::SameLine();
  if (ImGui::Button("Reset Position")) {
    artwork->origin = ImVec2(0.0f, 0.0f);
  }

  float scale[2] = {artwork->scale.x, artwork->scale.y};
  if (ImGui::InputFloat2("Scale", scale, "%.3f")) {
    artwork->scale =
        ImVec2(std::max(scale[0], 0.01f), std::max(scale[1], 0.01f));
  }
  ImGui::SameLine();
  if (ImGui::Button("Reset Scale")) {
    artwork->scale = ImVec2(1.0f, 1.0f);
  }

  bool lock_scale_ratio =
      im2d::operations::IsImportedArtworkScaleRatioLocked(*artwork);
  if (ImGui::Checkbox("Lock Scale Ratio", &lock_scale_ratio)) {
    im2d::operations::SetImportedArtworkScaleRatioLocked(*artwork,
                                                         lock_scale_ratio);
  }

  float scale_x = artwork->scale.x;
  if (ImGui::InputFloat("Scale X", &scale_x, 0.0f, 0.0f, "%.3f")) {
    im2d::operations::UpdateImportedArtworkScaleAxis(*artwork, 0, scale_x);
  }

  float scale_y = artwork->scale.y;
  if (ImGui::InputFloat("Scale Y", &scale_y, 0.0f, 0.0f, "%.3f")) {
    im2d::operations::UpdateImportedArtworkScaleAxis(*artwork, 1, scale_y);
  }

  ImGui::DragFloat2("Adjust Position", &artwork->origin.x, 1.0f, 0.0f, 0.0f,
                    "%.2f");
  float drag_scale_x = artwork->scale.x;
  if (ImGui::DragFloat("Adjust Scale X", &drag_scale_x, 0.01f, 0.01f, 100.0f,
                       "%.3f")) {
    im2d::operations::UpdateImportedArtworkScaleAxis(*artwork, 0, drag_scale_x);
  }

  float drag_scale_y = artwork->scale.y;
  if (ImGui::DragFloat("Adjust Scale Y", &drag_scale_y, 0.01f, 0.01f, 100.0f,
                       "%.3f")) {
    im2d::operations::UpdateImportedArtworkScaleAxis(*artwork, 1, drag_scale_y);
  }

  ImGui::Text("Bounds: %.1f x %.1f",
              artwork->bounds_max.x - artwork->bounds_min.x,
              artwork->bounds_max.y - artwork->bounds_min.y);
  ImGui::Text("Part ID: %d", artwork->part.part_id);
  ImGui::Text("Source Artwork ID: %d", artwork->part.source_artwork_id);
  ImGui::Text("Cut Ready: %s", artwork->part.cut_ready ? "Yes" : "No");
  ImGui::Text("Nest Ready: %s", artwork->part.nest_ready ? "Yes" : "No");
  ImGui::Text("Islands: %d", artwork->part.island_count);
  ImGui::Text("Outer Boundaries: %d", artwork->part.outer_contour_count);
  ImGui::Text("Hole Contours: %d", artwork->part.hole_contour_count);
  ImGui::Text("Attached Holes: %d", artwork->part.attached_hole_count);
  ImGui::Text("Orphan Holes: %d", artwork->part.orphan_hole_count);
  ImGui::Text("Ambiguous Cleanup: %d", artwork->part.ambiguous_contour_count);
  ImGui::Text("Closed Contours: %d", artwork->part.closed_contour_count);
  ImGui::Text("Open Contours: %d", artwork->part.open_contour_count);
  ImGui::Text("Placeholder Text: %d", artwork->part.placeholder_count);
  if (!artwork->part.contributing_source_artwork_ids.empty()) {
    std::string provenance = "Provenance: ";
    for (size_t index = 0;
         index < artwork->part.contributing_source_artwork_ids.size();
         ++index) {
      if (index != 0) {
        provenance += ", ";
      }
      provenance +=
          std::to_string(artwork->part.contributing_source_artwork_ids[index]);
    }
    ImGui::TextWrapped("%s", provenance.c_str());
  }
  ImGui::Text("Groups: %d",
              std::max(static_cast<int>(artwork->groups.size()) - 1, 0));
  ImGui::Text("Paths: %d", static_cast<int>(artwork->paths.size()));
  ImGui::Text("DXF Text: %d", static_cast<int>(artwork->dxf_text.size()));
  ImGui::Text("Selected Elements: %d",
              static_cast<int>(state.selected_imported_elements.size()));
  if (state.last_imported_operation_issue_artwork_id == artwork->id &&
      !state.last_imported_operation_issue_elements.empty()) {
    ImGui::Text(
        "Skipped By Last Op: %d",
        static_cast<int>(state.last_imported_operation_issue_elements.size()));
  }
  ImGui::Separator();

  ImGui::TextUnformatted("Edit Mode");
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

  ImVec4 outline_color =
      !artwork->paths.empty()
          ? artwork->paths.front().stroke_color
          : (!artwork->dxf_text.empty() ? artwork->dxf_text.front().stroke_color
                                        : ImVec4(0.92f, 0.94f, 0.97f, 1.0f));
  if (ImGui::ColorEdit4("Outline Color", &outline_color.x,
                        ImGuiColorEditFlags_Float)) {
    im2d::operations::UpdateImportedArtworkOutlineColor(state, artwork->id,
                                                        outline_color);
  }

  ImportedInspectorFilterMode &filter_mode = GetImportedInspectorFilterMode();
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

  DrawSelectedImportedItemDetails(*artwork, state);

  if (filter_mode == ImportedInspectorFilterMode::Hierarchy) {
    DrawImportedDebugTree(state, *artwork);
  } else {
    DrawFilteredImportedItems(state, *artwork, filter_mode);
  }
  ImGui::Separator();

  if (ImGui::Button("Prepare For Cutting")) {
    im2d::operations::PrepareImportedArtworkForCutting(state, artwork->id);
  }
  ImGui::SameLine();
  const bool has_selected_elements = !state.selected_imported_elements.empty();
  if (!has_selected_elements) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button("Extract Selection")) {
    im2d::operations::ExtractSelectedImportedElements(state, artwork->id);
  }
  if (!has_selected_elements) {
    ImGui::EndDisabled();
  }

  const im2d::ImportedArtworkOperationResult &operation =
      state.last_imported_artwork_operation;
  if (!operation.message.empty()) {
    ImGui::Separator();
    ImGui::TextWrapped("%s", operation.message.c_str());
    if (operation.artwork_id == artwork->id ||
        operation.created_artwork_id == artwork->id) {
      if (operation.selected_count > 0) {
        ImGui::Text("Selected: %d", operation.selected_count);
      }
      if (operation.moved_count > 0) {
        ImGui::Text("Moved: %d", operation.moved_count);
      }
      if (operation.skipped_count > 0) {
        ImGui::Text("Skipped: %d", operation.skipped_count);
        if (state.last_imported_operation_issue_artwork_id == artwork->id &&
            !state.last_imported_operation_issue_elements.empty()) {
          if (ImGui::Button("Select Skipped Elements")) {
            SelectLastOperationIssueElements(state, artwork->id);
          }
        }
      }
      if (operation.stitched_count > 0 || operation.cleaned_count > 0 ||
          operation.ambiguous_count > 0 || operation.closed_count > 0 ||
          operation.open_count > 0 || operation.placeholder_count > 0) {
        ImGui::Text("Part ID: %d", operation.part_id);
        ImGui::Text("Cut Ready: %s", operation.cut_ready ? "Yes" : "No");
        ImGui::Text("Nest Ready: %s", operation.nest_ready ? "Yes" : "No");
        ImGui::Text("Islands: %d", operation.island_count);
        ImGui::Text("Outer: %d", operation.outer_count);
        ImGui::Text("Holes: %d", operation.hole_count);
        ImGui::Text("Attached Holes: %d", operation.attached_hole_count);
        ImGui::Text("Orphan Holes: %d", operation.orphan_hole_count);
        ImGui::Text("Stitched: %d", operation.stitched_count);
        ImGui::Text("Cleaned: %d", operation.cleaned_count);
        ImGui::Text("Ambiguous Cleanup: %d", operation.ambiguous_count);
        ImGui::Text("Closed: %d", operation.closed_count);
        ImGui::Text("Open: %d", operation.open_count);
        ImGui::Text("Placeholder: %d", operation.placeholder_count);
      }
    }
  }

  if (ImGui::Button("Flip Horizontal")) {
    im2d::operations::FlipImportedArtworkHorizontal(state, artwork->id);
  }
  ImGui::SameLine();
  if (ImGui::Button("Flip Vertical")) {
    im2d::operations::FlipImportedArtworkVertical(state, artwork->id);
  }
  if (ImGui::Button("Rotate 90 CW")) {
    im2d::operations::RotateImportedArtworkClockwise(state, artwork->id);
  }
  ImGui::SameLine();
  if (ImGui::Button("Rotate 90 CCW")) {
    im2d::operations::RotateImportedArtworkCounterClockwise(state, artwork->id);
  }
  if (ImGui::Button("Delete")) {
    im2d::operations::DeleteImportedArtwork(state, artwork->id);
  }

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