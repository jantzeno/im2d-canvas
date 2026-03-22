#include "demo_imported_artwork_windows.h"

#include "../canvas/im2d_canvas_document.h"

#include <algorithm>

#include <imgui.h>

namespace demo {

namespace {

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
}

void SelectImportedGroup(im2d::CanvasState &state, int artwork_id,
                         int group_id) {
  state.selected_imported_artwork_id = artwork_id;
  state.selected_imported_debug = {im2d::ImportedDebugSelectionKind::Group,
                                   artwork_id, group_id};
}

void SelectImportedPath(im2d::CanvasState &state, int artwork_id, int path_id) {
  state.selected_imported_artwork_id = artwork_id;
  state.selected_imported_debug = {im2d::ImportedDebugSelectionKind::Path,
                                   artwork_id, path_id};
}

bool IsSelectedImportedDebugItem(const im2d::CanvasState &state, int artwork_id,
                                 im2d::ImportedDebugSelectionKind kind,
                                 int item_id) {
  return state.selected_imported_debug.artwork_id == artwork_id &&
         state.selected_imported_debug.kind == kind &&
         state.selected_imported_debug.item_id == item_id;
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
  return summary;
}

void DrawImportedPathNode(im2d::CanvasState &state,
                          const im2d::ImportedArtwork &artwork,
                          const im2d::ImportedPath &path) {
  const ImGuiTreeNodeFlags flags =
      ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
      ImGuiTreeNodeFlags_SpanAvailWidth |
      (IsSelectedImportedDebugItem(
           state, artwork.id, im2d::ImportedDebugSelectionKind::Path, path.id)
           ? ImGuiTreeNodeFlags_Selected
           : 0);
  ImGui::TreeNodeEx(reinterpret_cast<void *>(static_cast<intptr_t>(path.id)),
                    flags, "%s", ImportedPathSummary(path).c_str());
  if (ImGui::IsItemClicked()) {
    SelectImportedPath(state, artwork.id, path.id);
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
  } else {
    for (const im2d::ImportedPath &path : artwork.paths) {
      DrawImportedPathNode(state, artwork, path);
    }
  }

  ImGui::TreePop();
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

  bool lock_scale_ratio = im2d::IsImportedArtworkScaleRatioLocked(*artwork);
  if (ImGui::Checkbox("Lock Scale Ratio", &lock_scale_ratio)) {
    im2d::SetImportedArtworkScaleRatioLocked(*artwork, lock_scale_ratio);
  }

  float scale_x = artwork->scale.x;
  if (ImGui::InputFloat("Scale X", &scale_x, 0.0f, 0.0f, "%.3f")) {
    im2d::UpdateImportedArtworkScaleAxis(*artwork, 0, scale_x);
  }

  float scale_y = artwork->scale.y;
  if (ImGui::InputFloat("Scale Y", &scale_y, 0.0f, 0.0f, "%.3f")) {
    im2d::UpdateImportedArtworkScaleAxis(*artwork, 1, scale_y);
  }

  ImGui::DragFloat2("Adjust Position", &artwork->origin.x, 1.0f, 0.0f, 0.0f,
                    "%.2f");
  float drag_scale_x = artwork->scale.x;
  if (ImGui::DragFloat("Adjust Scale X", &drag_scale_x, 0.01f, 0.01f, 100.0f,
                       "%.3f")) {
    im2d::UpdateImportedArtworkScaleAxis(*artwork, 0, drag_scale_x);
  }

  float drag_scale_y = artwork->scale.y;
  if (ImGui::DragFloat("Adjust Scale Y", &drag_scale_y, 0.01f, 0.01f, 100.0f,
                       "%.3f")) {
    im2d::UpdateImportedArtworkScaleAxis(*artwork, 1, drag_scale_y);
  }

  ImGui::Text("Bounds: %.1f x %.1f",
              artwork->bounds_max.x - artwork->bounds_min.x,
              artwork->bounds_max.y - artwork->bounds_min.y);
  ImGui::Text("Groups: %d",
              std::max(static_cast<int>(artwork->groups.size()) - 1, 0));
  ImGui::Text("Paths: %d", static_cast<int>(artwork->paths.size()));
  DrawImportedDebugTree(state, *artwork);
  ImGui::Separator();

  if (ImGui::Button("Flip Horizontal")) {
    im2d::FlipImportedArtworkHorizontal(state, artwork->id);
  }
  ImGui::SameLine();
  if (ImGui::Button("Flip Vertical")) {
    im2d::FlipImportedArtworkVertical(state, artwork->id);
  }
  if (ImGui::Button("Rotate 90 CW")) {
    im2d::RotateImportedArtworkClockwise(state, artwork->id);
  }
  ImGui::SameLine();
  if (ImGui::Button("Rotate 90 CCW")) {
    im2d::RotateImportedArtworkCounterClockwise(state, artwork->id);
  }
  if (ImGui::Button("Delete")) {
    im2d::DeleteImportedArtwork(state, artwork->id);
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