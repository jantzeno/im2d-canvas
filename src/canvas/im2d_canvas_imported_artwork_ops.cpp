#include "im2d_canvas_imported_artwork_ops.h"

#include "im2d_canvas_artwork_ops_internal.h"
#include "im2d_canvas_document.h"
#include "im2d_canvas_internal.h"
#include "im2d_canvas_notification.h"
#include "im2d_canvas_undo.h"

#include "../common/im2d_log.h"
#include "../common/im2d_numeric_constants.h"
#include "../common/im2d_vector_math.h"
#include "../operations/im2d_operations_shared.h"

#include <clipper2/clipper.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace im2d {

namespace detail {

constexpr int kClipperDecimalPrecision = 3;

bool ImportedArtworkTraceEnabled() {
  return std::getenv("IM2D_REGRESSION_TRACE") != nullptr;
}

void TraceImportedArtworkStep(const std::string &message) {
  if (!ImportedArtworkTraceEnabled()) {
    return;
  }
  std::cout << "[TRACE] " << message << std::endl;
}

void SetLastImportedArtworkOperation(CanvasState &state,
                                     ImportedArtworkOperationResult result) {
  state.last_imported_artwork_operation = std::move(result);
}

void SetLastImportedOperationIssueElements(
    CanvasState &state, int artwork_id,
    std::vector<ImportedElementSelection> issue_elements,
    const bool highlight_on_canvas) {
  state.last_imported_operation_issue_artwork_id = artwork_id;
  state.last_imported_operation_issue_elements = std::move(issue_elements);
  state.highlight_last_imported_operation_issue_elements =
      highlight_on_canvas && artwork_id != 0 &&
      !state.last_imported_operation_issue_elements.empty();
}

void ClearImportedArtworkSeparationPreviewState(CanvasState &state) {
  state.imported_artwork_separation_preview = {};
}

void ClearImportedArtworkAutoCutPreviewState(CanvasState &state) {
  state.imported_artwork_auto_cut_preview = {};
}

void ClearImportedArtworkPreviewStatesForArtwork(CanvasState &state,
                                                 int artwork_id) {
  if (state.imported_artwork_separation_preview.active &&
      state.imported_artwork_separation_preview.artwork_id == artwork_id) {
    ClearImportedArtworkSeparationPreviewState(state);
  }
  if (state.imported_artwork_auto_cut_preview.active &&
      state.imported_artwork_auto_cut_preview.artwork_id == artwork_id) {
    ClearImportedArtworkAutoCutPreviewState(state);
  }
}

std::string
ImportedArtworkOperationTargetLabel(const ImportedArtwork *artwork) {
  if (artwork == nullptr) {
    return "Artwork";
  }
  if (!artwork->name.empty()) {
    return artwork->name;
  }
  return "Artwork " + std::to_string(artwork->id);
}

void AccumulateImportedArtworkOperationResult(
    ImportedArtworkOperationResult *aggregate,
    const ImportedArtworkOperationResult &result) {
  aggregate->selected_count += result.selected_count;
  aggregate->moved_count += result.moved_count;
  aggregate->skipped_count += result.skipped_count;
  aggregate->preserved_count += result.preserved_count;
  aggregate->stitched_count += result.stitched_count;
  aggregate->cleaned_count += result.cleaned_count;
  aggregate->ambiguous_count += result.ambiguous_count;
  aggregate->placeholder_count += result.placeholder_count;
  aggregate->outer_count += result.outer_count;
  aggregate->hole_count += result.hole_count;
  aggregate->island_count += result.island_count;
  aggregate->attached_hole_count += result.attached_hole_count;
  aggregate->orphan_hole_count += result.orphan_hole_count;
  aggregate->repaired_hole_count += result.repaired_hole_count;
  aggregate->closed_count += result.closed_count;
  aggregate->open_count += result.open_count;
  aggregate->auto_close_endpoint_count += result.auto_close_endpoint_count;
  aggregate->auto_close_cluster_count += result.auto_close_cluster_count;
  aggregate->auto_close_group_count += result.auto_close_group_count;
  aggregate->auto_close_component_count += result.auto_close_component_count;
  aggregate->auto_close_pass_count += result.auto_close_pass_count;
  aggregate->auto_close_elapsed_ms += result.auto_close_elapsed_ms;
  aggregate->cut_ready = aggregate->cut_ready && result.cut_ready;
  aggregate->nest_ready = aggregate->nest_ready && result.nest_ready;
  if (result.created_artwork_id != 0) {
    aggregate->created_artwork_id = result.created_artwork_id;
  }
}

void SyncImportedArtworkSourceMetadata(ImportedArtwork *artwork) {
  if (artwork == nullptr) {
    return;
  }

  std::vector<int> source_artwork_ids;
  const auto include_source_artwork_id =
      [&source_artwork_ids](const int source_artwork_id) {
        if (source_artwork_id == 0 ||
            std::find(source_artwork_ids.begin(), source_artwork_ids.end(),
                      source_artwork_id) != source_artwork_ids.end()) {
          return;
        }
        source_artwork_ids.push_back(source_artwork_id);
      };

  for (const ImportedPath &path : artwork->paths) {
    for (const ImportedSourceReference &reference : path.provenance) {
      include_source_artwork_id(reference.source_artwork_id);
    }
  }
  for (const ImportedDxfText &text : artwork->dxf_text) {
    for (const ImportedSourceReference &reference : text.provenance) {
      include_source_artwork_id(reference.source_artwork_id);
    }
  }

  if (source_artwork_ids.empty() && artwork->part.source_artwork_id != 0) {
    source_artwork_ids.push_back(artwork->part.source_artwork_id);
  }

  if (!source_artwork_ids.empty()) {
    artwork->part.source_artwork_id = source_artwork_ids.front();
    artwork->part.contributing_source_artwork_ids =
        std::move(source_artwork_ids);
  }
}

bool UngroupImportedGroupInPlace(ImportedArtwork *artwork, int group_id) {
  if (artwork == nullptr) {
    return false;
  }

  ImportedGroup *group = FindImportedGroup(*artwork, group_id);
  if (group == nullptr || group->id == artwork->root_group_id ||
      group->parent_group_id == 0) {
    return false;
  }

  const int parent_group_id = group->parent_group_id;
  const std::vector<int> child_group_ids = group->child_group_ids;
  const std::vector<int> path_ids = group->path_ids;
  const std::vector<int> text_ids = group->dxf_text_ids;

  ImportedGroup *parent_group = FindImportedGroup(*artwork, parent_group_id);
  if (parent_group == nullptr) {
    return false;
  }

  RemoveImportedGroupReference(&parent_group->child_group_ids, group_id);
  parent_group->child_group_ids.insert(parent_group->child_group_ids.end(),
                                       child_group_ids.begin(),
                                       child_group_ids.end());
  parent_group->path_ids.insert(parent_group->path_ids.end(), path_ids.begin(),
                                path_ids.end());
  parent_group->dxf_text_ids.insert(parent_group->dxf_text_ids.end(),
                                    text_ids.begin(), text_ids.end());

  for (ImportedGroup &child_group : artwork->groups) {
    if (std::find(child_group_ids.begin(), child_group_ids.end(),
                  child_group.id) != child_group_ids.end()) {
      child_group.parent_group_id = parent_group_id;
    }
  }
  for (ImportedPath &path : artwork->paths) {
    if (std::find(path_ids.begin(), path_ids.end(), path.id) !=
        path_ids.end()) {
      path.parent_group_id = parent_group_id;
    }
  }
  for (ImportedDxfText &text : artwork->dxf_text) {
    if (std::find(text_ids.begin(), text_ids.end(), text.id) !=
        text_ids.end()) {
      text.parent_group_id = parent_group_id;
    }
  }

  std::erase_if(artwork->groups, [group_id](const ImportedGroup &candidate) {
    return candidate.id == group_id;
  });
  PruneEmptyGroups(*artwork);
  RecomputeImportedHierarchyBounds(*artwork);
  RefreshImportedArtworkPartMetadata(*artwork);
  SyncImportedArtworkSourceMetadata(artwork);
  return true;
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

ImportedTextContour *FindImportedTextContourByIndex(ImportedDxfText &text,
                                                    int contour_index) {
  int current_index = 0;
  for (ImportedTextGlyph &glyph : text.glyphs) {
    for (ImportedTextContour &contour : glyph.contours) {
      if (contour.role == ImportedTextContourRole::Guide ||
          contour.segments.empty()) {
        continue;
      }
      if (current_index == contour_index) {
        return &contour;
      }
      current_index += 1;
    }
  }
  for (ImportedTextContour &contour : text.placeholder_contours) {
    if (contour.role == ImportedTextContourRole::Guide ||
        contour.segments.empty()) {
      continue;
    }
    if (current_index == contour_index) {
      return &contour;
    }
    current_index += 1;
  }
  return nullptr;
}

double ImportedPathSignedArea(const ImportedPath &path) {
  if (!path.closed || path.segments.empty()) {
    return 0.0;
  }

  const Clipper2Lib::PathD polygon =
      detail::SampleImportedPathToClipperPath(path);
  if (polygon.size() < 3) {
    return 0.0;
  }
  return Clipper2Lib::Area(polygon);
}

void CollectImportedIssueElements(
    const ImportedArtwork &artwork,
    std::vector<ImportedElementSelection> *issue_elements) {
  for (const ImportedPath &path : artwork.paths) {
    if (HasImportedElementIssueFlag(path.issue_flags,
                                    ImportedElementIssueFlagOpenGeometry) ||
        HasImportedElementIssueFlag(path.issue_flags,
                                    ImportedElementIssueFlagOrphanHole) ||
        HasImportedElementIssueFlag(path.issue_flags,
                                    ImportedElementIssueFlagAmbiguousCleanup)) {
      issue_elements->push_back({ImportedElementKind::Path, path.id});
    }
  }
  for (const ImportedDxfText &text : artwork.dxf_text) {
    if (HasImportedElementIssueFlag(text.issue_flags,
                                    ImportedElementIssueFlagOpenGeometry) ||
        HasImportedElementIssueFlag(text.issue_flags,
                                    ImportedElementIssueFlagOrphanHole) ||
        HasImportedElementIssueFlag(text.issue_flags,
                                    ImportedElementIssueFlagPlaceholderText)) {
      issue_elements->push_back({ImportedElementKind::DxfText, text.id});
    }
  }
}

int CountGroupableImportedRootItems(const ImportedArtwork &artwork) {
  const ImportedGroup *root = FindImportedGroup(artwork, artwork.root_group_id);
  if (root == nullptr) {
    return 0;
  }

  return static_cast<int>(root->child_group_ids.size()) +
         static_cast<int>(root->path_ids.size()) +
         static_cast<int>(root->dxf_text_ids.size());
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
                        std::unordered_set<int> *retained_group_ids,
                        int depth = 0) {
  constexpr int kMaxGroupNestingDepth = 256;
  if (depth >= kMaxGroupNestingDepth) {
    return false;
  }

  const ImportedGroup *group = FindImportedGroup(artwork, group_id);
  if (group == nullptr) {
    return false;
  }

  bool keep = !group->path_ids.empty() || !group->dxf_text_ids.empty();
  for (const int child_group_id : group->child_group_ids) {
    keep = MarkRetainedGroups(artwork, child_group_id, retained_group_ids,
                              depth + 1) ||
           keep;
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

void TranslateImportedPathToWorld(const ImportedArtwork &source,
                                  ImportedPath *path) {
  if (path == nullptr) {
    return;
  }

  for (ImportedPathSegment &segment : path->segments) {
    segment.start = ImportedArtworkPointToWorld(source, segment.start);
    if (segment.kind == ImportedPathSegmentKind::CubicBezier) {
      segment.control1 = ImportedArtworkPointToWorld(source, segment.control1);
      segment.control2 = ImportedArtworkPointToWorld(source, segment.control2);
    }
    segment.end = ImportedArtworkPointToWorld(source, segment.end);
  }
}

void TranslateImportedDxfTextToWorld(const ImportedArtwork &source,
                                     ImportedDxfText *text) {
  if (text == nullptr) {
    return;
  }

  text->anchor_point = ImportedArtworkPointToWorld(source, text->anchor_point);
  for (ImportedTextGlyph &glyph : text->glyphs) {
    for (ImportedTextContour &contour : glyph.contours) {
      for (ImportedPathSegment &segment : contour.segments) {
        segment.start = ImportedArtworkPointToWorld(source, segment.start);
        if (segment.kind == ImportedPathSegmentKind::CubicBezier) {
          segment.control1 =
              ImportedArtworkPointToWorld(source, segment.control1);
          segment.control2 =
              ImportedArtworkPointToWorld(source, segment.control2);
        }
        segment.end = ImportedArtworkPointToWorld(source, segment.end);
      }
    }
  }

  for (ImportedTextContour &contour : text->placeholder_contours) {
    for (ImportedPathSegment &segment : contour.segments) {
      segment.start = ImportedArtworkPointToWorld(source, segment.start);
      if (segment.kind == ImportedPathSegmentKind::CubicBezier) {
        segment.control1 =
            ImportedArtworkPointToWorld(source, segment.control1);
        segment.control2 =
            ImportedArtworkPointToWorld(source, segment.control2);
      }
      segment.end = ImportedArtworkPointToWorld(source, segment.end);
    }
  }
}

ImportedArtwork BuildArtworkGroupFromSelection(
    const std::vector<const ImportedArtwork *> &source_artworks) {
  ImportedArtwork grouped_artwork;
  grouped_artwork.name = source_artworks.front()->name.empty()
                             ? "Grouped Artwork"
                             : source_artworks.front()->name + " Group";
  grouped_artwork.source_path = source_artworks.front()->source_path;
  grouped_artwork.source_format = source_artworks.front()->source_format;
  grouped_artwork.origin = ImVec2(0.0f, 0.0f);
  grouped_artwork.scale = ImVec2(1.0f, 1.0f);
  grouped_artwork.part.source_artwork_id =
      source_artworks.front()->part.source_artwork_id;
  grouped_artwork.root_group_id = 1;
  grouped_artwork.next_group_id = 2;
  grouped_artwork.next_path_id = 1;
  grouped_artwork.next_dxf_text_id = 1;
  grouped_artwork.visible = true;
  grouped_artwork.flags = source_artworks.front()->flags;

  ImportedGroup root_group;
  root_group.id = grouped_artwork.root_group_id;
  root_group.label = "Root";

  bool has_world_bounds = false;
  ImVec2 grouped_world_min(0.0f, 0.0f);
  ImVec2 grouped_world_max(0.0f, 0.0f);
  const auto include_world_bounds = [&](const ImportedArtwork &artwork) {
    ImVec2 world_min;
    ImVec2 world_max;
    ImportedLocalBoundsToWorldBounds(artwork, artwork.bounds_min,
                                     artwork.bounds_max, &world_min,
                                     &world_max);
    if (!has_world_bounds) {
      grouped_world_min = world_min;
      grouped_world_max = world_max;
      has_world_bounds = true;
      return;
    }

    grouped_world_min.x = std::min(grouped_world_min.x, world_min.x);
    grouped_world_min.y = std::min(grouped_world_min.y, world_min.y);
    grouped_world_max.x = std::max(grouped_world_max.x, world_max.x);
    grouped_world_max.y = std::max(grouped_world_max.y, world_max.y);
  };

  for (const ImportedArtwork *source_artwork : source_artworks) {
    if (source_artwork == nullptr) {
      continue;
    }

    if (std::find(grouped_artwork.part.contributing_source_artwork_ids.begin(),
                  grouped_artwork.part.contributing_source_artwork_ids.end(),
                  source_artwork->part.source_artwork_id) ==
        grouped_artwork.part.contributing_source_artwork_ids.end()) {
      grouped_artwork.part.contributing_source_artwork_ids.push_back(
          source_artwork->part.source_artwork_id);
    }

    include_world_bounds(*source_artwork);

    const ImportedGroup *source_root =
        FindImportedGroup(*source_artwork, source_artwork->root_group_id);
    if (source_root == nullptr) {
      continue;
    }

    ImportedGroup artwork_group;
    artwork_group.id = grouped_artwork.next_group_id++;
    artwork_group.parent_group_id = grouped_artwork.root_group_id;
    artwork_group.label = source_artwork->name.empty()
                              ? "Artwork " + std::to_string(source_artwork->id)
                              : source_artwork->name;
    artwork_group.source_id = source_artwork->source_path;
    root_group.child_group_ids.push_back(artwork_group.id);

    std::unordered_map<int, int> group_id_map;
    group_id_map[source_artwork->root_group_id] = artwork_group.id;
    for (const ImportedGroup &group : source_artwork->groups) {
      if (group.id == source_artwork->root_group_id) {
        continue;
      }
      group_id_map[group.id] = grouped_artwork.next_group_id++;
    }

    std::unordered_map<int, int> path_id_map;
    for (const ImportedPath &path : source_artwork->paths) {
      path_id_map[path.id] = grouped_artwork.next_path_id++;
    }

    std::unordered_map<int, int> text_id_map;
    for (const ImportedDxfText &text : source_artwork->dxf_text) {
      text_id_map[text.id] = grouped_artwork.next_dxf_text_id++;
    }

    for (const int child_group_id : source_root->child_group_ids) {
      artwork_group.child_group_ids.push_back(group_id_map.at(child_group_id));
    }
    for (const int path_id : source_root->path_ids) {
      artwork_group.path_ids.push_back(path_id_map.at(path_id));
    }
    for (const int text_id : source_root->dxf_text_ids) {
      artwork_group.dxf_text_ids.push_back(text_id_map.at(text_id));
    }
    grouped_artwork.groups.push_back(std::move(artwork_group));

    for (const ImportedGroup &group : source_artwork->groups) {
      if (group.id == source_artwork->root_group_id) {
        continue;
      }

      ImportedGroup clone = group;
      clone.id = group_id_map.at(group.id);
      clone.parent_group_id = group_id_map.at(group.parent_group_id);
      clone.child_group_ids.clear();
      for (const int child_group_id : group.child_group_ids) {
        clone.child_group_ids.push_back(group_id_map.at(child_group_id));
      }
      clone.path_ids.clear();
      for (const int path_id : group.path_ids) {
        clone.path_ids.push_back(path_id_map.at(path_id));
      }
      clone.dxf_text_ids.clear();
      for (const int text_id : group.dxf_text_ids) {
        clone.dxf_text_ids.push_back(text_id_map.at(text_id));
      }
      grouped_artwork.groups.push_back(std::move(clone));
    }

    for (const ImportedPath &path : source_artwork->paths) {
      ImportedPath clone = path;
      clone.id = path_id_map.at(path.id);
      clone.parent_group_id = group_id_map.at(path.parent_group_id);
      if (clone.provenance.empty()) {
        clone.provenance.push_back({source_artwork->part.source_artwork_id,
                                    ImportedElementKind::Path, path.id});
      }
      TranslateImportedPathToWorld(*source_artwork, &clone);
      grouped_artwork.paths.push_back(std::move(clone));
    }

    for (const ImportedDxfText &text : source_artwork->dxf_text) {
      ImportedDxfText clone = text;
      clone.id = text_id_map.at(text.id);
      clone.parent_group_id = group_id_map.at(text.parent_group_id);
      if (clone.provenance.empty()) {
        clone.provenance.push_back({source_artwork->part.source_artwork_id,
                                    ImportedElementKind::DxfText, text.id});
      }
      TranslateImportedDxfTextToWorld(*source_artwork, &clone);
      grouped_artwork.dxf_text.push_back(std::move(clone));
    }
  }

  grouped_artwork.groups.insert(grouped_artwork.groups.begin(),
                                std::move(root_group));
  RecomputeImportedArtworkBounds(grouped_artwork);
  RecomputeImportedHierarchyBounds(grouped_artwork);
  RefreshImportedArtworkPartMetadata(grouped_artwork);
  SyncImportedArtworkSourceMetadata(&grouped_artwork);
  if (has_world_bounds) {
    grouped_artwork.origin = grouped_world_min;
  }
  return grouped_artwork;
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
  bool has_world_bounds = false;
  ImVec2 subset_world_min(0.0f, 0.0f);

  const auto include_world_bounds = [&](const ImVec2 &local_min,
                                        const ImVec2 &local_max) {
    ImVec2 world_min;
    ImVec2 world_max;
    ImportedLocalBoundsToWorldBounds(source, local_min, local_max, &world_min,
                                     &world_max);
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
      include_world_bounds(path.bounds_min, path.bounds_max);
    }
  }
  for (const ImportedDxfText &text : source.dxf_text) {
    if (text_ids.contains(text.id)) {
      subset.dxf_text.push_back(text);
      include_world_bounds(text.bounds_min, text.bounds_max);
    }
  }

  ResetImportedArtworkCounters(subset);
  RecomputeImportedArtworkBounds(subset);
  RecomputeImportedHierarchyBounds(subset);
  RefreshImportedArtworkPartMetadata(subset);
  if (has_world_bounds) {
    subset.origin = subset_world_min;
  }
  return subset;
}

int ResolveGroupingParentGroupId(const ImportedArtwork &artwork,
                                 const std::unordered_set<int> &path_ids,
                                 const std::unordered_set<int> &text_ids) {
  int parent_group_id = -1;
  const auto update_parent = [&](int candidate_parent_group_id) {
    if (parent_group_id == -1) {
      parent_group_id = candidate_parent_group_id;
      return;
    }
    if (parent_group_id != candidate_parent_group_id) {
      parent_group_id = artwork.root_group_id;
    }
  };

  for (const ImportedPath &path : artwork.paths) {
    if (path_ids.contains(path.id)) {
      update_parent(path.parent_group_id);
    }
  }
  for (const ImportedDxfText &text : artwork.dxf_text) {
    if (text_ids.contains(text.id)) {
      update_parent(text.parent_group_id);
    }
  }

  if (parent_group_id <= 0) {
    return artwork.root_group_id;
  }
  return parent_group_id;
}

void RemoveImportedGroupReference(std::vector<int> *group_ids, int group_id) {
  if (group_ids == nullptr) {
    return;
  }
  std::erase(*group_ids, group_id);
}

int CountGroupingTargets(const std::unordered_set<int> &group_ids,
                         const std::unordered_set<int> &path_ids,
                         const std::unordered_set<int> &text_ids) {
  return static_cast<int>(group_ids.size() + path_ids.size() + text_ids.size());
}

void MoveImportedGroupingTargetsToGroup(
    ImportedArtwork *artwork, ImportedGroup *parent_group,
    const std::unordered_set<int> &group_ids,
    const std::unordered_set<int> &path_ids,
    const std::unordered_set<int> &text_ids, ImportedGroup *new_group) {
  if (artwork == nullptr || parent_group == nullptr || new_group == nullptr) {
    return;
  }

  for (ImportedGroup &group : artwork->groups) {
    if (!group_ids.contains(group.id)) {
      continue;
    }
    if (ImportedGroup *old_parent =
            FindImportedGroup(*artwork, group.parent_group_id);
        old_parent != nullptr) {
      RemoveImportedGroupReference(&old_parent->child_group_ids, group.id);
    }
    group.parent_group_id = new_group->id;
    new_group->child_group_ids.push_back(group.id);
  }
  for (ImportedPath &path : artwork->paths) {
    if (!path_ids.contains(path.id)) {
      continue;
    }
    if (ImportedGroup *old_parent =
            FindImportedGroup(*artwork, path.parent_group_id);
        old_parent != nullptr) {
      RemoveImportedGroupReference(&old_parent->path_ids, path.id);
    }
    path.parent_group_id = new_group->id;
    new_group->path_ids.push_back(path.id);
  }
  for (ImportedDxfText &text : artwork->dxf_text) {
    if (!text_ids.contains(text.id)) {
      continue;
    }
    if (ImportedGroup *old_parent =
            FindImportedGroup(*artwork, text.parent_group_id);
        old_parent != nullptr) {
      RemoveImportedGroupReference(&old_parent->dxf_text_ids, text.id);
    }
    text.parent_group_id = new_group->id;
    new_group->dxf_text_ids.push_back(text.id);
  }

  parent_group->child_group_ids.push_back(new_group->id);
}

struct GuideSplitLogicalPart {
  std::unordered_set<int> path_ids;
  std::unordered_set<int> text_ids;
  std::vector<ImVec2> sample_points;
};

struct GuideSplitPreviewPlan {
  struct BucketAssignment {
    int bucket_index = -1;
    int bucket_column = 0;
    int bucket_row = 0;
    std::unordered_set<int> path_ids;
    std::unordered_set<int> text_ids;
  };

  std::vector<int> guide_ids;
  std::vector<BucketAssignment> buckets;
  std::vector<ImportedElementSelection> skipped_elements;
  std::vector<ImportedSeparationPreviewPart> preview_parts;
};

struct GuideBandLayout {
  std::vector<float> vertical_positions;
  std::vector<float> horizontal_positions;
};

struct AutoCutPreviewPlan {
  struct BucketAssignment {
    int bucket_index = -1;
    int bucket_column = 0;
    int bucket_row = 0;
    std::unordered_set<int> path_ids;
    std::unordered_set<int> text_ids;
  };

  GuideBandLayout layout;
  std::vector<BucketAssignment> buckets;
  std::vector<ImportedElementSelection> skipped_elements;
  std::vector<ImportedSeparationPreviewPart> preview_parts;
};

struct LogicalPartPreviewBounds {
  const GuideSplitLogicalPart *logical_part = nullptr;
  ImVec2 world_bounds_min = ImVec2(0.0f, 0.0f);
  ImVec2 world_bounds_max = ImVec2(0.0f, 0.0f);
  bool has_bounds = false;
};

ImVec2 MinPoint(const ImVec2 &a, const ImVec2 &b) {
  return ImVec2(std::min(a.x, b.x), std::min(a.y, b.y));
}

ImVec2 MaxPoint(const ImVec2 &a, const ImVec2 &b) {
  return ImVec2(std::max(a.x, b.x), std::max(a.y, b.y));
}

void AppendPartPathSamples(const ImportedArtwork &artwork,
                           const std::unordered_set<int> &path_ids,
                           std::vector<ImVec2> *sample_points) {
  for (const int path_id : path_ids) {
    const ImportedPath *path = FindImportedPath(artwork, path_id);
    if (path == nullptr) {
      continue;
    }
    operations::detail::AppendPathSamplePointsWorldShared(artwork, *path,
                                                          sample_points);
  }
}

void AppendPartTextSamples(const ImportedArtwork &artwork,
                           const std::unordered_set<int> &text_ids,
                           std::vector<ImVec2> *sample_points) {
  for (const int text_id : text_ids) {
    const ImportedDxfText *text = FindImportedDxfText(artwork, text_id);
    if (text == nullptr) {
      continue;
    }
    operations::detail::AppendTextSamplePointsWorldShared(artwork, *text,
                                                          sample_points);
  }
}

std::vector<GuideSplitLogicalPart> BuildGuideSplitLogicalParts(
    const ImportedArtwork &artwork,
    std::vector<ImportedElementSelection> *skipped_elements,
    bool skip_orphan_items) {
  std::vector<GuideSplitLogicalPart> parts;
  std::unordered_map<int, size_t> path_outer_part_indices;
  std::unordered_map<int, size_t> text_outer_part_indices;
  std::unordered_set<int> assigned_path_ids;
  std::unordered_set<int> assigned_text_ids;
  std::unordered_set<int> orphan_path_ids;
  std::unordered_set<int> orphan_text_ids;

  for (const ImportedContourReference &hole_ref : artwork.part.orphan_holes) {
    if (hole_ref.kind == ImportedElementKind::Path) {
      orphan_path_ids.insert(hole_ref.item_id);
    } else {
      orphan_text_ids.insert(hole_ref.item_id);
    }
  }

  const auto ensure_part_for_outer =
      [&](const ImportedContourReference &outer) -> GuideSplitLogicalPart * {
    if (outer.kind == ImportedElementKind::Path) {
      auto it = path_outer_part_indices.find(outer.item_id);
      if (it != path_outer_part_indices.end()) {
        return &parts[it->second];
      }
      const size_t index = parts.size();
      parts.push_back({});
      parts.back().path_ids.insert(outer.item_id);
      path_outer_part_indices.emplace(outer.item_id, index);
      assigned_path_ids.insert(outer.item_id);
      return &parts.back();
    }

    auto it = text_outer_part_indices.find(outer.item_id);
    if (it != text_outer_part_indices.end()) {
      return &parts[it->second];
    }
    const size_t index = parts.size();
    parts.push_back({});
    parts.back().text_ids.insert(outer.item_id);
    text_outer_part_indices.emplace(outer.item_id, index);
    assigned_text_ids.insert(outer.item_id);
    return &parts.back();
  };

  for (const ImportedHoleOwnership &ownership : artwork.part.hole_attachments) {
    GuideSplitLogicalPart *part = ensure_part_for_outer(ownership.outer);
    if (ownership.hole.kind == ImportedElementKind::Path) {
      part->path_ids.insert(ownership.hole.item_id);
      assigned_path_ids.insert(ownership.hole.item_id);
    } else {
      part->text_ids.insert(ownership.hole.item_id);
      assigned_text_ids.insert(ownership.hole.item_id);
    }
  }

  for (const ImportedPath &path : artwork.paths) {
    if (assigned_path_ids.contains(path.id)) {
      continue;
    }
    if (skip_orphan_items && orphan_path_ids.contains(path.id)) {
      skipped_elements->push_back({ImportedElementKind::Path, path.id});
      continue;
    }
    GuideSplitLogicalPart part;
    part.path_ids.insert(path.id);
    assigned_path_ids.insert(path.id);
    parts.push_back(std::move(part));
  }

  for (const ImportedDxfText &text : artwork.dxf_text) {
    if (assigned_text_ids.contains(text.id)) {
      continue;
    }
    if (skip_orphan_items && orphan_text_ids.contains(text.id)) {
      skipped_elements->push_back({ImportedElementKind::DxfText, text.id});
      continue;
    }
    GuideSplitLogicalPart part;
    part.text_ids.insert(text.id);
    assigned_text_ids.insert(text.id);
    parts.push_back(std::move(part));
  }

  for (GuideSplitLogicalPart &part : parts) {
    AppendPartPathSamples(artwork, part.path_ids, &part.sample_points);
    AppendPartTextSamples(artwork, part.text_ids, &part.sample_points);
  }

  return parts;
}

bool AutoCutAxisModeIncludesVertical(AutoCutPreviewAxisMode axis_mode) {
  return axis_mode == AutoCutPreviewAxisMode::VerticalOnly ||
         axis_mode == AutoCutPreviewAxisMode::Both;
}

bool AutoCutAxisModeIncludesHorizontal(AutoCutPreviewAxisMode axis_mode) {
  return axis_mode == AutoCutPreviewAxisMode::HorizontalOnly ||
         axis_mode == AutoCutPreviewAxisMode::Both;
}

float NormalizeAutoCutMinimumGap(float minimum_gap) {
  return std::max(minimum_gap,
                  operations::detail::kSharedGuideClassificationEpsilon);
}

std::vector<LogicalPartPreviewBounds> BuildLogicalPartPreviewBounds(
    const std::vector<GuideSplitLogicalPart> &logical_parts) {
  std::vector<LogicalPartPreviewBounds> bounds;
  bounds.reserve(logical_parts.size());

  for (const GuideSplitLogicalPart &part : logical_parts) {
    LogicalPartPreviewBounds preview_bounds;
    preview_bounds.logical_part = &part;
    preview_bounds.world_bounds_min = ImVec2(std::numeric_limits<float>::max(),
                                             std::numeric_limits<float>::max());
    preview_bounds.world_bounds_max = ImVec2(
        -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max());

    for (const ImVec2 &point : part.sample_points) {
      preview_bounds.world_bounds_min =
          MinPoint(preview_bounds.world_bounds_min, point);
      preview_bounds.world_bounds_max =
          MaxPoint(preview_bounds.world_bounds_max, point);
      preview_bounds.has_bounds = true;
    }

    if (!preview_bounds.has_bounds) {
      preview_bounds.world_bounds_min = ImVec2(0.0f, 0.0f);
      preview_bounds.world_bounds_max = ImVec2(0.0f, 0.0f);
    }
    bounds.push_back(preview_bounds);
  }

  return bounds;
}

std::vector<float> BuildAutoCutAxisPositions(
    const std::vector<LogicalPartPreviewBounds> &part_bounds,
    bool vertical_axis, float minimum_gap) {
  struct AxisInterval {
    float min = 0.0f;
    float max = 0.0f;
  };

  std::vector<AxisInterval> intervals;
  intervals.reserve(part_bounds.size());
  for (const LogicalPartPreviewBounds &part : part_bounds) {
    if (!part.has_bounds) {
      continue;
    }

    const float axis_min =
        vertical_axis ? part.world_bounds_min.x : part.world_bounds_min.y;
    const float axis_max =
        vertical_axis ? part.world_bounds_max.x : part.world_bounds_max.y;
    intervals.push_back(
        {std::min(axis_min, axis_max), std::max(axis_min, axis_max)});
  }

  if (intervals.size() < 2) {
    return {};
  }

  std::sort(intervals.begin(), intervals.end(),
            [](const AxisInterval &a, const AxisInterval &b) {
              if (a.min != b.min) {
                return a.min < b.min;
              }
              return a.max < b.max;
            });

  std::vector<AxisInterval> merged_intervals;
  merged_intervals.push_back(intervals.front());
  for (size_t index = 1; index < intervals.size(); ++index) {
    AxisInterval &current = merged_intervals.back();
    const AxisInterval &candidate = intervals[index];
    if (candidate.min <=
        current.max + operations::detail::kSharedGuideClassificationEpsilon) {
      current.max = std::max(current.max, candidate.max);
      continue;
    }
    merged_intervals.push_back(candidate);
  }

  std::vector<float> positions;
  for (size_t index = 1; index < merged_intervals.size(); ++index) {
    const AxisInterval &previous = merged_intervals[index - 1];
    const AxisInterval &next = merged_intervals[index];
    const float gap = next.min - previous.max;
    if (gap < minimum_gap) {
      continue;
    }
    positions.push_back((previous.max + next.min) * 0.5f);
  }

  return positions;
}

AutoCutPreviewPlan BuildAutoCutPreviewPlan(const ImportedArtwork &artwork,
                                           AutoCutPreviewAxisMode axis_mode,
                                           float minimum_gap) {
  AutoCutPreviewPlan plan;
  std::vector<GuideSplitLogicalPart> logical_parts =
      BuildGuideSplitLogicalParts(artwork, &plan.skipped_elements, false);
  const size_t orphan_count = plan.skipped_elements.size();
  const std::vector<LogicalPartPreviewBounds> logical_part_bounds =
      BuildLogicalPartPreviewBounds(logical_parts);
  const float normalized_gap = NormalizeAutoCutMinimumGap(minimum_gap);

  if (AutoCutAxisModeIncludesVertical(axis_mode)) {
    plan.layout.vertical_positions =
        BuildAutoCutAxisPositions(logical_part_bounds, true, normalized_gap);
  }
  if (AutoCutAxisModeIncludesHorizontal(axis_mode)) {
    plan.layout.horizontal_positions =
        BuildAutoCutAxisPositions(logical_part_bounds, false, normalized_gap);
  }

  std::map<std::pair<int, int>, size_t> bucket_lookup;
  const auto classify_axis = [](float min_value, float max_value,
                                const std::vector<float> &positions,
                                int *bucket) {
    *bucket = 0;
    for (float position : positions) {
      if (max_value <
          position - operations::detail::kSharedGuideClassificationEpsilon) {
        return true;
      }
      if (min_value >
          position + operations::detail::kSharedGuideClassificationEpsilon) {
        *bucket += 1;
        continue;
      }
      return false;
    }
    return true;
  };

  for (const LogicalPartPreviewBounds &part_bounds : logical_part_bounds) {
    ImportedSeparationPreviewPart preview_part;
    preview_part.world_bounds_min = part_bounds.world_bounds_min;
    preview_part.world_bounds_max = part_bounds.world_bounds_max;

    for (const int path_id : part_bounds.logical_part->path_ids) {
      preview_part.elements.push_back({ImportedElementKind::Path, path_id});
    }
    for (const int text_id : part_bounds.logical_part->text_ids) {
      preview_part.elements.push_back({ImportedElementKind::DxfText, text_id});
    }

    int bucket_column = 0;
    int bucket_row = 0;
    const bool in_vertical_band = classify_axis(
        preview_part.world_bounds_min.x, preview_part.world_bounds_max.x,
        plan.layout.vertical_positions, &bucket_column);
    const bool in_horizontal_band = classify_axis(
        preview_part.world_bounds_min.y, preview_part.world_bounds_max.y,
        plan.layout.horizontal_positions, &bucket_row);

    if (!in_vertical_band || !in_horizontal_band) {
      preview_part.classification =
          ImportedSeparationPreviewClassification::Crossing;
      for (const int path_id : part_bounds.logical_part->path_ids) {
        plan.skipped_elements.push_back({ImportedElementKind::Path, path_id});
      }
      for (const int text_id : part_bounds.logical_part->text_ids) {
        plan.skipped_elements.push_back(
            {ImportedElementKind::DxfText, text_id});
      }
      plan.preview_parts.push_back(std::move(preview_part));
      continue;
    }

    preview_part.classification =
        ImportedSeparationPreviewClassification::Assigned;
    preview_part.bucket_column = bucket_column;
    preview_part.bucket_row = bucket_row;

    const std::pair<int, int> bucket_key(bucket_column, bucket_row);
    auto bucket_it = bucket_lookup.find(bucket_key);
    if (bucket_it == bucket_lookup.end()) {
      const size_t new_index = plan.buckets.size();
      plan.buckets.push_back(
          {static_cast<int>(new_index), bucket_column, bucket_row, {}, {}});
      bucket_lookup.emplace(bucket_key, new_index);
      bucket_it = bucket_lookup.find(bucket_key);
    }

    AutoCutPreviewPlan::BucketAssignment &bucket =
        plan.buckets[bucket_it->second];
    preview_part.bucket_index = bucket.bucket_index;
    bucket.path_ids.insert(part_bounds.logical_part->path_ids.begin(),
                           part_bounds.logical_part->path_ids.end());
    bucket.text_ids.insert(part_bounds.logical_part->text_ids.begin(),
                           part_bounds.logical_part->text_ids.end());
    plan.preview_parts.push_back(std::move(preview_part));
  }

  for (size_t index = 0; index < orphan_count; ++index) {
    const ImportedElementSelection &skipped = plan.skipped_elements[index];
    ImportedSeparationPreviewPart skipped_part;
    skipped_part.classification =
        ImportedSeparationPreviewClassification::Orphan;
    if (skipped.kind == ImportedElementKind::Path) {
      if (const ImportedPath *path = FindImportedPath(artwork, skipped.item_id);
          path != nullptr) {
        ImportedLocalBoundsToWorldBounds(
            artwork, path->bounds_min, path->bounds_max,
            &skipped_part.world_bounds_min, &skipped_part.world_bounds_max);
      }
    } else {
      if (const ImportedDxfText *text =
              FindImportedDxfText(artwork, skipped.item_id);
          text != nullptr) {
        ImportedLocalBoundsToWorldBounds(
            artwork, text->bounds_min, text->bounds_max,
            &skipped_part.world_bounds_min, &skipped_part.world_bounds_max);
      }
    }
    skipped_part.elements.push_back(skipped);
    plan.preview_parts.push_back(std::move(skipped_part));
  }

  return plan;
}

GuideSplitPreviewPlan BuildGuideSplitPreviewPlan(const ImportedArtwork &artwork,
                                                 const CanvasState &state,
                                                 int requested_guide_id) {
  GuideSplitPreviewPlan plan;
  GuideBandLayout layout;
  for (const Guide &guide : state.guides) {
    plan.guide_ids.push_back(guide.id);
    if (guide.orientation == GuideOrientation::Vertical) {
      layout.vertical_positions.push_back(guide.position);
    } else {
      layout.horizontal_positions.push_back(guide.position);
    }
  }
  std::sort(layout.vertical_positions.begin(), layout.vertical_positions.end());
  std::sort(layout.horizontal_positions.begin(),
            layout.horizontal_positions.end());
  std::vector<GuideSplitLogicalPart> logical_parts =
      BuildGuideSplitLogicalParts(artwork, &plan.skipped_elements, false);
  const size_t orphan_count = plan.skipped_elements.size();
  std::map<std::pair<int, int>, size_t> bucket_lookup;

  const auto classify_axis = [](float min_value, float max_value,
                                const std::vector<float> &positions,
                                int *bucket) {
    *bucket = 0;
    for (float position : positions) {
      if (max_value <
          position - operations::detail::kSharedGuideClassificationEpsilon) {
        return true;
      }
      if (min_value >
          position + operations::detail::kSharedGuideClassificationEpsilon) {
        *bucket += 1;
        continue;
      }
      return false;
    }
    return true;
  };

  for (const GuideSplitLogicalPart &part : logical_parts) {
    ImportedSeparationPreviewPart preview_part;
    preview_part.world_bounds_min = ImVec2(std::numeric_limits<float>::max(),
                                           std::numeric_limits<float>::max());
    preview_part.world_bounds_max = ImVec2(-std::numeric_limits<float>::max(),
                                           -std::numeric_limits<float>::max());

    for (const int path_id : part.path_ids) {
      preview_part.elements.push_back({ImportedElementKind::Path, path_id});
    }
    for (const int text_id : part.text_ids) {
      preview_part.elements.push_back({ImportedElementKind::DxfText, text_id});
    }

    for (const ImVec2 &point : part.sample_points) {
      preview_part.world_bounds_min =
          MinPoint(preview_part.world_bounds_min, point);
      preview_part.world_bounds_max =
          MaxPoint(preview_part.world_bounds_max, point);
    }

    if (part.sample_points.empty()) {
      preview_part.world_bounds_min = ImVec2(0.0f, 0.0f);
      preview_part.world_bounds_max = ImVec2(0.0f, 0.0f);
    }

    int bucket_column = 0;
    int bucket_row = 0;
    const bool in_vertical_band = classify_axis(
        preview_part.world_bounds_min.x, preview_part.world_bounds_max.x,
        layout.vertical_positions, &bucket_column);
    const bool in_horizontal_band = classify_axis(
        preview_part.world_bounds_min.y, preview_part.world_bounds_max.y,
        layout.horizontal_positions, &bucket_row);

    if (!in_vertical_band || !in_horizontal_band) {
      preview_part.classification =
          ImportedSeparationPreviewClassification::Crossing;
      for (const int path_id : part.path_ids) {
        plan.skipped_elements.push_back({ImportedElementKind::Path, path_id});
      }
      for (const int text_id : part.text_ids) {
        plan.skipped_elements.push_back(
            {ImportedElementKind::DxfText, text_id});
      }
      plan.preview_parts.push_back(std::move(preview_part));
      continue;
    }

    preview_part.classification =
        ImportedSeparationPreviewClassification::Assigned;
    preview_part.bucket_column = bucket_column;
    preview_part.bucket_row = bucket_row;

    const std::pair<int, int> bucket_key(bucket_column, bucket_row);
    auto bucket_it = bucket_lookup.find(bucket_key);
    if (bucket_it == bucket_lookup.end()) {
      const size_t new_index = plan.buckets.size();
      plan.buckets.push_back(
          {static_cast<int>(new_index), bucket_column, bucket_row, {}, {}});
      bucket_lookup.emplace(bucket_key, new_index);
      bucket_it = bucket_lookup.find(bucket_key);
    }

    GuideSplitPreviewPlan::BucketAssignment &bucket =
        plan.buckets[bucket_it->second];
    preview_part.bucket_index = bucket.bucket_index;
    bucket.path_ids.insert(part.path_ids.begin(), part.path_ids.end());
    bucket.text_ids.insert(part.text_ids.begin(), part.text_ids.end());
    plan.preview_parts.push_back(std::move(preview_part));
  }

  for (size_t index = 0; index < orphan_count; ++index) {
    const ImportedElementSelection &skipped = plan.skipped_elements[index];
    ImportedSeparationPreviewPart skipped_part;
    skipped_part.classification =
        ImportedSeparationPreviewClassification::Orphan;
    if (skipped.kind == ImportedElementKind::Path) {
      if (const ImportedPath *path = FindImportedPath(artwork, skipped.item_id);
          path != nullptr) {
        ImportedLocalBoundsToWorldBounds(
            artwork, path->bounds_min, path->bounds_max,
            &skipped_part.world_bounds_min, &skipped_part.world_bounds_max);
      }
    } else {
      if (const ImportedDxfText *text =
              FindImportedDxfText(artwork, skipped.item_id);
          text != nullptr) {
        ImportedLocalBoundsToWorldBounds(
            artwork, text->bounds_min, text->bounds_max,
            &skipped_part.world_bounds_min, &skipped_part.world_bounds_max);
      }
    }
    skipped_part.elements.push_back(skipped);
    plan.preview_parts.push_back(std::move(skipped_part));
  }

  if (requested_guide_id != 0 &&
      std::find(plan.guide_ids.begin(), plan.guide_ids.end(),
                requested_guide_id) == plan.guide_ids.end()) {
    plan.guide_ids.push_back(requested_guide_id);
  }

  return plan;
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
  ClearImportedArtworkPreviewStatesForArtwork(state, imported_artwork_id);

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
      AppendImportedArtwork(state, std::move(subset), false);
  if (source_empty) {
    DeleteImportedArtwork(state, imported_artwork_id);
  }

  ClearSelectedImportedElements(state);
  SetSingleSelectedImportedArtworkObject(state, created_artwork_id);
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

template <typename BucketAssignment>
ImportedArtworkOperationResult
MoveImportedElementsToNewArtworks(CanvasState &state, int imported_artwork_id,
                                  const std::vector<BucketAssignment> &buckets,
                                  const std::string &name_suffix,
                                  const std::string &action_verb,
                                  bool create_groups_from_cuts = false) {
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
  ClearImportedArtworkPreviewStatesForArtwork(state, imported_artwork_id);

  std::unordered_set<int> moved_path_ids;
  std::unordered_set<int> moved_text_ids;
  for (const BucketAssignment &bucket : buckets) {
    moved_path_ids.insert(bucket.path_ids.begin(), bucket.path_ids.end());
    moved_text_ids.insert(bucket.text_ids.begin(), bucket.text_ids.end());
  }

  const int moved_count =
      static_cast<int>(moved_path_ids.size() + moved_text_ids.size());
  result.moved_count = moved_count;
  if (moved_count == 0) {
    result.message = "No imported elements were eligible for extraction.";
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  std::vector<ImportedArtwork> subsets;
  subsets.reserve(buckets.size());
  for (const BucketAssignment &bucket : buckets) {
    if (bucket.path_ids.empty() && bucket.text_ids.empty()) {
      continue;
    }
    subsets.push_back(BuildArtworkSubset(
        *artwork, bucket.path_ids, bucket.text_ids,
        name_suffix + " R" + std::to_string(bucket.bucket_row + 1) + "C" +
            std::to_string(bucket.bucket_column + 1)));
  }

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

  int created_artwork_id = 0;
  std::vector<int> created_artwork_ids;
  created_artwork_ids.reserve(subsets.size());
  for (ImportedArtwork &subset : subsets) {
    const int appended_artwork_id =
        AppendImportedArtwork(state, std::move(subset), false);
    if (created_artwork_id == 0) {
      created_artwork_id = appended_artwork_id;
    }
    if (appended_artwork_id != 0) {
      created_artwork_ids.push_back(appended_artwork_id);
    }
  }

  int grouped_artwork_count = 0;
  if (create_groups_from_cuts) {
    for (const int appended_artwork_id : created_artwork_ids) {
      ImportedArtwork *created_artwork =
          FindImportedArtwork(state, appended_artwork_id);
      if (created_artwork == nullptr) {
        continue;
      }

      const ImportedGroup *root_group =
          FindImportedGroup(*created_artwork, created_artwork->root_group_id);
      int root_item_count = 0;
      if (root_group != nullptr) {
        root_item_count = static_cast<int>(root_group->child_group_ids.size() +
                                           root_group->path_ids.size() +
                                           root_group->dxf_text_ids.size());
      } else {
        root_item_count = static_cast<int>(created_artwork->paths.size() +
                                           created_artwork->dxf_text.size());
      }
      if (root_item_count < 2) {
        continue;
      }

      ImportedArtworkOperationResult group_result =
          GroupImportedArtworkRootContents(state, appended_artwork_id);
      if (group_result.success) {
        grouped_artwork_count += 1;
      }
    }
  }
  if (source_empty) {
    DeleteImportedArtwork(state, imported_artwork_id);
  }

  ClearSelectedImportedElements(state);
  SetSingleSelectedImportedArtworkObject(state, created_artwork_id);
  state.selected_imported_debug = {ImportedDebugSelectionKind::Artwork,
                                   created_artwork_id, 0};

  result.success = created_artwork_id != 0;
  result.created_artwork_id = created_artwork_id;
  if (const ImportedArtwork *created_artwork =
          FindImportedArtwork(state, created_artwork_id);
      created_artwork != nullptr) {
    PopulateOperationReadiness(&result, *created_artwork);
  }
  result.message =
      action_verb + " " + std::to_string(moved_count) + " imported element" +
      (moved_count == 1 ? std::string() : std::string("s")) + " into " +
      std::to_string(subsets.size()) + " artwork" +
      (subsets.size() == 1 ? std::string() : std::string("s")) + ".";
  if (create_groups_from_cuts && grouped_artwork_count > 0) {
    result.message +=
        " Auto-grouped " + std::to_string(grouped_artwork_count) +
        " cut artwork" +
        (grouped_artwork_count == 1 ? std::string() : std::string("s")) + ".";
  }
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

  PushUndoSnapshot(state, action_name);
  ClearImportedArtworkPreviewStatesForArtwork(state, imported_artwork_id);

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

int CountImportedElementIds(const std::unordered_set<int> &path_ids,
                            const std::unordered_set<int> &text_ids) {
  return static_cast<int>(path_ids.size() + text_ids.size());
}

bool CollectDebugSelectionElementIds(const CanvasState &state,
                                     const ImportedArtwork &artwork,
                                     std::unordered_set<int> *path_ids,
                                     std::unordered_set<int> *text_ids) {
  if (path_ids == nullptr || text_ids == nullptr ||
      state.selected_imported_debug.artwork_id != artwork.id) {
    return false;
  }

  switch (state.selected_imported_debug.kind) {
  case ImportedDebugSelectionKind::Group: {
    if (FindImportedGroup(artwork, state.selected_imported_debug.item_id) ==
        nullptr) {
      return false;
    }
    std::unordered_set<int> visited_group_ids;
    operations::detail::CollectImportedGroupElementIdsShared(
        artwork, state.selected_imported_debug.item_id, path_ids, text_ids,
        &visited_group_ids);
    return CountImportedElementIds(*path_ids, *text_ids) > 0;
  }
  case ImportedDebugSelectionKind::Path:
    if (FindImportedPath(artwork, state.selected_imported_debug.item_id) ==
        nullptr) {
      return false;
    }
    path_ids->insert(state.selected_imported_debug.item_id);
    return true;
  case ImportedDebugSelectionKind::DxfText:
    if (FindImportedDxfText(artwork, state.selected_imported_debug.item_id) ==
        nullptr) {
      return false;
    }
    text_ids->insert(state.selected_imported_debug.item_id);
    return true;
  case ImportedDebugSelectionKind::Artwork:
  case ImportedDebugSelectionKind::None:
    break;
  }
  return false;
}

bool CollectElementClipboardSelection(const CanvasState &state,
                                      const ImportedArtwork &artwork,
                                      std::vector<ImportedArtwork> *artworks,
                                      int *selected_count) {
  if (artworks == nullptr || selected_count == nullptr) {
    return false;
  }

  std::unordered_set<int> path_ids;
  std::unordered_set<int> text_ids;
  if (state.selected_imported_artwork_id == artwork.id &&
      !state.selected_imported_elements.empty()) {
    for (const ImportedElementSelection &selection :
         state.selected_imported_elements) {
      if (selection.kind == ImportedElementKind::Path) {
        if (FindImportedPath(artwork, selection.item_id) != nullptr) {
          path_ids.insert(selection.item_id);
        }
      } else if (FindImportedDxfText(artwork, selection.item_id) != nullptr) {
        text_ids.insert(selection.item_id);
      }
    }
  }

  if (path_ids.empty() && text_ids.empty() &&
      !CollectDebugSelectionElementIds(state, artwork, &path_ids, &text_ids)) {
    return false;
  }

  const int element_count = CountImportedElementIds(path_ids, text_ids);
  if (element_count == 0) {
    return false;
  }

  artworks->push_back(BuildArtworkSubset(artwork, path_ids, text_ids, ""));
  *selected_count = element_count;
  return true;
}

bool CollectSelectionForClipboard(const CanvasState &state,
                                  std::vector<ImportedArtwork> *artworks,
                                  int *selected_count) {
  if (artworks == nullptr || selected_count == nullptr) {
    return false;
  }

  artworks->clear();
  *selected_count = 0;

  if (state.selected_imported_artwork_id != 0) {
    const ImportedArtwork *artwork =
        FindImportedArtwork(state, state.selected_imported_artwork_id);
    if (artwork != nullptr && CollectElementClipboardSelection(
                                  state, *artwork, artworks, selected_count)) {
      return true;
    }
  }

  const std::vector<int> selected_artwork_ids =
      GetSelectedImportedArtworkObjects(state);
  if (selected_artwork_ids.empty()) {
    return false;
  }

  artworks->reserve(selected_artwork_ids.size());
  for (const int artwork_id : selected_artwork_ids) {
    const ImportedArtwork *artwork = FindImportedArtwork(state, artwork_id);
    if (artwork == nullptr) {
      continue;
    }
    artworks->push_back(*artwork);
  }
  *selected_count = static_cast<int>(artworks->size());
  return !artworks->empty();
}

ImportedArtworkOperationResult
DeleteImportedElementsFromArtwork(CanvasState &state,
                                  const int imported_artwork_id,
                                  const std::unordered_set<int> &path_ids,
                                  const std::unordered_set<int> &text_ids) {
  ImportedArtworkOperationResult result;
  result.artwork_id = imported_artwork_id;

  ImportedArtwork *artwork = FindImportedArtwork(state, imported_artwork_id);
  if (artwork == nullptr) {
    result.message = "Imported artwork was not found.";
    return result;
  }

  const int deleted_count = CountImportedElementIds(path_ids, text_ids);
  result.selected_count = deleted_count;
  if (deleted_count == 0) {
    PopulateOperationReadiness(&result, *artwork);
    result.message = "No imported elements were selected.";
    return result;
  }

  SetLastImportedOperationIssueElements(state, 0, {});
  ClearImportedArtworkPreviewStatesForArtwork(state, imported_artwork_id);
  std::erase_if(artwork->paths, [&path_ids](const ImportedPath &path) {
    return path_ids.contains(path.id);
  });
  std::erase_if(artwork->dxf_text, [&text_ids](const ImportedDxfText &text) {
    return text_ids.contains(text.id);
  });

  PruneEmptyGroups(*artwork);
  ResetImportedArtworkCounters(*artwork);
  RecomputeImportedArtworkBounds(*artwork);
  RecomputeImportedHierarchyBounds(*artwork);
  RefreshImportedArtworkPartMetadata(*artwork);

  const bool source_empty = artwork->paths.empty() && artwork->dxf_text.empty();
  if (source_empty) {
    DeleteImportedArtwork(state, imported_artwork_id);
    ClearSelectedImportedElements(state);
    ClearImportedDebugSelection(state);
    result.success = true;
    result.message =
        "Deleted " + std::to_string(deleted_count) + " imported element" +
        (deleted_count == 1 ? std::string() : std::string("s")) + ".";
    return result;
  }

  ClearSelectedImportedElements(state);
  SetSingleSelectedImportedArtworkObject(state, imported_artwork_id);
  state.selected_imported_debug = {ImportedDebugSelectionKind::Artwork,
                                   imported_artwork_id, 0};

  result.success = true;
  PopulateOperationReadiness(&result, *artwork);
  result.message =
      "Deleted " + std::to_string(deleted_count) + " imported element" +
      (deleted_count == 1 ? std::string() : std::string("s")) + ".";
  return result;
}

} // namespace detail

using namespace detail;

bool IsImportedArtworkScaleRatioLocked(const ImportedArtwork &artwork) {
  return HasImportedArtworkFlag(artwork.flags,
                                ImportedArtworkFlagLockScaleRatio);
}

void UpdateImportedArtworkScaleFromTarget(ImportedArtwork &artwork,
                                          const ImVec2 &target_scale) {
  operations::detail::UpdateImportedArtworkScaleFromTargetShared(artwork,
                                                                 target_scale);
}

std::vector<int>
ResolveImportedArtworkOperationTargets(const CanvasState &state,
                                       const int fallback_artwork_id) {
  if (state.selection_scope == ImportedArtworkSelectionScope::Canvas) {
    std::vector<int> selected_artwork_ids =
        GetSelectedImportedArtworkObjects(state);
    if (!selected_artwork_ids.empty()) {
      return selected_artwork_ids;
    }
  }

  if (fallback_artwork_id != 0) {
    return {fallback_artwork_id};
  }
  if (state.selected_imported_artwork_id != 0) {
    return {state.selected_imported_artwork_id};
  }
  return {};
}

ImportedArtworkOperationResult ApplyImportedArtworkOperationToSelection(
    CanvasState &state, const int fallback_artwork_id,
    const char *operation_name,
    const std::function<ImportedArtworkOperationResult(CanvasState &, int)>
        &operation) {
  const std::vector<int> target_artwork_ids =
      ResolveImportedArtworkOperationTargets(state, fallback_artwork_id);
  if (target_artwork_ids.empty()) {
    ImportedArtworkOperationResult result;
    result.message = "Select one or more imported artworks.";
    state.last_imported_operation_issue_artwork_id = 0;
    state.last_imported_operation_issue_elements.clear();
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  if (target_artwork_ids.size() == 1) {
    return operation(state, target_artwork_ids.front());
  }

  ImportedArtworkOperationResult aggregate;
  aggregate.cut_ready = true;
  aggregate.nest_ready = true;

  int success_count = 0;
  std::vector<std::string> failure_messages;
  failure_messages.reserve(target_artwork_ids.size());
  for (const int artwork_id : target_artwork_ids) {
    const ImportedArtworkOperationResult result = operation(state, artwork_id);
    aggregate.artwork_id = artwork_id;
    AccumulateImportedArtworkOperationResult(&aggregate, result);
    if (result.success) {
      success_count += 1;
      continue;
    }

    const ImportedArtwork *artwork = FindImportedArtwork(state, artwork_id);
    failure_messages.push_back(ImportedArtworkOperationTargetLabel(artwork) +
                               ": " + result.message);
  }

  aggregate.success = failure_messages.empty();
  aggregate.selected_count = static_cast<int>(target_artwork_ids.size());
  aggregate.artwork_id = fallback_artwork_id;
  aggregate.message =
      std::string(operation_name) + ": completed for " +
      std::to_string(success_count) + " of " +
      std::to_string(target_artwork_ids.size()) + " selected artwork" +
      (target_artwork_ids.size() == 1 ? std::string() : std::string("s")) + ".";
  if (!failure_messages.empty()) {
    aggregate.message += " Failures: ";
    for (size_t index = 0; index < failure_messages.size(); ++index) {
      if (index != 0) {
        aggregate.message += " | ";
      }
      aggregate.message += failure_messages[index];
    }
  }

  state.last_imported_operation_issue_artwork_id = 0;
  state.last_imported_operation_issue_elements.clear();
  SetLastImportedArtworkOperation(state, aggregate);
  return aggregate;
}

bool HideAllImportedArtwork(CanvasState &state) {
  if (state.imported_artwork.empty()) {
    state.last_imported_artwork_operation = {
        .success = false,
        .message = "No imported artwork is available.",
    };
    return false;
  }

  ScopedUndoTransaction undo_transaction(state, "Hide all imported artwork");
  int hidden_count = 0;
  for (ImportedArtwork &artwork : state.imported_artwork) {
    if (!artwork.visible) {
      continue;
    }
    artwork.visible = false;
    ClearImportedArtworkPreviewStatesForArtwork(state, artwork.id);
    hidden_count += 1;
  }

  state.last_imported_artwork_operation = {
      .success = hidden_count > 0,
      .selected_count = hidden_count,
      .message = hidden_count > 0 ? "Hidden all imported artwork."
                                  : "All imported artwork is already hidden.",
  };
  return hidden_count > 0;
}

bool ShowAllImportedArtwork(CanvasState &state) {
  if (state.imported_artwork.empty()) {
    state.last_imported_artwork_operation = {
        .success = false,
        .message = "No imported artwork is available.",
    };
    return false;
  }

  ScopedUndoTransaction undo_transaction(state, "Show all imported artwork");
  int shown_count = 0;
  for (ImportedArtwork &artwork : state.imported_artwork) {
    if (artwork.visible) {
      continue;
    }
    artwork.visible = true;
    shown_count += 1;
  }

  state.last_imported_artwork_operation = {
      .success = shown_count > 0,
      .selected_count = shown_count,
      .message = shown_count > 0 ? "Showing all imported artwork."
                                 : "All imported artwork is already visible.",
  };
  return shown_count > 0;
}

bool HideSelectedImportedArtwork(CanvasState &state) {
  const std::vector<int> target_artwork_ids =
      ResolveImportedArtworkOperationTargets(
          state, state.selected_imported_artwork_id);
  if (target_artwork_ids.empty()) {
    state.last_imported_artwork_operation = {
        .success = false,
        .message = "Select one or more imported artworks.",
    };
    return false;
  }

  ScopedUndoTransaction undo_transaction(state,
                                         "Hide selected imported artwork");
  int hidden_count = 0;
  for (const int artwork_id : target_artwork_ids) {
    ImportedArtwork *artwork = FindImportedArtwork(state, artwork_id);
    if (artwork == nullptr || !artwork->visible) {
      continue;
    }
    artwork->visible = false;
    ClearImportedArtworkPreviewStatesForArtwork(state, artwork_id);
    hidden_count += 1;
  }

  state.last_imported_artwork_operation = {
      .success = hidden_count > 0,
      .selected_count = hidden_count,
      .message = hidden_count > 0
                     ? "Hidden selected imported artwork."
                     : "Selected imported artwork is already hidden.",
  };
  return hidden_count > 0;
}

bool IsolateSelectedImportedArtwork(CanvasState &state) {
  const std::vector<int> target_artwork_ids =
      ResolveImportedArtworkOperationTargets(
          state, state.selected_imported_artwork_id);
  if (target_artwork_ids.empty()) {
    state.last_imported_artwork_operation = {
        .success = false,
        .message = "Select one or more imported artworks.",
    };
    return false;
  }

  ScopedUndoTransaction undo_transaction(state,
                                         "Isolate selected imported artwork");
  const std::unordered_set<int> visible_ids(target_artwork_ids.begin(),
                                            target_artwork_ids.end());
  int changed_count = 0;
  for (ImportedArtwork &artwork : state.imported_artwork) {
    const bool next_visible = visible_ids.contains(artwork.id);
    if (artwork.visible == next_visible) {
      continue;
    }
    artwork.visible = next_visible;
    ClearImportedArtworkPreviewStatesForArtwork(state, artwork.id);
    changed_count += 1;
  }

  state.last_imported_artwork_operation = {
      .success = true,
      .selected_count = static_cast<int>(target_artwork_ids.size()),
      .message = "Isolated selected imported artwork.",
  };
  return changed_count > 0;
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

bool HasExtractableImportedDebugSelection(const CanvasState &state,
                                          const ImportedArtwork &artwork) {
  if (state.selected_imported_debug.artwork_id != artwork.id) {
    return false;
  }

  switch (state.selected_imported_debug.kind) {
  case ImportedDebugSelectionKind::Group:
    return state.selected_imported_debug.item_id != artwork.root_group_id &&
           FindImportedGroup(artwork, state.selected_imported_debug.item_id) !=
               nullptr;
  case ImportedDebugSelectionKind::Path:
    return FindImportedPath(artwork, state.selected_imported_debug.item_id) !=
           nullptr;
  case ImportedDebugSelectionKind::DxfText:
    return FindImportedDxfText(
               artwork, state.selected_imported_debug.item_id) != nullptr;
  case ImportedDebugSelectionKind::Artwork:
  case ImportedDebugSelectionKind::None:
    return false;
  }
  return false;
}

bool HasGroupableImportedElementSelection(const CanvasState &state,
                                          const ImportedArtwork &artwork) {
  return state.selected_imported_artwork_id == artwork.id &&
         state.selected_imported_elements.size() >= 2;
}

bool HasGroupableImportedRootSelection(const CanvasState &state,
                                       const ImportedArtwork &artwork) {
  if (state.selected_imported_artwork_id != artwork.id ||
      !state.selected_imported_elements.empty()) {
    return false;
  }

  if (state.selected_imported_debug.artwork_id != artwork.id) {
    return false;
  }

  const bool artwork_selected =
      state.selected_imported_debug.kind == ImportedDebugSelectionKind::Artwork;
  const bool root_group_selected =
      state.selected_imported_debug.kind == ImportedDebugSelectionKind::Group &&
      state.selected_imported_debug.item_id == artwork.root_group_id;
  return (artwork_selected || root_group_selected) &&
         CountGroupableImportedRootItems(artwork) >= 2;
}

bool HasGroupableImportedArtworkSelection(const CanvasState &state) {
  return CountSelectedImportedArtworkObjects(state) >= 2;
}

bool HasSingleWrappedTopLevelImportedGroup(const ImportedArtwork &artwork) {
  const ImportedGroup *root_group =
      FindImportedGroup(artwork, artwork.root_group_id);
  return root_group != nullptr && root_group->child_group_ids.size() == 1 &&
         root_group->path_ids.empty() && root_group->dxf_text_ids.empty();
}

bool HasUngroupableImportedArtworkSelection(const CanvasState &state,
                                            const ImportedArtwork &artwork) {
  if (CountSelectedImportedArtworkObjects(state) != 1 ||
      state.selected_imported_debug.artwork_id != artwork.id) {
    return false;
  }

  const bool artwork_selected =
      state.selected_imported_debug.kind == ImportedDebugSelectionKind::Artwork;
  const bool root_group_selected =
      state.selected_imported_debug.kind == ImportedDebugSelectionKind::Group &&
      state.selected_imported_debug.item_id == artwork.root_group_id;
  if (!artwork_selected && !root_group_selected) {
    return false;
  }

  const ImportedGroup *root_group =
      FindImportedGroup(artwork, artwork.root_group_id);
  return (root_group != nullptr && !root_group->child_group_ids.empty() &&
          artwork.part.contributing_source_artwork_ids.size() >= 2) ||
         HasSingleWrappedTopLevelImportedGroup(artwork);
}

bool HasUngroupableImportedDebugSelection(const CanvasState &state,
                                          const ImportedArtwork &artwork) {
  return state.selected_imported_debug.artwork_id == artwork.id &&
         state.selected_imported_debug.kind ==
             ImportedDebugSelectionKind::Group &&
         state.selected_imported_debug.item_id != artwork.root_group_id &&
         FindImportedGroup(artwork, state.selected_imported_debug.item_id) !=
             nullptr;
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

ImportedArtworkOperationResult SelectImportedPathsInWorldRect(
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

  result.selected_count =
      static_cast<int>(state.selected_imported_elements.size());
  result.success = result.selected_count > 0;
  SetSingleSelectedImportedArtworkObject(state, artwork->id);
  PopulateOperationReadiness(&result, *artwork);
  result.message =
      "Selected " + std::to_string(result.selected_count) +
      " enclosed imported path" +
      (result.selected_count == 1 ? std::string() : std::string("s")) + ".";
  if (result.skipped_count > 0) {
    result.message +=
        " Skipped " + std::to_string(result.skipped_count) + " crossing path" +
        (result.skipped_count == 1 ? std::string() : std::string("s")) + ".";
  }
  if (!state.selected_imported_elements.empty()) {
    state.selected_imported_debug = {
        ImportedDebugSelectionKind::Path, artwork->id,
        state.selected_imported_elements.front().item_id};
  } else {
    state.selected_imported_debug = {ImportedDebugSelectionKind::Artwork,
                                     artwork->id, 0};
  }
  SetLastImportedOperationIssueElements(state, artwork->id,
                                        std::move(skipped_elements));
  SetLastImportedArtworkOperation(state, result);
  return result;
}

ImportedArtworkOperationResult
PreviewSeparateImportedArtworkByGuide(CanvasState &state,
                                      int imported_artwork_id, int guide_id) {
  ImportedArtworkOperationResult result;
  result.artwork_id = imported_artwork_id;

  ImportedArtwork *artwork = FindImportedArtwork(state, imported_artwork_id);
  const Guide *guide = FindGuide(state, guide_id);
  if (artwork == nullptr || guide == nullptr) {
    result.message =
        "Guide split preview requires a valid guide and imported artwork.";
    ClearImportedArtworkSeparationPreviewState(state);
    SetLastImportedOperationIssueElements(state, 0, {});
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  GuideSplitPreviewPlan plan =
      BuildGuideSplitPreviewPlan(*artwork, state, guide_id);
  ClearImportedArtworkAutoCutPreviewState(state);
  state.imported_artwork_separation_preview.active = true;
  state.imported_artwork_separation_preview.artwork_id = imported_artwork_id;
  state.imported_artwork_separation_preview.guide_id = guide_id;
  state.selected_guide_id = guide_id;
  state.imported_artwork_separation_preview.guide_ids = plan.guide_ids;
  state.imported_artwork_separation_preview.future_object_count =
      static_cast<int>(plan.buckets.size());
  state.imported_artwork_separation_preview.skipped_count =
      static_cast<int>(plan.skipped_elements.size());
  state.imported_artwork_separation_preview.skipped_elements =
      plan.skipped_elements;
  state.imported_artwork_separation_preview.parts =
      std::move(plan.preview_parts);

  PopulateOperationReadiness(&result, *artwork);
  result.success =
      state.imported_artwork_separation_preview.future_object_count > 0;
  result.skipped_count =
      state.imported_artwork_separation_preview.skipped_count;
  result.created_artwork_id = 0;
  result.message =
      "Previewing guide-band split across " +
      std::to_string(
          state.imported_artwork_separation_preview.guide_ids.size()) +
      " guide" +
      (state.imported_artwork_separation_preview.guide_ids.size() == 1
           ? std::string()
           : std::string("s")) +
      " into " +
      std::to_string(
          state.imported_artwork_separation_preview.future_object_count) +
      " object" +
      (state.imported_artwork_separation_preview.future_object_count == 1
           ? std::string()
           : std::string("s")) +
      ".";
  if (result.skipped_count > 0) {
    result.message +=
        " Skipped " + std::to_string(result.skipped_count) + " element" +
        (result.skipped_count == 1 ? std::string() : std::string("s")) + ".";
  }
  state.imported_artwork_separation_preview.message = result.message;
  SetLastImportedOperationIssueElements(state, artwork->id,
                                        plan.skipped_elements);
  SetLastImportedArtworkOperation(state, result);
  return result;
}

void ClearImportedArtworkSeparationPreview(CanvasState &state) {
  ClearImportedArtworkSeparationPreviewState(state);
}

ImportedArtworkOperationResult
PreviewImportedArtworkAutoCut(CanvasState &state, int imported_artwork_id,
                              AutoCutPreviewAxisMode axis_mode,
                              float minimum_gap) {
  ImportedArtworkOperationResult result;
  result.artwork_id = imported_artwork_id;

  ImportedArtwork *artwork = FindImportedArtwork(state, imported_artwork_id);
  if (artwork == nullptr) {
    result.message = "Auto cut preview requires a valid imported artwork.";
    ClearImportedArtworkAutoCutPreviewState(state);
    SetLastImportedOperationIssueElements(state, 0, {});
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  AutoCutPreviewPlan plan = BuildAutoCutPreviewPlan(
      *artwork, axis_mode, NormalizeAutoCutMinimumGap(minimum_gap));
  const int inferred_cut_count =
      static_cast<int>(plan.layout.vertical_positions.size() +
                       plan.layout.horizontal_positions.size());

  if (inferred_cut_count == 0) {
    ClearImportedArtworkAutoCutPreviewState(state);
    PopulateOperationReadiness(&result, *artwork);
    result.skipped_count = static_cast<int>(plan.skipped_elements.size());
    result.message = "Auto cut preview did not find any gaps wide enough for "
                     "inferred cut bands.";
    SetLastImportedOperationIssueElements(state, artwork->id,
                                          std::move(plan.skipped_elements));
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  ClearImportedArtworkSeparationPreviewState(state);
  state.imported_artwork_auto_cut_preview.active = true;
  state.imported_artwork_auto_cut_preview.artwork_id = imported_artwork_id;
  state.imported_artwork_auto_cut_preview.axis_mode = axis_mode;
  state.imported_artwork_auto_cut_preview.minimum_gap =
      NormalizeAutoCutMinimumGap(minimum_gap);
  state.imported_artwork_auto_cut_preview.vertical_positions =
      plan.layout.vertical_positions;
  state.imported_artwork_auto_cut_preview.horizontal_positions =
      plan.layout.horizontal_positions;
  state.imported_artwork_auto_cut_preview.future_band_count =
      static_cast<int>(plan.buckets.size());
  state.imported_artwork_auto_cut_preview.skipped_count =
      static_cast<int>(plan.skipped_elements.size());
  state.imported_artwork_auto_cut_preview.skipped_elements =
      plan.skipped_elements;
  state.imported_artwork_auto_cut_preview.parts = std::move(plan.preview_parts);

  PopulateOperationReadiness(&result, *artwork);
  result.success =
      state.imported_artwork_auto_cut_preview.future_band_count > 0;
  result.skipped_count = state.imported_artwork_auto_cut_preview.skipped_count;
  result.message =
      "Previewing auto cut using " +
      std::to_string(
          state.imported_artwork_auto_cut_preview.vertical_positions.size()) +
      " vertical and " +
      std::to_string(
          state.imported_artwork_auto_cut_preview.horizontal_positions.size()) +
      " horizontal inferred cuts across " +
      std::to_string(
          state.imported_artwork_auto_cut_preview.future_band_count) +
      " band" +
      (state.imported_artwork_auto_cut_preview.future_band_count == 1
           ? std::string()
           : std::string("s")) +
      ".";
  if (result.skipped_count > 0) {
    result.message +=
        " Skipped " + std::to_string(result.skipped_count) + " element" +
        (result.skipped_count == 1 ? std::string() : std::string("s")) + ".";
  }
  state.imported_artwork_auto_cut_preview.message = result.message;
  SetLastImportedOperationIssueElements(state, artwork->id,
                                        std::move(plan.skipped_elements));
  SetLastImportedArtworkOperation(state, result);
  return result;
}

ImportedArtworkOperationResult
ApplyImportedArtworkAutoCut(CanvasState &state, int imported_artwork_id,
                            AutoCutPreviewAxisMode axis_mode, float minimum_gap,
                            bool create_groups_from_cuts) {
  ImportedArtworkOperationResult result;
  result.artwork_id = imported_artwork_id;

  ImportedArtwork *artwork = FindImportedArtwork(state, imported_artwork_id);
  if (artwork == nullptr) {
    result.message = "Auto cut requires a valid imported artwork.";
    SetLastImportedOperationIssueElements(state, 0, {});
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  AutoCutPreviewPlan plan = BuildAutoCutPreviewPlan(
      *artwork, axis_mode, NormalizeAutoCutMinimumGap(minimum_gap));
  const int inferred_cut_count =
      static_cast<int>(plan.layout.vertical_positions.size() +
                       plan.layout.horizontal_positions.size());
  result.skipped_count = static_cast<int>(plan.skipped_elements.size());

  if (inferred_cut_count == 0 || plan.buckets.size() <= 1) {
    PopulateOperationReadiness(&result, *artwork);
    result.message = "Auto cut needs movable content in more than one inferred "
                     "band.";
    SetLastImportedOperationIssueElements(state, artwork->id,
                                          std::move(plan.skipped_elements));
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  ScopedUndoTransaction undo_transaction(state,
                                         "Apply imported artwork auto cut");
  result = MoveImportedElementsToNewArtworks(state, imported_artwork_id,
                                             plan.buckets, " Auto Cut", "Cut",
                                             create_groups_from_cuts);
  result.skipped_count = static_cast<int>(plan.skipped_elements.size());
  if (result.skipped_count > 0) {
    result.message +=
        " Skipped " + std::to_string(result.skipped_count) + " element" +
        (result.skipped_count == 1 ? std::string() : std::string("s")) +
        " that crossed an inferred cut band.";
  }
  ClearImportedArtworkAutoCutPreviewState(state);
  SetLastImportedOperationIssueElements(state, imported_artwork_id,
                                        std::move(plan.skipped_elements));
  SetLastImportedArtworkOperation(state, result);
  return result;
}

void ClearImportedArtworkAutoCutPreview(CanvasState &state) {
  ClearImportedArtworkAutoCutPreviewState(state);
}

bool CanCopySelectionToClipboard(const CanvasState &state) {
  std::vector<ImportedArtwork> clipboard_artworks;
  int selected_count = 0;
  return CollectSelectionForClipboard(state, &clipboard_artworks,
                                      &selected_count);
}

bool HasClipboardContent(const CanvasState &state) {
  return state.clipboard.has_content();
}

ImportedArtworkOperationResult CopySelectedToClipboard(CanvasState &state) {
  ImportedArtworkOperationResult result;

  std::vector<ImportedArtwork> clipboard_artworks;
  int selected_count = 0;
  if (!CollectSelectionForClipboard(state, &clipboard_artworks,
                                    &selected_count)) {
    result.message = "Select imported artwork or imported elements to copy.";
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  state.clipboard.artworks = std::move(clipboard_artworks);
  state.clipboard.paste_generation = 0;
  result.success = true;
  result.selected_count = selected_count;
  result.message = "Copied " + std::to_string(selected_count) +
                   (state.clipboard.artworks.size() == 1 && selected_count > 1
                        ? " imported element"
                        : " imported object") +
                   (selected_count == 1 ? std::string() : std::string("s")) +
                   " to the clipboard.";
  SetLastImportedArtworkOperation(state, result);
  return result;
}

ImportedArtworkOperationResult
DeleteSelectedImportedContent(CanvasState &state) {
  ImportedArtworkOperationResult result;

  if (state.selected_imported_artwork_id != 0) {
    if (ImportedArtwork *artwork =
            FindImportedArtwork(state, state.selected_imported_artwork_id);
        artwork != nullptr) {
      std::unordered_set<int> path_ids;
      std::unordered_set<int> text_ids;
      if (state.selected_imported_artwork_id == artwork->id &&
          !state.selected_imported_elements.empty()) {
        for (const ImportedElementSelection &selection :
             state.selected_imported_elements) {
          if (selection.kind == ImportedElementKind::Path) {
            if (FindImportedPath(*artwork, selection.item_id) != nullptr) {
              path_ids.insert(selection.item_id);
            }
          } else if (FindImportedDxfText(*artwork, selection.item_id) !=
                     nullptr) {
            text_ids.insert(selection.item_id);
          }
        }
      }

      if ((path_ids.empty() && text_ids.empty()) &&
          CollectDebugSelectionElementIds(state, *artwork, &path_ids,
                                          &text_ids)) {
        ScopedUndoTransaction undo_transaction(
            state, "Delete selected imported elements");
        result = DeleteImportedElementsFromArtwork(state, artwork->id, path_ids,
                                                   text_ids);
        SetLastImportedArtworkOperation(state, result);
        return result;
      }
    }
  }

  const std::vector<int> target_artwork_ids =
      GetSelectedImportedArtworkObjects(state);
  if (target_artwork_ids.empty()) {
    result.message = "Select imported artwork or imported elements to delete.";
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  ScopedUndoTransaction undo_transaction(state,
                                         "Delete selected imported artwork");
  int deleted_count = 0;
  for (const int artwork_id : target_artwork_ids) {
    if (operations::detail::DeleteImportedArtworkShared(state, artwork_id)) {
      deleted_count += 1;
    }
  }

  result.success = deleted_count > 0;
  result.selected_count = deleted_count;
  result.message =
      deleted_count > 0
          ? "Deleted " + std::to_string(deleted_count) + " imported object" +
                (deleted_count == 1 ? std::string() : std::string("s")) + "."
          : "No imported artwork was deleted.";
  SetLastImportedArtworkOperation(state, result);
  return result;
}

ImportedArtworkOperationResult CutSelectedToClipboard(CanvasState &state) {
  ImportedArtworkOperationResult copy_result = CopySelectedToClipboard(state);
  if (!copy_result.success) {
    return copy_result;
  }

  ImportedArtworkOperationResult delete_result =
      DeleteSelectedImportedContent(state);
  if (!delete_result.success) {
    return delete_result;
  }

  delete_result.message = "Cut selection to the clipboard.";
  SetLastImportedArtworkOperation(state, delete_result);
  return delete_result;
}

namespace {

bool TryGetClipboardWorldBounds(const CanvasClipboard &clipboard,
                                ImVec2 *world_min, ImVec2 *world_max) {
  if (world_min == nullptr || world_max == nullptr ||
      clipboard.artworks.empty()) {
    return false;
  }

  bool has_bounds = false;
  for (const ImportedArtwork &artwork : clipboard.artworks) {
    ImVec2 artwork_world_min;
    ImVec2 artwork_world_max;
    ImportedLocalBoundsToWorldBounds(artwork, artwork.bounds_min,
                                     artwork.bounds_max, &artwork_world_min,
                                     &artwork_world_max);
    if (!has_bounds) {
      *world_min = artwork_world_min;
      *world_max = artwork_world_max;
      has_bounds = true;
      continue;
    }

    world_min->x = std::min(world_min->x, artwork_world_min.x);
    world_min->y = std::min(world_min->y, artwork_world_min.y);
    world_max->x = std::max(world_max->x, artwork_world_max.x);
    world_max->y = std::max(world_max->y, artwork_world_max.y);
  }

  return has_bounds;
}

} // namespace

ImportedArtworkOperationResult PasteFromClipboard(CanvasState &state) {
  ImportedArtworkOperationResult result;
  if (!state.clipboard.has_content()) {
    result.message = "Clipboard is empty.";
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  ScopedUndoTransaction undo_transaction(state, "Paste imported artwork");
  state.clipboard.paste_generation += 1;
  const float offset =
      24.0f * static_cast<float>(state.clipboard.paste_generation);
  ImVec2 translation(offset, offset);
  ImVec2 clipboard_world_min;
  ImVec2 clipboard_world_max;
  if (state.runtime.has_cursor_world &&
      TryGetClipboardWorldBounds(state.clipboard, &clipboard_world_min,
                                 &clipboard_world_max)) {
    const ImVec2 clipboard_center(
        (clipboard_world_min.x + clipboard_world_max.x) * 0.5f,
        (clipboard_world_min.y + clipboard_world_max.y) * 0.5f);
    translation = ImVec2(state.runtime.cursor_world.x - clipboard_center.x,
                         state.runtime.cursor_world.y - clipboard_center.y);
  }

  std::vector<int> pasted_artwork_ids;
  pasted_artwork_ids.reserve(state.clipboard.artworks.size());
  for (ImportedArtwork source_artwork : state.clipboard.artworks) {
    source_artwork.id = 0;
    source_artwork.part.part_id = 0;
    source_artwork.origin.x += translation.x;
    source_artwork.origin.y += translation.y;

    const std::string original_name = source_artwork.name;
    if (!original_name.empty() &&
        original_name.find("Copy") == std::string::npos) {
      source_artwork.name = original_name + " Copy";
      if (ImportedGroup *root_group =
              FindImportedGroup(source_artwork, source_artwork.root_group_id);
          root_group != nullptr) {
        root_group->label = source_artwork.name;
      }
    }

    const int pasted_artwork_id =
        AppendImportedArtwork(state, std::move(source_artwork), false);
    if (pasted_artwork_id != 0) {
      pasted_artwork_ids.push_back(pasted_artwork_id);
    }
  }

  if (pasted_artwork_ids.empty()) {
    result.message = "Clipboard contents could not be pasted.";
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  ClearSelectedImportedElements(state);
  state.selected_imported_artwork_ids = pasted_artwork_ids;
  state.selected_imported_artwork_id = pasted_artwork_ids.front();
  state.selected_imported_debug = {ImportedDebugSelectionKind::Artwork,
                                   state.selected_imported_artwork_id, 0};

  result.success = true;
  result.selected_count = static_cast<int>(pasted_artwork_ids.size());
  result.artwork_id = state.selected_imported_artwork_id;
  result.created_artwork_id = state.selected_imported_artwork_id;
  if (const ImportedArtwork *artwork =
          FindImportedArtwork(state, state.selected_imported_artwork_id);
      artwork != nullptr) {
    PopulateOperationReadiness(&result, *artwork);
  }
  result.message =
      "Pasted " + std::to_string(pasted_artwork_ids.size()) +
      " imported object" +
      (pasted_artwork_ids.size() == 1 ? std::string() : std::string("s")) + ".";
  SetLastImportedArtworkOperation(state, result);
  return result;
}

ImportedArtworkOperationResult
ExtractSelectedImportedElements(CanvasState &state, int imported_artwork_id) {
  ScopedUndoTransaction undo_transaction(state,
                                         "Extract selected imported elements");
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
GroupSelectedImportedElements(CanvasState &state, int imported_artwork_id) {
  ImportedArtworkOperationResult result;
  result.artwork_id = imported_artwork_id;

  ImportedArtwork *artwork = FindImportedArtwork(state, imported_artwork_id);
  if (artwork == nullptr) {
    result.message = "Imported artwork was not found.";
    SetLastImportedOperationIssueElements(state, 0, {});
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  std::unordered_set<int> group_ids;
  std::unordered_set<int> path_ids;
  std::unordered_set<int> text_ids;
  for (const ImportedElementSelection &selection :
       state.selected_imported_elements) {
    if (selection.kind == ImportedElementKind::Path) {
      if (FindImportedPath(*artwork, selection.item_id) != nullptr) {
        path_ids.insert(selection.item_id);
      }
    } else if (FindImportedDxfText(*artwork, selection.item_id) != nullptr) {
      text_ids.insert(selection.item_id);
    }
  }

  result.selected_count = CountGroupingTargets(group_ids, path_ids, text_ids);
  if (result.selected_count <= 1) {
    PopulateOperationReadiness(&result, *artwork);
    result.message = "Grouping needs at least two selected imported elements.";
    SetLastImportedOperationIssueElements(state, 0, {});
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  const int parent_group_id =
      ResolveGroupingParentGroupId(*artwork, path_ids, text_ids);
  ImportedGroup *parent_group = FindImportedGroup(*artwork, parent_group_id);
  if (parent_group == nullptr) {
    result.message = "Imported grouping could not resolve a parent group.";
    SetLastImportedOperationIssueElements(state, 0, {});
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  ScopedUndoTransaction undo_transaction(state,
                                         "Group selected imported elements");
  ClearImportedArtworkPreviewStatesForArtwork(state, imported_artwork_id);
  SetLastImportedOperationIssueElements(state, 0, {});

  ImportedGroup new_group;
  new_group.id = artwork->next_group_id++;
  new_group.parent_group_id = parent_group_id;
  new_group.label = "Group " + std::to_string(new_group.id);
  MoveImportedGroupingTargetsToGroup(artwork, parent_group, group_ids, path_ids,
                                     text_ids, &new_group);
  artwork->groups.push_back(std::move(new_group));

  RecomputeImportedHierarchyBounds(*artwork);
  RefreshImportedArtworkPartMetadata(*artwork);
  ClearSelectedImportedElements(state);
  SetSingleSelectedImportedArtworkObject(state, imported_artwork_id);
  state.selected_imported_debug = {ImportedDebugSelectionKind::Group,
                                   imported_artwork_id,
                                   artwork->groups.back().id};

  result.success = true;
  result.created_artwork_id = artwork->groups.back().id;
  PopulateOperationReadiness(&result, *artwork);
  result.message =
      "Grouped " + std::to_string(result.selected_count) + " imported element" +
      (result.selected_count == 1 ? std::string() : std::string("s")) +
      " into a new group.";
  SetLastImportedArtworkOperation(state, result);
  return result;
}

ImportedArtworkOperationResult
GroupImportedArtworkRootContents(CanvasState &state, int imported_artwork_id) {
  ImportedArtworkOperationResult result;
  result.artwork_id = imported_artwork_id;

  ImportedArtwork *artwork = FindImportedArtwork(state, imported_artwork_id);
  if (artwork == nullptr) {
    result.message = "Imported artwork was not found.";
    SetLastImportedOperationIssueElements(state, 0, {});
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  ImportedGroup *root_group =
      FindImportedGroup(*artwork, artwork->root_group_id);
  if (root_group == nullptr) {
    PopulateOperationReadiness(&result, *artwork);
    result.message = "Imported artwork root group was not found.";
    SetLastImportedOperationIssueElements(state, 0, {});
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  std::unordered_set<int> group_ids(root_group->child_group_ids.begin(),
                                    root_group->child_group_ids.end());
  std::unordered_set<int> path_ids(root_group->path_ids.begin(),
                                   root_group->path_ids.end());
  std::unordered_set<int> text_ids(root_group->dxf_text_ids.begin(),
                                   root_group->dxf_text_ids.end());

  result.selected_count = CountGroupingTargets(group_ids, path_ids, text_ids);
  if (result.selected_count <= 1) {
    PopulateOperationReadiness(&result, *artwork);
    result.message = "Grouping artwork contents needs at least two root-level "
                     "imported items.";
    SetLastImportedOperationIssueElements(state, 0, {});
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  ScopedUndoTransaction undo_transaction(
      state, "Group imported artwork root contents");
  ClearImportedArtworkPreviewStatesForArtwork(state, imported_artwork_id);
  SetLastImportedOperationIssueElements(state, 0, {});

  ImportedGroup new_group;
  new_group.id = artwork->next_group_id++;
  new_group.parent_group_id = artwork->root_group_id;
  new_group.label = "Group " + std::to_string(new_group.id);

  MoveImportedGroupingTargetsToGroup(artwork, root_group, group_ids, path_ids,
                                     text_ids, &new_group);
  artwork->groups.push_back(std::move(new_group));

  RecomputeImportedHierarchyBounds(*artwork);
  RefreshImportedArtworkPartMetadata(*artwork);
  ClearSelectedImportedElements(state);
  SetSingleSelectedImportedArtworkObject(state, imported_artwork_id);
  state.selected_imported_debug = {ImportedDebugSelectionKind::Group,
                                   imported_artwork_id,
                                   artwork->groups.back().id};

  result.success = true;
  result.created_artwork_id = artwork->groups.back().id;
  PopulateOperationReadiness(&result, *artwork);
  result.message =
      "Grouped " + std::to_string(result.selected_count) +
      " root imported item" +
      (result.selected_count == 1 ? std::string() : std::string("s")) +
      " into a new top-level group.";
  SetLastImportedArtworkOperation(state, result);
  return result;
}

ImportedArtworkOperationResult
GroupSelectedImportedArtworkObjects(CanvasState &state) {
  ImportedArtworkOperationResult result;

  const std::vector<int> target_artwork_ids =
      GetSelectedImportedArtworkObjects(state);
  result.selected_count = static_cast<int>(target_artwork_ids.size());
  if (result.selected_count <= 1) {
    result.message = "Grouping needs at least two selected imported objects.";
    SetLastImportedOperationIssueElements(state, 0, {});
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  std::vector<const ImportedArtwork *> source_artworks;
  source_artworks.reserve(target_artwork_ids.size());
  for (const int artwork_id : target_artwork_ids) {
    const ImportedArtwork *artwork = FindImportedArtwork(state, artwork_id);
    if (artwork == nullptr) {
      result.message = "Imported artwork was not found.";
      SetLastImportedOperationIssueElements(state, 0, {});
      SetLastImportedArtworkOperation(state, result);
      return result;
    }
    source_artworks.push_back(artwork);
  }

  ScopedUndoTransaction undo_transaction(state,
                                         "Group selected imported artwork");
  SetLastImportedOperationIssueElements(state, 0, {});
  ImportedArtwork grouped_artwork =
      BuildArtworkGroupFromSelection(source_artworks);
  const int created_artwork_id =
      AppendImportedArtwork(state, std::move(grouped_artwork), false);
  for (const int artwork_id : target_artwork_ids) {
    DeleteImportedArtwork(state, artwork_id);
  }

  ClearSelectedImportedElements(state);
  SetSingleSelectedImportedArtworkObject(state, created_artwork_id);
  state.selected_imported_debug = {ImportedDebugSelectionKind::Artwork,
                                   created_artwork_id, 0};

  result.success = true;
  result.artwork_id = created_artwork_id;
  result.created_artwork_id = created_artwork_id;
  if (const ImportedArtwork *grouped =
          FindImportedArtwork(state, created_artwork_id);
      grouped != nullptr) {
    PopulateOperationReadiness(&result, *grouped);
  }
  result.message =
      "Grouped " + std::to_string(result.selected_count) + " imported object" +
      (result.selected_count == 1 ? std::string() : std::string("s")) +
      " into a new artwork.";
  SetLastImportedArtworkOperation(state, result);
  return result;
}

ImportedArtworkOperationResult
UngroupSelectedImportedArtworkObjects(CanvasState &state) {
  ImportedArtworkOperationResult result;

  if (CountSelectedImportedArtworkObjects(state) != 1 ||
      state.selected_imported_artwork_id == 0) {
    result.message = "Select a grouped imported object to ungroup it.";
    SetLastImportedOperationIssueElements(state, 0, {});
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  const int imported_artwork_id = state.selected_imported_artwork_id;
  ImportedArtwork *artwork = FindImportedArtwork(state, imported_artwork_id);
  if (artwork == nullptr) {
    result.message = "Imported artwork was not found.";
    SetLastImportedOperationIssueElements(state, 0, {});
    SetLastImportedArtworkOperation(state, result);
    return result;
  }
  if (!HasUngroupableImportedArtworkSelection(state, *artwork)) {
    PopulateOperationReadiness(&result, *artwork);
    result.message = "Select a grouped imported object to ungroup it.";
    SetLastImportedOperationIssueElements(state, 0, {});
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  const ImportedGroup *root_group =
      FindImportedGroup(*artwork, artwork->root_group_id);
  if (root_group == nullptr) {
    result.message = "Imported artwork root group was not found.";
    SetLastImportedOperationIssueElements(state, 0, {});
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  if (HasSingleWrappedTopLevelImportedGroup(*artwork)) {
    const int wrapped_group_id = root_group->child_group_ids.front();

    ScopedUndoTransaction undo_transaction(state,
                                           "Ungroup imported artwork contents");
    ClearImportedArtworkPreviewStatesForArtwork(state, imported_artwork_id);
    SetLastImportedOperationIssueElements(state, 0, {});
    if (!UngroupImportedGroupInPlace(artwork, wrapped_group_id)) {
      result.message = "Grouped imported artwork could not be ungrouped.";
      SetLastImportedArtworkOperation(state, result);
      return result;
    }

    ClearSelectedImportedElements(state);
    SetSingleSelectedImportedArtworkObject(state, imported_artwork_id);
    state.selected_imported_debug = {ImportedDebugSelectionKind::Artwork,
                                     imported_artwork_id, 0};

    result.success = true;
    result.artwork_id = imported_artwork_id;
    result.created_artwork_id = imported_artwork_id;
    PopulateOperationReadiness(&result, *artwork);
    result.message = "Ungrouped artwork contents.";
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  struct ArtworkSplitBucket {
    std::unordered_set<int> path_ids;
    std::unordered_set<int> text_ids;
    std::string label;
    int wrapper_group_id = 0;
  };

  std::vector<ArtworkSplitBucket> buckets;
  buckets.reserve(root_group->child_group_ids.size() + 1);
  for (const int child_group_id : root_group->child_group_ids) {
    const ImportedGroup *child_group =
        FindImportedGroup(*artwork, child_group_id);
    if (child_group == nullptr) {
      continue;
    }

    ArtworkSplitBucket bucket;
    bucket.label = child_group->label;
    bucket.wrapper_group_id = child_group_id;
    std::unordered_set<int> visited_group_ids;
    operations::detail::CollectImportedGroupElementIdsShared(
        *artwork, child_group_id, &bucket.path_ids, &bucket.text_ids,
        &visited_group_ids);
    if (!bucket.path_ids.empty() || !bucket.text_ids.empty()) {
      buckets.push_back(std::move(bucket));
    }
  }

  if (!root_group->path_ids.empty() || !root_group->dxf_text_ids.empty()) {
    ArtworkSplitBucket root_bucket;
    root_bucket.label = artwork->name;
    root_bucket.path_ids.insert(root_group->path_ids.begin(),
                                root_group->path_ids.end());
    root_bucket.text_ids.insert(root_group->dxf_text_ids.begin(),
                                root_group->dxf_text_ids.end());
    buckets.push_back(std::move(root_bucket));
  }

  result.selected_count = static_cast<int>(buckets.size());
  if (result.selected_count <= 1) {
    PopulateOperationReadiness(&result, *artwork);
    result.message =
        "Grouped imported object does not contain multiple child objects.";
    SetLastImportedOperationIssueElements(state, 0, {});
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  ScopedUndoTransaction undo_transaction(state,
                                         "Ungroup selected imported artwork");
  ClearImportedArtworkPreviewStatesForArtwork(state, imported_artwork_id);
  SetLastImportedOperationIssueElements(state, 0, {});

  std::vector<int> created_artwork_ids;
  created_artwork_ids.reserve(buckets.size());
  for (const ArtworkSplitBucket &bucket : buckets) {
    ImportedArtwork subset =
        BuildArtworkSubset(*artwork, bucket.path_ids, bucket.text_ids, "");
    if (!bucket.label.empty()) {
      subset.name = bucket.label;
    }
    if (bucket.wrapper_group_id != 0) {
      UngroupImportedGroupInPlace(&subset, bucket.wrapper_group_id);
    }
    SyncImportedArtworkSourceMetadata(&subset);

    const int created_artwork_id =
        AppendImportedArtwork(state, std::move(subset), false);
    if (created_artwork_id != 0) {
      created_artwork_ids.push_back(created_artwork_id);
    }
  }

  DeleteImportedArtwork(state, imported_artwork_id);
  ClearSelectedImportedElements(state);
  state.selected_imported_artwork_ids = created_artwork_ids;
  state.selected_imported_artwork_id =
      created_artwork_ids.empty() ? 0 : created_artwork_ids.front();
  state.selected_imported_debug = {ImportedDebugSelectionKind::Artwork,
                                   state.selected_imported_artwork_id, 0};

  result.success = !created_artwork_ids.empty();
  result.artwork_id = state.selected_imported_artwork_id;
  result.created_artwork_id = state.selected_imported_artwork_id;
  if (const ImportedArtwork *created_artwork =
          FindImportedArtwork(state, state.selected_imported_artwork_id);
      created_artwork != nullptr) {
    PopulateOperationReadiness(&result, *created_artwork);
  }
  result.message =
      "Ungrouped imported object into " +
      std::to_string(created_artwork_ids.size()) + " imported object" +
      (created_artwork_ids.size() == 1 ? std::string() : std::string("s")) +
      ".";
  SetLastImportedArtworkOperation(state, result);
  return result;
}

ImportedArtworkOperationResult
UngroupSelectedImportedGroup(CanvasState &state, int imported_artwork_id) {
  ImportedArtworkOperationResult result;
  result.artwork_id = imported_artwork_id;

  ImportedArtwork *artwork = FindImportedArtwork(state, imported_artwork_id);
  if (artwork == nullptr) {
    result.message = "Imported artwork was not found.";
    SetLastImportedOperationIssueElements(state, 0, {});
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  if (state.selected_imported_debug.artwork_id != imported_artwork_id ||
      state.selected_imported_debug.kind != ImportedDebugSelectionKind::Group) {
    PopulateOperationReadiness(&result, *artwork);
    result.message = "Select a non-root imported group to ungroup it.";
    SetLastImportedOperationIssueElements(state, 0, {});
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  ImportedGroup *group =
      FindImportedGroup(*artwork, state.selected_imported_debug.item_id);
  if (group == nullptr || group->id == artwork->root_group_id ||
      group->parent_group_id == 0) {
    PopulateOperationReadiness(&result, *artwork);
    result.message = "Select a non-root imported group to ungroup it.";
    SetLastImportedOperationIssueElements(state, 0, {});
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  const int group_id = group->id;
  const int parent_group_id = group->parent_group_id;
  const int ungrouped_item_count =
      static_cast<int>(group->path_ids.size() + group->dxf_text_ids.size());

  ScopedUndoTransaction undo_transaction(state, "Ungroup imported group");
  ClearImportedArtworkPreviewStatesForArtwork(state, imported_artwork_id);
  SetLastImportedOperationIssueElements(state, 0, {});
  if (!UngroupImportedGroupInPlace(artwork, group_id)) {
    result.message = "Imported group could not be ungrouped because its parent "
                     "was not found.";
    SetLastImportedOperationIssueElements(state, 0, {});
    SetLastImportedArtworkOperation(state, result);
    return result;
  }
  ClearSelectedImportedElements(state);
  SetSingleSelectedImportedArtworkObject(state, imported_artwork_id);
  if (parent_group_id == artwork->root_group_id) {
    state.selected_imported_debug = {ImportedDebugSelectionKind::Artwork,
                                     imported_artwork_id, 0};
  } else {
    state.selected_imported_debug = {ImportedDebugSelectionKind::Group,
                                     imported_artwork_id, parent_group_id};
  }

  result.success = true;
  result.selected_count = ungrouped_item_count;
  PopulateOperationReadiness(&result, *artwork);
  result.message = "Ungrouped imported group " + std::to_string(group_id) + ".";
  SetLastImportedArtworkOperation(state, result);
  return result;
}

ImportedArtworkOperationResult
SeparateImportedArtworkByGuide(CanvasState &state, int imported_artwork_id,
                               int guide_id, bool create_groups_from_cuts) {
  ImportedArtworkOperationResult result;
  result.artwork_id = imported_artwork_id;

  ImportedArtwork *artwork = FindImportedArtwork(state, imported_artwork_id);
  const Guide *guide = FindGuide(state, guide_id);
  if (artwork == nullptr || guide == nullptr) {
    result.message = "Guide split requires a valid guide and imported artwork.";
    SetLastImportedOperationIssueElements(state, 0, {});
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  GuideSplitPreviewPlan plan =
      BuildGuideSplitPreviewPlan(*artwork, state, guide_id);
  result.skipped_count = static_cast<int>(plan.skipped_elements.size());
  if (plan.buckets.size() <= 1) {
    PopulateOperationReadiness(&result, *artwork);
    result.message =
        "Guide-band split needs movable content in more than one band.";
    SetLastImportedOperationIssueElements(state, artwork->id,
                                          std::move(plan.skipped_elements));
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  ScopedUndoTransaction undo_transaction(state,
                                         "Separate imported artwork by guide");
  result = MoveImportedElementsToNewArtworks(state, imported_artwork_id,
                                             plan.buckets, " Guide Split",
                                             "Split", create_groups_from_cuts);
  result.skipped_count = static_cast<int>(plan.skipped_elements.size());
  if (result.skipped_count > 0) {
    result.message +=
        " Skipped " + std::to_string(result.skipped_count) + " element" +
        (result.skipped_count == 1 ? std::string() : std::string("s")) +
        " that crossed the guide band.";
  }
  ClearImportedArtworkSeparationPreviewState(state);
  SetLastImportedOperationIssueElements(state, imported_artwork_id,
                                        std::move(plan.skipped_elements));
  SetLastImportedArtworkOperation(state, result);
  return result;
}

bool DeleteImportedArtwork(CanvasState &state, int imported_artwork_id) {
  if (FindImportedArtwork(state, imported_artwork_id) == nullptr) {
    return false;
  }
  PushUndoSnapshot(state, "Delete imported artwork");
  return operations::detail::DeleteImportedArtworkShared(state,
                                                         imported_artwork_id);
}

} // namespace im2d