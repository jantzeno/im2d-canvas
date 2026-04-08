#include "im2d_operations.h"

#include "im2d_operations_shared.h"

#include "../canvas/im2d_canvas_document.h"
#include "../canvas/im2d_canvas_imported_artwork_ops.h"

#include <algorithm>
#include <unordered_set>

namespace {

constexpr float kMinimumImportedArtworkScale = 0.01f;

void PopulateOperationReadiness(im2d::ImportedArtworkOperationResult *result,
                                const im2d::ImportedArtwork &artwork) {
  result->part_id = artwork.part.part_id;
  result->ambiguous_count = artwork.part.ambiguous_contour_count;
  result->outer_count = artwork.part.outer_contour_count;
  result->hole_count = artwork.part.hole_contour_count;
  result->island_count = artwork.part.island_count;
  result->attached_hole_count = artwork.part.attached_hole_count;
  result->orphan_hole_count = artwork.part.orphan_hole_count;
  result->closed_count = artwork.part.closed_contour_count;
  result->open_count = artwork.part.open_contour_count;
  result->placeholder_count = artwork.part.placeholder_count;
  result->cut_ready = artwork.part.cut_ready;
  result->nest_ready = artwork.part.nest_ready;
}

void SetLastImportedArtworkOperation(
    im2d::CanvasState &state, im2d::ImportedArtworkOperationResult result) {
  state.last_imported_artwork_operation = std::move(result);
}

void SetLastImportedOperationIssueElements(
    im2d::CanvasState &state, int artwork_id,
    std::vector<im2d::ImportedElementSelection> issue_elements,
    const bool highlight_on_canvas = true) {
  state.last_imported_operation_issue_artwork_id = artwork_id;
  state.last_imported_operation_issue_elements = std::move(issue_elements);
  state.highlight_last_imported_operation_issue_elements =
      highlight_on_canvas && artwork_id != 0 &&
      !state.last_imported_operation_issue_elements.empty();
}

void ResetImportedArtworkCounters(im2d::ImportedArtwork &artwork) {
  int max_group_id = 0;
  int max_path_id = 0;
  int max_text_id = 0;
  for (const im2d::ImportedGroup &group : artwork.groups) {
    max_group_id = std::max(max_group_id, group.id);
  }
  for (const im2d::ImportedPath &path : artwork.paths) {
    max_path_id = std::max(max_path_id, path.id);
  }
  for (const im2d::ImportedDxfText &text : artwork.dxf_text) {
    max_text_id = std::max(max_text_id, text.id);
  }
  artwork.next_group_id = max_group_id + 1;
  artwork.next_path_id = max_path_id + 1;
  artwork.next_dxf_text_id = max_text_id + 1;
}

void CollectAncestorGroupIds(const im2d::ImportedArtwork &artwork, int group_id,
                             std::unordered_set<int> *group_ids) {
  int current_group_id = group_id;
  while (current_group_id != 0 && group_ids->insert(current_group_id).second) {
    const im2d::ImportedGroup *group =
        im2d::FindImportedGroup(artwork, current_group_id);
    if (group == nullptr) {
      break;
    }
    current_group_id = group->parent_group_id;
  }
}

void FilterGroupReferences(im2d::ImportedArtwork &artwork,
                           const std::unordered_set<int> &path_ids,
                           const std::unordered_set<int> &text_ids,
                           const std::unordered_set<int> &group_ids) {
  for (im2d::ImportedGroup &group : artwork.groups) {
    std::erase_if(group.child_group_ids, [&group_ids](int child_group_id) {
      return !group_ids.contains(child_group_id);
    });
    std::erase_if(group.path_ids, [&path_ids](int path_id) {
      return !path_ids.contains(path_id);
    });
    std::erase_if(group.dxf_text_ids, [&text_ids](int text_id) {
      return !text_ids.contains(text_id);
    });
  }
}

bool MarkRetainedGroups(const im2d::ImportedArtwork &artwork, int group_id,
                        std::unordered_set<int> *retained_group_ids) {
  const im2d::ImportedGroup *group = im2d::FindImportedGroup(artwork, group_id);
  if (group == nullptr) {
    return false;
  }

  bool keep = !group->path_ids.empty() || !group->dxf_text_ids.empty();
  for (const int child_group_id : group->child_group_ids) {
    keep =
        MarkRetainedGroups(artwork, child_group_id, retained_group_ids) || keep;
  }

  if (group_id == artwork.root_group_id) {
    keep = true;
  }
  if (keep) {
    retained_group_ids->insert(group_id);
  }
  return keep;
}

void PruneEmptyGroups(im2d::ImportedArtwork &artwork) {
  std::unordered_set<int> retained_group_ids;
  MarkRetainedGroups(artwork, artwork.root_group_id, &retained_group_ids);
  std::erase_if(artwork.groups,
                [&retained_group_ids](const im2d::ImportedGroup &group) {
                  return !retained_group_ids.contains(group.id);
                });

  std::unordered_set<int> retained_path_ids;
  for (const im2d::ImportedPath &path : artwork.paths) {
    retained_path_ids.insert(path.id);
  }
  std::unordered_set<int> retained_text_ids;
  for (const im2d::ImportedDxfText &text : artwork.dxf_text) {
    retained_text_ids.insert(text.id);
  }
  FilterGroupReferences(artwork, retained_path_ids, retained_text_ids,
                        retained_group_ids);
}

im2d::ImportedArtwork
BuildArtworkSubset(const im2d::ImportedArtwork &source,
                   const std::unordered_set<int> &path_ids,
                   const std::unordered_set<int> &text_ids,
                   const std::string &name_suffix) {
  im2d::ImportedArtwork subset;
  subset.name = source.name + name_suffix;
  subset.source_path = source.source_path;
  subset.source_format = source.source_format;
  subset.origin = source.origin;
  subset.scale = source.scale;
  subset.root_group_id = source.root_group_id;
  subset.part = source.part;
  subset.part.part_id = 0;
  subset.visible = source.visible;
  subset.flags = source.flags;
  bool has_world_bounds = false;
  ImVec2 subset_world_min(0.0f, 0.0f);

  const auto include_world_bounds = [&](const ImVec2 &local_min,
                                        const ImVec2 &local_max) {
    ImVec2 world_min;
    ImVec2 world_max;
    im2d::ImportedLocalBoundsToWorldBounds(source, local_min, local_max,
                                           &world_min, &world_max);
    if (!has_world_bounds) {
      subset_world_min = world_min;
      has_world_bounds = true;
      return;
    }
    subset_world_min.x = std::min(subset_world_min.x, world_min.x);
    subset_world_min.y = std::min(subset_world_min.y, world_min.y);
  };

  std::unordered_set<int> required_group_ids;
  required_group_ids.insert(source.root_group_id);
  for (const im2d::ImportedPath &path : source.paths) {
    if (path_ids.contains(path.id)) {
      CollectAncestorGroupIds(source, path.parent_group_id,
                              &required_group_ids);
    }
  }
  for (const im2d::ImportedDxfText &text : source.dxf_text) {
    if (text_ids.contains(text.id)) {
      CollectAncestorGroupIds(source, text.parent_group_id,
                              &required_group_ids);
    }
  }

  for (const im2d::ImportedGroup &group : source.groups) {
    if (!required_group_ids.contains(group.id)) {
      continue;
    }
    im2d::ImportedGroup clone = group;
    std::erase_if(clone.child_group_ids,
                  [&required_group_ids](int child_group_id) {
                    return !required_group_ids.contains(child_group_id);
                  });
    std::erase_if(clone.path_ids, [&path_ids](int path_id) {
      return !path_ids.contains(path_id);
    });
    std::erase_if(clone.dxf_text_ids, [&text_ids](int text_id) {
      return !text_ids.contains(text_id);
    });
    subset.groups.push_back(std::move(clone));
  }

  for (const im2d::ImportedPath &path : source.paths) {
    if (path_ids.contains(path.id)) {
      subset.paths.push_back(path);
      include_world_bounds(path.bounds_min, path.bounds_max);
    }
  }
  for (const im2d::ImportedDxfText &text : source.dxf_text) {
    if (text_ids.contains(text.id)) {
      subset.dxf_text.push_back(text);
      include_world_bounds(text.bounds_min, text.bounds_max);
    }
  }

  ResetImportedArtworkCounters(subset);
  im2d::RecomputeImportedArtworkBounds(subset);
  im2d::RecomputeImportedHierarchyBounds(subset);
  im2d::RefreshImportedArtworkPartMetadata(subset);
  if (has_world_bounds) {
    subset.origin = subset_world_min;
  }
  return subset;
}

im2d::ImportedArtworkOperationResult MoveImportedElementsToNewArtwork(
    im2d::CanvasState &state, int imported_artwork_id,
    const std::unordered_set<int> &moved_path_ids,
    const std::unordered_set<int> &moved_text_ids,
    const std::string &name_suffix, const std::string &action_verb) {
  im2d::ImportedArtworkOperationResult result;
  result.artwork_id = imported_artwork_id;

  im2d::ImportedArtwork *artwork =
      im2d::FindImportedArtwork(state, imported_artwork_id);
  if (artwork == nullptr) {
    result.message = "Imported artwork was not found.";
    SetLastImportedOperationIssueElements(state, 0, {});
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  SetLastImportedOperationIssueElements(state, 0, {});

  const int moved_count =
      static_cast<int>(moved_path_ids.size() + moved_text_ids.size());
  result.moved_count = moved_count;
  if (moved_count == 0) {
    result.message = "No imported elements were eligible for extraction.";
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  im2d::ImportedArtwork subset =
      BuildArtworkSubset(*artwork, moved_path_ids, moved_text_ids, name_suffix);

  std::erase_if(artwork->paths,
                [&moved_path_ids](const im2d::ImportedPath &path) {
                  return moved_path_ids.contains(path.id);
                });
  std::erase_if(artwork->dxf_text,
                [&moved_text_ids](const im2d::ImportedDxfText &text) {
                  return moved_text_ids.contains(text.id);
                });

  PruneEmptyGroups(*artwork);
  ResetImportedArtworkCounters(*artwork);
  im2d::RecomputeImportedArtworkBounds(*artwork);
  im2d::RecomputeImportedHierarchyBounds(*artwork);
  im2d::RefreshImportedArtworkPartMetadata(*artwork);

  const bool source_empty = artwork->paths.empty() && artwork->dxf_text.empty();
  if (!source_empty) {
    artwork->part.part_id = state.next_imported_part_id++;
  }
  const int created_artwork_id =
      im2d::AppendImportedArtwork(state, std::move(subset), false);
  if (source_empty) {
    im2d::DeleteImportedArtwork(state, imported_artwork_id);
  }

  im2d::ClearSelectedImportedElements(state);
  im2d::SetSingleSelectedImportedArtworkObject(state, created_artwork_id);
  state.selected_imported_debug = {im2d::ImportedDebugSelectionKind::Artwork,
                                   created_artwork_id, 0};

  result.success = true;
  result.created_artwork_id = created_artwork_id;
  if (const im2d::ImportedArtwork *created_artwork =
          im2d::FindImportedArtwork(state, created_artwork_id);
      created_artwork != nullptr) {
    PopulateOperationReadiness(&result, *created_artwork);
  }
  result.message = action_verb + " " + std::to_string(moved_count) +
                   " imported element" +
                   (moved_count == 1 ? std::string() : std::string("s")) +
                   " into a new artwork.";
  SetLastImportedArtworkOperation(state, result);
  return result;
}

} // namespace

namespace im2d::operations {

bool IsImportedArtworkScaleRatioLocked(const ImportedArtwork &artwork) {
  return HasImportedArtworkFlag(artwork.flags,
                                ImportedArtworkFlagLockScaleRatio);
}

void SetImportedArtworkScaleRatioLocked(ImportedArtwork &artwork, bool locked) {
  if (locked) {
    artwork.flags |= static_cast<uint32_t>(ImportedArtworkFlagLockScaleRatio);
    return;
  }

  artwork.flags &= ~static_cast<uint32_t>(ImportedArtworkFlagLockScaleRatio);
}

void UpdateImportedArtworkScaleAxis(ImportedArtwork &artwork, int axis,
                                    float new_value) {
  const float clamped_value = std::max(new_value, kMinimumImportedArtworkScale);
  const bool lock_ratio =
      operations::IsImportedArtworkScaleRatioLocked(artwork);

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

void UpdateImportedArtworkScaleFromTarget(ImportedArtwork &artwork,
                                          const ImVec2 &target_scale) {
  detail::UpdateImportedArtworkScaleFromTargetShared(artwork, target_scale);
}

bool FlipImportedArtworkHorizontal(CanvasState &state,
                                   int imported_artwork_id) {
  return im2d::FlipImportedArtworkHorizontal(state, imported_artwork_id);
}

bool FlipImportedArtworkVertical(CanvasState &state, int imported_artwork_id) {
  return im2d::FlipImportedArtworkVertical(state, imported_artwork_id);
}

bool RotateImportedArtworkClockwise(CanvasState &state,
                                    int imported_artwork_id) {
  return im2d::RotateImportedArtworkClockwise(state, imported_artwork_id);
}

bool RotateImportedArtworkCounterClockwise(CanvasState &state,
                                           int imported_artwork_id) {
  return im2d::RotateImportedArtworkCounterClockwise(state,
                                                     imported_artwork_id);
}

ImportedArtworkOperationResult
UpdateImportedArtworkOutlineColor(CanvasState &state, int imported_artwork_id,
                                  const ImVec4 &stroke_color) {
  ImportedArtworkOperationResult result;
  result.artwork_id = imported_artwork_id;

  ImportedArtwork *artwork =
      im2d::FindImportedArtwork(state, imported_artwork_id);
  if (artwork == nullptr) {
    result.message = "Imported artwork was not found.";
    SetLastImportedOperationIssueElements(state, 0, {});
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  SetLastImportedOperationIssueElements(state, 0, {});

  for (ImportedPath &path : artwork->paths) {
    path.stroke_color = stroke_color;
  }
  for (ImportedDxfText &text : artwork->dxf_text) {
    text.stroke_color = stroke_color;
  }

  result.success = true;
  PopulateOperationReadiness(&result, *artwork);
  result.message = "Updated outline color for the selected imported artwork.";
  SetLastImportedArtworkOperation(state, result);
  return result;
}

ImportedArtworkOperationResult
ExtractSelectedImportedElements(CanvasState &state, int imported_artwork_id) {
  return detail::ExtractSelectedImportedElementsShared(
      state, imported_artwork_id,
      [](CanvasState &callback_state, int callback_artwork_id,
         const std::unordered_set<int> &path_ids,
         const std::unordered_set<int> &text_ids,
         const std::string &name_suffix, const std::string &action_verb) {
        return MoveImportedElementsToNewArtwork(
            callback_state, callback_artwork_id, path_ids, text_ids,
            name_suffix, action_verb);
      },
      [](CanvasState &callback_state, int artwork_id,
         std::vector<ImportedElementSelection> issue_elements) {
        SetLastImportedOperationIssueElements(callback_state, artwork_id,
                                              std::move(issue_elements));
      },
      [](CanvasState &callback_state,
         ImportedArtworkOperationResult callback_result) {
        SetLastImportedArtworkOperation(callback_state,
                                        std::move(callback_result));
      });
}

ImportedArtworkOperationResult
GroupSelectedImportedElements(CanvasState &state, int imported_artwork_id) {
  return im2d::GroupSelectedImportedElements(state, imported_artwork_id);
}

ImportedArtworkOperationResult
GroupImportedArtworkRootContents(CanvasState &state, int imported_artwork_id) {
  return im2d::GroupImportedArtworkRootContents(state, imported_artwork_id);
}

ImportedArtworkOperationResult
GroupSelectedImportedArtworkObjects(CanvasState &state) {
  return im2d::GroupSelectedImportedArtworkObjects(state);
}

ImportedArtworkOperationResult
UngroupSelectedImportedArtworkObjects(CanvasState &state) {
  return im2d::UngroupSelectedImportedArtworkObjects(state);
}

ImportedArtworkOperationResult
UngroupSelectedImportedGroup(CanvasState &state, int imported_artwork_id) {
  return im2d::UngroupSelectedImportedGroup(state, imported_artwork_id);
}

ImportedArtworkOperationResult
PreviewSeparateImportedArtworkByGuide(CanvasState &state,
                                      int imported_artwork_id, int guide_id) {
  return im2d::PreviewSeparateImportedArtworkByGuide(state, imported_artwork_id,
                                                     guide_id);
}

void ClearImportedArtworkSeparationPreview(CanvasState &state) {
  im2d::ClearImportedArtworkSeparationPreview(state);
}

ImportedArtworkOperationResult
PreviewImportedArtworkAutoCut(CanvasState &state, int imported_artwork_id,
                              AutoCutPreviewAxisMode axis_mode,
                              float minimum_gap) {
  return im2d::PreviewImportedArtworkAutoCut(state, imported_artwork_id,
                                             axis_mode, minimum_gap);
}

ImportedArtworkOperationResult
ApplyImportedArtworkAutoCut(CanvasState &state, int imported_artwork_id,
                            AutoCutPreviewAxisMode axis_mode, float minimum_gap,
                            bool create_groups_from_cuts) {
  return im2d::ApplyImportedArtworkAutoCut(state, imported_artwork_id,
                                           axis_mode, minimum_gap,
                                           create_groups_from_cuts);
}

void ClearImportedArtworkAutoCutPreview(CanvasState &state) {
  im2d::ClearImportedArtworkAutoCutPreview(state);
}

ImportedArtworkOperationResult
SeparateImportedArtworkByGuide(CanvasState &state, int imported_artwork_id,
                               int guide_id, bool create_groups_from_cuts) {
  return im2d::SeparateImportedArtworkByGuide(
      state, imported_artwork_id, guide_id, create_groups_from_cuts);
}

bool DeleteImportedArtwork(CanvasState &state, int imported_artwork_id) {
  return detail::DeleteImportedArtworkShared(state, imported_artwork_id);
}

} // namespace im2d::operations