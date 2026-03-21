#include "demo_imported_artwork_windows.h"

#include "../canvas/im2d_canvas_document.h"

#include <algorithm>

#include <imgui.h>

namespace demo {

namespace {

constexpr float kMinimumImportedArtworkScale = 0.01f;

bool HasAnyDxfArtwork(const im2d::CanvasState &state) {
  return std::any_of(state.imported_artwork.begin(),
                     state.imported_artwork.end(),
                     [](const im2d::ImportedArtwork &artwork) {
                       return artwork.source_format == "DXF";
                     });
}

bool IsScaleRatioLocked(const im2d::ImportedArtwork &artwork) {
  return im2d::HasImportedArtworkFlag(artwork.flags,
                                      im2d::ImportedArtworkFlagLockScaleRatio);
}

void SetScaleRatioLocked(im2d::ImportedArtwork &artwork, bool locked) {
  if (locked) {
    artwork.flags |=
        static_cast<uint32_t>(im2d::ImportedArtworkFlagLockScaleRatio);
    return;
  }

  artwork.flags &=
      ~static_cast<uint32_t>(im2d::ImportedArtworkFlagLockScaleRatio);
}

void UpdateArtworkScaleAxis(im2d::ImportedArtwork &artwork, int axis,
                            float new_value) {
  const float clamped_value = std::max(new_value, kMinimumImportedArtworkScale);
  const bool lock_ratio = IsScaleRatioLocked(artwork);

  if (!lock_ratio) {
    if (axis == 0) {
      artwork.scale.x = clamped_value;
    } else {
      artwork.scale.y = clamped_value;
    }
    return;
  }

  const float old_x = std::max(artwork.scale.x, kMinimumImportedArtworkScale);
  const float old_y = std::max(artwork.scale.y, kMinimumImportedArtworkScale);
  if (axis == 0) {
    const float factor = clamped_value / old_x;
    artwork.scale.x = clamped_value;
    artwork.scale.y = std::max(old_y * factor, kMinimumImportedArtworkScale);
    return;
  }

  const float factor = clamped_value / old_y;
  artwork.scale.y = clamped_value;
  artwork.scale.x = std::max(old_x * factor, kMinimumImportedArtworkScale);
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
      state.selected_imported_artwork_id = artwork.id;
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

  bool lock_scale_ratio = IsScaleRatioLocked(*artwork);
  if (ImGui::Checkbox("Lock Scale Ratio", &lock_scale_ratio)) {
    SetScaleRatioLocked(*artwork, lock_scale_ratio);
  }

  float scale_x = artwork->scale.x;
  if (ImGui::InputFloat("Scale X", &scale_x, 0.0f, 0.0f, "%.3f")) {
    UpdateArtworkScaleAxis(*artwork, 0, scale_x);
  }

  float scale_y = artwork->scale.y;
  if (ImGui::InputFloat("Scale Y", &scale_y, 0.0f, 0.0f, "%.3f")) {
    UpdateArtworkScaleAxis(*artwork, 1, scale_y);
  }

  ImGui::DragFloat2("Adjust Position", &artwork->origin.x, 1.0f, 0.0f, 0.0f,
                    "%.2f");
  float drag_scale_x = artwork->scale.x;
  if (ImGui::DragFloat("Adjust Scale X", &drag_scale_x, 0.01f,
                       kMinimumImportedArtworkScale, 100.0f, "%.3f")) {
    UpdateArtworkScaleAxis(*artwork, 0, drag_scale_x);
  }

  float drag_scale_y = artwork->scale.y;
  if (ImGui::DragFloat("Adjust Scale Y", &drag_scale_y, 0.01f,
                       kMinimumImportedArtworkScale, 100.0f, "%.3f")) {
    UpdateArtworkScaleAxis(*artwork, 1, drag_scale_y);
  }

  ImGui::Text("Bounds: %.1f x %.1f",
              artwork->bounds_max.x - artwork->bounds_min.x,
              artwork->bounds_max.y - artwork->bounds_min.y);
  ImGui::Text("Paths: %d", static_cast<int>(artwork->paths.size()));
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