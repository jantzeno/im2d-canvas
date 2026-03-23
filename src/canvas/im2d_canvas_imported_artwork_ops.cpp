#include "im2d_canvas_imported_artwork_ops.h"

#include "im2d_canvas_document.h"
#include "im2d_canvas_internal.h"

#include "../common/im2d_log.h"
#include "../operations/im2d_operations_shared.h"

#include <clipper2/clipper.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_set>

namespace im2d {

namespace {

constexpr int kClipperDecimalPrecision = 3;
constexpr double kMinimumPreparedContourArea = 0.01;

void SetLastImportedArtworkOperation(CanvasState &state,
                                     ImportedArtworkOperationResult result) {
  state.last_imported_artwork_operation = std::move(result);
}

void SetLastImportedOperationIssueElements(
    CanvasState &state, int artwork_id,
    std::vector<ImportedElementSelection> issue_elements) {
  state.last_imported_operation_issue_artwork_id = artwork_id;
  state.last_imported_operation_issue_elements = std::move(issue_elements);
}

void PopulateOperationReadiness(ImportedArtworkOperationResult *result,
                                const ImportedArtwork &artwork) {
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

bool SameImportedSourceReference(const ImportedSourceReference &a,
                                 const ImportedSourceReference &b) {
  return a.source_artwork_id == b.source_artwork_id && a.kind == b.kind &&
         a.item_id == b.item_id;
}

bool ImportedSourceReferenceLess(const ImportedSourceReference &a,
                                 const ImportedSourceReference &b) {
  if (a.source_artwork_id != b.source_artwork_id) {
    return a.source_artwork_id < b.source_artwork_id;
  }
  if (a.kind != b.kind) {
    return static_cast<int>(a.kind) < static_cast<int>(b.kind);
  }
  return a.item_id < b.item_id;
}

void NormalizeImportedSourceReferences(
    std::vector<ImportedSourceReference> *references) {
  std::sort(references->begin(), references->end(),
            ImportedSourceReferenceLess);
  references->erase(std::unique(references->begin(), references->end(),
                                SameImportedSourceReference),
                    references->end());
}

void ResetImportedArtworkCounters(ImportedArtwork &artwork) {
  int max_group_id = 0;
  int max_path_id = 0;
  int max_text_id = 0;
  for (const ImportedGroup &group : artwork.groups) {
    max_group_id = std::max(max_group_id, group.id);
  }
  for (const ImportedPath &path : artwork.paths) {
    max_path_id = std::max(max_path_id, path.id);
  }
  for (const ImportedDxfText &text : artwork.dxf_text) {
    max_text_id = std::max(max_text_id, text.id);
  }
  artwork.next_group_id = max_group_id + 1;
  artwork.next_path_id = max_path_id + 1;
  artwork.next_dxf_text_id = max_text_id + 1;
}

void CollectAncestorGroupIds(const ImportedArtwork &artwork, int group_id,
                             std::unordered_set<int> *group_ids) {
  int current_group_id = group_id;
  while (current_group_id != 0 && group_ids->insert(current_group_id).second) {
    const ImportedGroup *group = FindImportedGroup(artwork, current_group_id);
    if (group == nullptr) {
      break;
    }
    current_group_id = group->parent_group_id;
  }
}

void FilterGroupReferences(ImportedArtwork &artwork,
                           const std::unordered_set<int> &path_ids,
                           const std::unordered_set<int> &text_ids,
                           const std::unordered_set<int> &group_ids) {
  for (ImportedGroup &group : artwork.groups) {
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

bool MarkRetainedGroups(const ImportedArtwork &artwork, int group_id,
                        std::unordered_set<int> *retained_group_ids) {
  const ImportedGroup *group = FindImportedGroup(artwork, group_id);
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

void PruneEmptyGroups(ImportedArtwork &artwork) {
  std::unordered_set<int> retained_group_ids;
  MarkRetainedGroups(artwork, artwork.root_group_id, &retained_group_ids);
  std::erase_if(artwork.groups,
                [&retained_group_ids](const ImportedGroup &group) {
                  return !retained_group_ids.contains(group.id);
                });

  std::unordered_set<int> retained_path_ids;
  for (const ImportedPath &path : artwork.paths) {
    retained_path_ids.insert(path.id);
  }
  std::unordered_set<int> retained_text_ids;
  for (const ImportedDxfText &text : artwork.dxf_text) {
    retained_text_ids.insert(text.id);
  }
  FilterGroupReferences(artwork, retained_path_ids, retained_text_ids,
                        retained_group_ids);
}

ImportedArtwork BuildArtworkSubset(const ImportedArtwork &source,
                                   const std::unordered_set<int> &path_ids,
                                   const std::unordered_set<int> &text_ids,
                                   const std::string &name_suffix) {
  ImportedArtwork subset;
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

  std::unordered_set<int> required_group_ids;
  required_group_ids.insert(source.root_group_id);
  for (const ImportedPath &path : source.paths) {
    if (path_ids.contains(path.id)) {
      CollectAncestorGroupIds(source, path.parent_group_id,
                              &required_group_ids);
    }
  }
  for (const ImportedDxfText &text : source.dxf_text) {
    if (text_ids.contains(text.id)) {
      CollectAncestorGroupIds(source, text.parent_group_id,
                              &required_group_ids);
    }
  }

  for (const ImportedGroup &group : source.groups) {
    if (!required_group_ids.contains(group.id)) {
      continue;
    }
    ImportedGroup clone = group;
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

  for (const ImportedPath &path : source.paths) {
    if (path_ids.contains(path.id)) {
      subset.paths.push_back(path);
    }
  }
  for (const ImportedDxfText &text : source.dxf_text) {
    if (text_ids.contains(text.id)) {
      subset.dxf_text.push_back(text);
    }
  }

  ResetImportedArtworkCounters(subset);
  RecomputeImportedArtworkBounds(subset);
  RecomputeImportedHierarchyBounds(subset);
  RefreshImportedArtworkPartMetadata(subset);
  return subset;
}

ImportedPath ReverseImportedPathCopy(const ImportedPath &path) {
  ImportedPath reversed = path;
  reversed.segments.clear();
  reversed.segments.reserve(path.segments.size());
  for (auto it = path.segments.rbegin(); it != path.segments.rend(); ++it) {
    ImportedPathSegment segment = *it;
    std::swap(segment.start, segment.end);
    std::swap(segment.control1, segment.control2);
    reversed.segments.push_back(segment);
  }
  return reversed;
}

bool SupportsClipperCleanup(const ImportedPath &path) {
  return !HasImportedPathFlag(path.flags, ImportedPathFlagTextPlaceholder) &&
         !HasImportedPathFlag(path.flags, ImportedPathFlagFilledText) &&
         !HasImportedPathFlag(path.flags, ImportedPathFlagHoleContour);
}

ImportedPath BuildImportedPathFromClipperContour(
    const Clipper2Lib::PathD &path, const ImportedPath &source, int new_path_id,
    std::vector<ImportedSourceReference> provenance, uint32_t issue_flags) {
  ImportedPath imported_path = source;
  imported_path.id = new_path_id;
  imported_path.closed = true;
  imported_path.flags &= ~static_cast<uint32_t>(ImportedPathFlagHoleContour);
  imported_path.issue_flags = issue_flags;
  imported_path.provenance = std::move(provenance);
  NormalizeImportedSourceReferences(&imported_path.provenance);
  imported_path.segments.clear();
  imported_path.segments.reserve(path.size());

  for (size_t index = 0; index < path.size(); ++index) {
    const Clipper2Lib::PointD &start = path[index];
    const Clipper2Lib::PointD &end = path[(index + 1) % path.size()];
    ImportedPathSegment segment;
    segment.kind = ImportedPathSegmentKind::Line;
    segment.start =
        ImVec2(static_cast<float>(start.x), static_cast<float>(start.y));
    segment.end = ImVec2(static_cast<float>(end.x), static_cast<float>(end.y));
    imported_path.segments.push_back(segment);
  }

  return imported_path;
}

struct ClosedCleanupGroup {
  int parent_group_id = 0;
  ImportedPath template_path;
  std::unordered_set<int> source_path_ids;
  std::vector<ImportedSourceReference> provenance;
  Clipper2Lib::PathsD clipper_paths;
  bool requires_clipper_cleanup = false;
};

bool MatchesCleanupGroup(const ClosedCleanupGroup &group,
                         const ImportedPath &path) {
  return group.parent_group_id == path.parent_group_id &&
         group.template_path.flags == path.flags &&
         std::fabs(group.template_path.stroke_width - path.stroke_width) <=
             0.0001f &&
         std::fabs(group.template_path.stroke_color.x - path.stroke_color.x) <=
             0.0001f &&
         std::fabs(group.template_path.stroke_color.y - path.stroke_color.y) <=
             0.0001f &&
         std::fabs(group.template_path.stroke_color.z - path.stroke_color.z) <=
             0.0001f &&
         std::fabs(group.template_path.stroke_color.w - path.stroke_color.w) <=
             0.0001f;
}

ClosedCleanupGroup *
FindOrAddCleanupGroup(std::vector<ClosedCleanupGroup> *groups,
                      const ImportedPath &path) {
  for (ClosedCleanupGroup &group : *groups) {
    if (MatchesCleanupGroup(group, path)) {
      return &group;
    }
  }

  groups->push_back({path.parent_group_id, path, {}, {}, {}, false});
  return &groups->back();
}

bool TryOrientPathsForMerge(const ImportedPath &first,
                            const ImportedPath &second, float tolerance,
                            ImportedPath *merged_first,
                            ImportedPath *merged_second) {
  const std::array<ImportedPath, 2> first_variants = {
      first, ReverseImportedPathCopy(first)};
  const std::array<ImportedPath, 2> second_variants = {
      second, ReverseImportedPathCopy(second)};

  for (const ImportedPath &first_variant : first_variants) {
    for (const ImportedPath &second_variant : second_variants) {
      if (first_variant.segments.empty() || second_variant.segments.empty()) {
        continue;
      }
      if (detail::PointsNear(first_variant.segments.back().end,
                             second_variant.segments.front().start,
                             tolerance)) {
        *merged_first = first_variant;
        *merged_second = second_variant;
        return true;
      }
    }
  }

  return false;
}

ImportedArtworkOperationResult
MoveImportedElementsToNewArtwork(CanvasState &state, int imported_artwork_id,
                                 const std::unordered_set<int> &moved_path_ids,
                                 const std::unordered_set<int> &moved_text_ids,
                                 const std::string &name_suffix,
                                 const std::string &action_verb) {
  ImportedArtworkOperationResult result;
  result.artwork_id = imported_artwork_id;

  ImportedArtwork *artwork = FindImportedArtwork(state, imported_artwork_id);
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

  ImportedArtwork subset =
      BuildArtworkSubset(*artwork, moved_path_ids, moved_text_ids, name_suffix);

  std::erase_if(artwork->paths, [&moved_path_ids](const ImportedPath &path) {
    return moved_path_ids.contains(path.id);
  });
  std::erase_if(artwork->dxf_text,
                [&moved_text_ids](const ImportedDxfText &text) {
                  return moved_text_ids.contains(text.id);
                });

  PruneEmptyGroups(*artwork);
  ResetImportedArtworkCounters(*artwork);
  RecomputeImportedArtworkBounds(*artwork);
  RecomputeImportedHierarchyBounds(*artwork);
  RefreshImportedArtworkPartMetadata(*artwork);

  const bool source_empty = artwork->paths.empty() && artwork->dxf_text.empty();
  if (!source_empty) {
    artwork->part.part_id = state.next_imported_part_id++;
  }
  const int created_artwork_id =
      AppendImportedArtwork(state, std::move(subset));
  if (source_empty) {
    DeleteImportedArtwork(state, imported_artwork_id);
  }

  ClearSelectedImportedElements(state);
  state.selected_imported_artwork_id = created_artwork_id;
  state.selected_imported_debug = {ImportedDebugSelectionKind::Artwork,
                                   created_artwork_id, 0};

  result.success = true;
  result.created_artwork_id = created_artwork_id;
  if (const ImportedArtwork *created_artwork =
          FindImportedArtwork(state, created_artwork_id);
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

template <typename Function>
bool TransformImportedArtwork(CanvasState &state, int imported_artwork_id,
                              Function &&transform, const char *action_name) {
  ImportedArtwork *artwork = FindImportedArtwork(state, imported_artwork_id);
  if (artwork == nullptr) {
    return false;
  }

  const ImVec2 size = detail::ImportedArtworkLocalSize(*artwork);
  const ImVec2 original_scaled_size =
      detail::ImportedArtworkScaledSize(*artwork);
  const ImVec2 world_center(artwork->origin.x + original_scaled_size.x * 0.5f,
                            artwork->origin.y + original_scaled_size.y * 0.5f);

  detail::ForEachImportedArtworkPoint(*artwork, [&](ImVec2 &point) {
    const ImVec2 local(point.x - artwork->bounds_min.x,
                       point.y - artwork->bounds_min.y);
    point = transform(local, size);
  });

  RecomputeImportedArtworkBounds(*artwork);
  RecomputeImportedHierarchyBounds(*artwork);
  const ImVec2 new_scaled_size = detail::ImportedArtworkScaledSize(*artwork);
  artwork->origin = ImVec2(world_center.x - new_scaled_size.x * 0.5f,
                           world_center.y - new_scaled_size.y * 0.5f);

  log::GetLogger()->info("{} imported artwork id={} name='{}'", action_name,
                         artwork->id, artwork->name);
  return true;
}

} // namespace

bool IsImportedArtworkScaleRatioLocked(const ImportedArtwork &artwork) {
  return HasImportedArtworkFlag(artwork.flags,
                                ImportedArtworkFlagLockScaleRatio);
}

void UpdateImportedArtworkScaleFromTarget(ImportedArtwork &artwork,
                                          const ImVec2 &target_scale) {
  operations::detail::UpdateImportedArtworkScaleFromTargetShared(artwork,
                                                                 target_scale);
}

bool FlipImportedArtworkHorizontal(CanvasState &state,
                                   int imported_artwork_id) {
  return TransformImportedArtwork(
      state, imported_artwork_id,
      [](const ImVec2 &local, const ImVec2 &size) {
        return ImVec2(size.x - local.x, local.y);
      },
      "Flipped horizontally");
}

bool FlipImportedArtworkVertical(CanvasState &state, int imported_artwork_id) {
  return TransformImportedArtwork(
      state, imported_artwork_id,
      [](const ImVec2 &local, const ImVec2 &size) {
        return ImVec2(local.x, size.y - local.y);
      },
      "Flipped vertically");
}

bool RotateImportedArtworkClockwise(CanvasState &state,
                                    int imported_artwork_id) {
  return TransformImportedArtwork(
      state, imported_artwork_id,
      [](const ImVec2 &local, const ImVec2 &size) {
        const ImVec2 center(size.x * 0.5f, size.y * 0.5f);
        const ImVec2 delta(local.x - center.x, local.y - center.y);
        return ImVec2(center.x + delta.y, center.y - delta.x);
      },
      "Rotated 90 CW");
}

bool RotateImportedArtworkCounterClockwise(CanvasState &state,
                                           int imported_artwork_id) {
  return TransformImportedArtwork(
      state, imported_artwork_id,
      [](const ImVec2 &local, const ImVec2 &size) {
        const ImVec2 center(size.x * 0.5f, size.y * 0.5f);
        const ImVec2 delta(local.x - center.x, local.y - center.y);
        return ImVec2(center.x - delta.y, center.y + delta.x);
      },
      "Rotated 90 CCW");
}

ImportedArtworkOperationResult
PrepareImportedArtworkForCutting(CanvasState &state, int imported_artwork_id,
                                 float weld_tolerance) {
  return operations::detail::PrepareImportedArtworkForCuttingShared(
      state, imported_artwork_id, weld_tolerance,
      [](ImportedArtwork &artwork, float callback_weld_tolerance,
         ImportedArtworkOperationResult *result) {
        std::unordered_set<int> newly_closed_path_ids;
        for (ImportedPath &path : artwork.paths) {
          if (path.segments.empty() || path.closed) {
            continue;
          }
          if (detail::PointsNear(path.segments.front().start,
                                 path.segments.back().end,
                                 callback_weld_tolerance)) {
            path.segments.back().end = path.segments.front().start;
            path.closed = true;
            newly_closed_path_ids.insert(path.id);
          }
        }

        bool merged_any = true;
        while (merged_any) {
          merged_any = false;
          for (size_t first_index = 0; first_index < artwork.paths.size();
               ++first_index) {
            ImportedPath &first_path = artwork.paths[first_index];
            if (first_path.closed || first_path.segments.empty()) {
              continue;
            }

            for (size_t second_index = first_index + 1;
                 second_index < artwork.paths.size(); ++second_index) {
              ImportedPath &second_path = artwork.paths[second_index];
              if (second_path.closed || second_path.segments.empty() ||
                  second_path.parent_group_id != first_path.parent_group_id) {
                continue;
              }

              ImportedPath oriented_first;
              ImportedPath oriented_second;
              if (!TryOrientPathsForMerge(first_path, second_path,
                                          callback_weld_tolerance,
                                          &oriented_first, &oriented_second)) {
                continue;
              }

              oriented_second.segments.front().start =
                  oriented_first.segments.back().end;
              oriented_first.segments.insert(oriented_first.segments.end(),
                                             oriented_second.segments.begin(),
                                             oriented_second.segments.end());
              if (detail::PointsNear(oriented_first.segments.front().start,
                                     oriented_first.segments.back().end,
                                     callback_weld_tolerance)) {
                oriented_first.segments.back().end =
                    oriented_first.segments.front().start;
                oriented_first.closed = true;
                newly_closed_path_ids.insert(oriented_first.id);
              }

              const int removed_path_id = second_path.id;
              artwork.paths[first_index] = std::move(oriented_first);
              artwork.paths.erase(artwork.paths.begin() +
                                  static_cast<std::ptrdiff_t>(second_index));
              for (ImportedGroup &group : artwork.groups) {
                std::erase(group.path_ids, removed_path_id);
              }
              result->stitched_count += 1;
              merged_any = true;
              break;
            }

            if (merged_any) {
              break;
            }
          }
        }

        std::vector<ClosedCleanupGroup> cleanup_groups;
        for (const ImportedPath &path : artwork.paths) {
          if (!path.closed || path.segments.empty() ||
              !SupportsClipperCleanup(path)) {
            continue;
          }

          Clipper2Lib::PathD clipper_path =
              detail::SampleImportedPathToClipperPath(path);
          if (clipper_path.size() < 3) {
            continue;
          }

          ClosedCleanupGroup *group =
              FindOrAddCleanupGroup(&cleanup_groups, path);
          group->source_path_ids.insert(path.id);
          group->provenance.insert(group->provenance.end(),
                                   path.provenance.begin(),
                                   path.provenance.end());
          group->clipper_paths.push_back(std::move(clipper_path));
          if (newly_closed_path_ids.contains(path.id)) {
            group->requires_clipper_cleanup = true;
          }
        }

        std::unordered_set<int> replaced_path_ids;
        std::vector<ImportedPath> cleaned_paths;
        for (ClosedCleanupGroup &group : cleanup_groups) {
          if (!group.requires_clipper_cleanup &&
              group.clipper_paths.size() < 2) {
            continue;
          }

          Clipper2Lib::PathsD prepared_paths = group.clipper_paths;
          if (callback_weld_tolerance > 0.0f) {
            Clipper2Lib::PathsD inflated = Clipper2Lib::InflatePaths(
                prepared_paths,
                static_cast<double>(callback_weld_tolerance) * 0.5,
                Clipper2Lib::JoinType::Round, Clipper2Lib::EndType::Polygon,
                2.0, kClipperDecimalPrecision);
            if (!inflated.empty()) {
              Clipper2Lib::PathsD deflated = Clipper2Lib::InflatePaths(
                  inflated, static_cast<double>(-callback_weld_tolerance) * 0.5,
                  Clipper2Lib::JoinType::Round, Clipper2Lib::EndType::Polygon,
                  2.0, kClipperDecimalPrecision);
              if (!deflated.empty()) {
                prepared_paths = std::move(deflated);
              }
            }
          }

          Clipper2Lib::PathsD unified_paths =
              Clipper2Lib::Union(prepared_paths, Clipper2Lib::FillRule::NonZero,
                                 kClipperDecimalPrecision);
          if (unified_paths.empty()) {
            continue;
          }

          std::vector<ImportedPath> group_cleaned_paths;
          group_cleaned_paths.reserve(unified_paths.size());
          NormalizeImportedSourceReferences(&group.provenance);
          const bool ambiguous_cleanup =
              group.source_path_ids.size() > 1 ||
              unified_paths.size() != group.source_path_ids.size();
          const uint32_t output_issue_flags =
              ambiguous_cleanup ? static_cast<uint32_t>(
                                      ImportedElementIssueFlagAmbiguousCleanup)
                                : ImportedElementIssueFlagNone;
          for (const Clipper2Lib::PathD &unified_path : unified_paths) {
            if (unified_path.size() < 3 ||
                std::fabs(Clipper2Lib::Area(unified_path)) <
                    kMinimumPreparedContourArea) {
              continue;
            }

            group_cleaned_paths.push_back(BuildImportedPathFromClipperContour(
                unified_path, group.template_path, artwork.next_path_id++,
                group.provenance, output_issue_flags));
          }

          if (group_cleaned_paths.empty()) {
            continue;
          }

          for (int source_path_id : group.source_path_ids) {
            replaced_path_ids.insert(source_path_id);
          }
          result->cleaned_count += static_cast<int>(group_cleaned_paths.size());
          if (ambiguous_cleanup) {
            result->ambiguous_count +=
                static_cast<int>(group_cleaned_paths.size());
          }
          std::move(group_cleaned_paths.begin(), group_cleaned_paths.end(),
                    std::back_inserter(cleaned_paths));
        }

        if (!replaced_path_ids.empty()) {
          std::erase_if(artwork.paths,
                        [&replaced_path_ids](const ImportedPath &path) {
                          return replaced_path_ids.contains(path.id);
                        });

          for (ImportedGroup &group : artwork.groups) {
            std::erase_if(group.path_ids, [&replaced_path_ids](int path_id) {
              return replaced_path_ids.contains(path_id);
            });
          }

          for (ImportedPath &cleaned_path : cleaned_paths) {
            if (ImportedGroup *group =
                    FindImportedGroup(artwork, cleaned_path.parent_group_id);
                group != nullptr) {
              group->path_ids.push_back(cleaned_path.id);
            }
            artwork.paths.push_back(std::move(cleaned_path));
          }
        }
      },
      [](const ImportedArtwork &artwork,
         ImportedArtworkOperationResult *result) {
        result->closed_count = 0;
        result->open_count = 0;
        for (const ImportedPath &path : artwork.paths) {
          if (path.closed) {
            result->closed_count += 1;
          } else {
            result->open_count += 1;
          }
        }
      },
      [](ImportedArtworkOperationResult *result,
         const ImportedArtwork &artwork) {
        PopulateOperationReadiness(result, artwork);
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

ImportedArtworkOperationResult SelectImportedElementsInWorldRect(
    CanvasState &state, int imported_artwork_id, const ImVec2 &world_start,
    const ImVec2 &world_end, ImportedArtworkEditMode mode) {
  ImportedArtworkOperationResult result;
  result.artwork_id = imported_artwork_id;

  ImportedArtwork *artwork = FindImportedArtwork(state, imported_artwork_id);
  if (artwork == nullptr) {
    result.message = "Imported artwork was not found.";
    SetLastImportedOperationIssueElements(state, 0, {});
    SetLastImportedArtworkOperation(state, result);
    return result;
  }
  if (mode == ImportedArtworkEditMode::None) {
    result.message = "Imported artwork selection mode is not active.";
    SetLastImportedOperationIssueElements(state, 0, {});
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  const detail::SelectionRect selection_rect =
      detail::NormalizeRect(world_start, world_end);
  ClearSelectedImportedElements(state);
  std::vector<ImportedElementSelection> skipped_elements;

  std::vector<ImVec2> sample_points;
  for (const ImportedPath &path : artwork->paths) {
    sample_points.clear();
    detail::AppendPathSamplePointsWorld(*artwork, path, &sample_points);
    if (sample_points.empty()) {
      continue;
    }

    int inside_count = 0;
    for (const ImVec2 &point : sample_points) {
      inside_count +=
          detail::PointInsideSelection(selection_rect, mode, point) ? 1 : 0;
    }
    if (inside_count == static_cast<int>(sample_points.size())) {
      state.selected_imported_elements.push_back(
          {ImportedElementKind::Path, path.id});
    } else if (inside_count != 0) {
      result.skipped_count += 1;
      skipped_elements.push_back({ImportedElementKind::Path, path.id});
    }
  }

  for (const ImportedDxfText &text : artwork->dxf_text) {
    sample_points.clear();
    detail::AppendTextSamplePointsWorld(*artwork, text, &sample_points);
    if (sample_points.empty()) {
      continue;
    }

    int inside_count = 0;
    for (const ImVec2 &point : sample_points) {
      inside_count +=
          detail::PointInsideSelection(selection_rect, mode, point) ? 1 : 0;
    }
    if (inside_count == static_cast<int>(sample_points.size())) {
      state.selected_imported_elements.push_back(
          {ImportedElementKind::DxfText, text.id});
    } else if (inside_count != 0) {
      result.skipped_count += 1;
      skipped_elements.push_back({ImportedElementKind::DxfText, text.id});
    }
  }

  result.selected_count =
      static_cast<int>(state.selected_imported_elements.size());
  result.success = result.selected_count > 0;
  PopulateOperationReadiness(&result, *artwork);
  result.message =
      "Selected " + std::to_string(result.selected_count) +
      " enclosed imported element" +
      (result.selected_count == 1 ? std::string() : std::string("s")) + ".";
  if (result.skipped_count > 0) {
    result.message +=
        " Skipped " + std::to_string(result.skipped_count) +
        " crossing element" +
        (result.skipped_count == 1 ? std::string() : std::string("s")) + ".";
  }
  SetLastImportedOperationIssueElements(state, artwork->id,
                                        std::move(skipped_elements));
  SetLastImportedArtworkOperation(state, result);
  return result;
}

ImportedArtworkOperationResult
ExtractSelectedImportedElements(CanvasState &state, int imported_artwork_id) {
  return operations::detail::ExtractSelectedImportedElementsShared(
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
SeparateImportedArtworkByGuide(CanvasState &state, int imported_artwork_id,
                               int guide_id) {
  return operations::detail::SeparateImportedArtworkByGuideShared(
      state, imported_artwork_id, guide_id,
      [](CanvasState &callback_state, int callback_artwork_id,
         const std::unordered_set<int> &path_ids,
         const std::unordered_set<int> &text_ids,
         const std::string &name_suffix, const std::string &action_verb) {
        return MoveImportedElementsToNewArtwork(
            callback_state, callback_artwork_id, path_ids, text_ids,
            name_suffix, action_verb);
      },
      [](ImportedArtworkOperationResult *result,
         const ImportedArtwork &artwork) {
        PopulateOperationReadiness(result, artwork);
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

bool DeleteImportedArtwork(CanvasState &state, int imported_artwork_id) {
  return operations::detail::DeleteImportedArtworkShared(state,
                                                         imported_artwork_id);
}

} // namespace im2d