#include "im2d_canvas_document.h"

#include "../common/im2d_log.h"

#include "im2d_canvas_units.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace im2d {

namespace {

constexpr float kMinimumImportedArtworkScale = 0.01f;
constexpr float kImportedCurveFlatteningTolerance = 12.0f;
constexpr float kGuideClassificationEpsilon = 0.25f;

enum class GuideSideClassification {
  Empty,
  Negative,
  Positive,
  Crossing,
};

struct SelectionRect {
  ImVec2 min = ImVec2(0.0f, 0.0f);
  ImVec2 max = ImVec2(0.0f, 0.0f);
};

float DistanceSquared(const ImVec2 &a, const ImVec2 &b) {
  const float dx = a.x - b.x;
  const float dy = a.y - b.y;
  return dx * dx + dy * dy;
}

bool PointsNear(const ImVec2 &a, const ImVec2 &b, float tolerance) {
  return DistanceSquared(a, b) <= tolerance * tolerance;
}

SelectionRect NormalizeRect(const ImVec2 &a, const ImVec2 &b) {
  return {ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)),
          ImVec2(std::max(a.x, b.x), std::max(a.y, b.y))};
}

ImVec2 CubicBezierPoint(const ImVec2 &start, const ImVec2 &control1,
                        const ImVec2 &control2, const ImVec2 &end, float t) {
  const float mt = 1.0f - t;
  const float mt2 = mt * mt;
  const float t2 = t * t;
  return ImVec2(mt2 * mt * start.x + 3.0f * mt2 * t * control1.x +
                    3.0f * mt * t2 * control2.x + t2 * t * end.x,
                mt2 * mt * start.y + 3.0f * mt2 * t * control1.y +
                    3.0f * mt * t2 * control2.y + t2 * t * end.y);
}

void AppendSampledSegmentPointsLocal(
    const std::vector<ImportedPathSegment> &segments,
    std::vector<ImVec2> *sample_points) {
  if (segments.empty()) {
    return;
  }

  sample_points->push_back(segments.front().start);
  for (const ImportedPathSegment &segment : segments) {
    if (segment.kind == ImportedPathSegmentKind::Line) {
      sample_points->push_back(segment.end);
      continue;
    }

    for (int sample_index = 1;
         sample_index <= static_cast<int>(kImportedCurveFlatteningTolerance);
         ++sample_index) {
      const float t =
          static_cast<float>(sample_index) / kImportedCurveFlatteningTolerance;
      sample_points->push_back(CubicBezierPoint(
          segment.start, segment.control1, segment.control2, segment.end, t));
    }
  }
}

void AppendPathSamplePointsWorld(const ImportedArtwork &artwork,
                                 const ImportedPath &path,
                                 std::vector<ImVec2> *sample_points) {
  const size_t start_index = sample_points->size();
  AppendSampledSegmentPointsLocal(path.segments, sample_points);
  for (size_t index = start_index; index < sample_points->size(); ++index) {
    sample_points->at(index) =
        ImportedArtworkPointToWorld(artwork, sample_points->at(index));
  }
}

void AppendTextSamplePointsWorld(const ImportedArtwork &artwork,
                                 const ImportedDxfText &text,
                                 std::vector<ImVec2> *sample_points) {
  for (const ImportedTextGlyph &glyph : text.glyphs) {
    for (const ImportedTextContour &contour : glyph.contours) {
      const size_t start_index = sample_points->size();
      AppendSampledSegmentPointsLocal(contour.segments, sample_points);
      for (size_t index = start_index; index < sample_points->size(); ++index) {
        sample_points->at(index) =
            ImportedArtworkPointToWorld(artwork, sample_points->at(index));
      }
    }
  }

  for (const ImportedTextContour &contour : text.placeholder_contours) {
    const size_t start_index = sample_points->size();
    AppendSampledSegmentPointsLocal(contour.segments, sample_points);
    for (size_t index = start_index; index < sample_points->size(); ++index) {
      sample_points->at(index) =
          ImportedArtworkPointToWorld(artwork, sample_points->at(index));
    }
  }
}

bool PointInsideSelection(const SelectionRect &rect,
                          ImportedArtworkEditMode mode, const ImVec2 &point) {
  if (mode == ImportedArtworkEditMode::SelectRectangle) {
    return point.x >= rect.min.x && point.x <= rect.max.x &&
           point.y >= rect.min.y && point.y <= rect.max.y;
  }

  const ImVec2 center((rect.min.x + rect.max.x) * 0.5f,
                      (rect.min.y + rect.max.y) * 0.5f);
  const float radius_x = std::max((rect.max.x - rect.min.x) * 0.5f, 0.001f);
  const float radius_y = std::max((rect.max.y - rect.min.y) * 0.5f, 0.001f);
  const float dx = (point.x - center.x) / radius_x;
  const float dy = (point.y - center.y) / radius_y;
  return dx * dx + dy * dy <= 1.0f;
}

GuideSideClassification ClassifyGuideSide(const std::vector<ImVec2> &points,
                                          const Guide &guide) {
  if (points.empty()) {
    return GuideSideClassification::Empty;
  }

  bool has_negative = false;
  bool has_positive = false;
  for (const ImVec2 &point : points) {
    const float delta = guide.orientation == GuideOrientation::Vertical
                            ? point.x - guide.position
                            : point.y - guide.position;
    if (delta < -kGuideClassificationEpsilon) {
      has_negative = true;
    } else if (delta > kGuideClassificationEpsilon) {
      has_positive = true;
    } else {
      has_negative = true;
      has_positive = true;
    }

    if (has_negative && has_positive) {
      return GuideSideClassification::Crossing;
    }
  }

  if (has_negative) {
    return GuideSideClassification::Negative;
  }
  if (has_positive) {
    return GuideSideClassification::Positive;
  }
  return GuideSideClassification::Crossing;
}

void SetLastImportedArtworkOperation(CanvasState &state,
                                     ImportedArtworkOperationResult result) {
  state.last_imported_artwork_operation = std::move(result);
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
      if (PointsNear(first_variant.segments.back().end,
                     second_variant.segments.front().start, tolerance)) {
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
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

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

  const bool source_empty = artwork->paths.empty() && artwork->dxf_text.empty();
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
  result.message = action_verb + " " + std::to_string(moved_count) +
                   " imported element" +
                   (moved_count == 1 ? std::string() : std::string("s")) +
                   " into a new artwork.";
  SetLastImportedArtworkOperation(state, result);
  return result;
}

template <typename Function>
void ForEachImportedArtworkPoint(ImportedArtwork &artwork,
                                 Function &&function) {
  for (ImportedPath &path : artwork.paths) {
    for (ImportedPathSegment &segment : path.segments) {
      function(segment.start);
      if (segment.kind == ImportedPathSegmentKind::CubicBezier) {
        function(segment.control1);
        function(segment.control2);
      }
      function(segment.end);
    }
  }

  for (ImportedDxfText &text : artwork.dxf_text) {
    for (ImportedTextGlyph &glyph : text.glyphs) {
      for (ImportedTextContour &contour : glyph.contours) {
        for (ImportedPathSegment &segment : contour.segments) {
          function(segment.start);
          if (segment.kind == ImportedPathSegmentKind::CubicBezier) {
            function(segment.control1);
            function(segment.control2);
          }
          function(segment.end);
        }
      }
    }

    for (ImportedTextContour &contour : text.placeholder_contours) {
      for (ImportedPathSegment &segment : contour.segments) {
        function(segment.start);
        if (segment.kind == ImportedPathSegmentKind::CubicBezier) {
          function(segment.control1);
          function(segment.control2);
        }
        function(segment.end);
      }
    }
  }
}

struct ImportedArtworkBounds {
  ImVec2 min = ImVec2(0.0f, 0.0f);
  ImVec2 max = ImVec2(0.0f, 0.0f);
  bool valid = false;
};

void IncludePoint(ImportedArtworkBounds &bounds, const ImVec2 &point) {
  if (!bounds.valid) {
    bounds.min = point;
    bounds.max = point;
    bounds.valid = true;
    return;
  }

  bounds.min.x = std::min(bounds.min.x, point.x);
  bounds.min.y = std::min(bounds.min.y, point.y);
  bounds.max.x = std::max(bounds.max.x, point.x);
  bounds.max.y = std::max(bounds.max.y, point.y);
}

ImportedArtworkBounds ComputeImportedPathBounds(const ImportedPath &path) {
  ImportedArtworkBounds bounds;
  for (const ImportedPathSegment &segment : path.segments) {
    IncludePoint(bounds, segment.start);
    if (segment.kind == ImportedPathSegmentKind::CubicBezier) {
      IncludePoint(bounds, segment.control1);
      IncludePoint(bounds, segment.control2);
    }
    IncludePoint(bounds, segment.end);
  }
  return bounds;
}

ImportedArtworkBounds
ComputeImportedTextContourBounds(const ImportedTextContour &contour) {
  ImportedArtworkBounds bounds;
  for (const ImportedPathSegment &segment : contour.segments) {
    IncludePoint(bounds, segment.start);
    if (segment.kind == ImportedPathSegmentKind::CubicBezier) {
      IncludePoint(bounds, segment.control1);
      IncludePoint(bounds, segment.control2);
    }
    IncludePoint(bounds, segment.end);
  }
  return bounds;
}

ImportedArtworkBounds
ComputeImportedDxfTextBounds(const ImportedDxfText &text) {
  ImportedArtworkBounds bounds;
  for (const ImportedTextGlyph &glyph : text.glyphs) {
    for (const ImportedTextContour &contour : glyph.contours) {
      const ImportedArtworkBounds contour_bounds =
          ComputeImportedTextContourBounds(contour);
      if (!contour_bounds.valid) {
        continue;
      }
      IncludePoint(bounds, contour_bounds.min);
      IncludePoint(bounds, contour_bounds.max);
    }
  }

  for (const ImportedTextContour &contour : text.placeholder_contours) {
    const ImportedArtworkBounds contour_bounds =
        ComputeImportedTextContourBounds(contour);
    if (!contour_bounds.valid) {
      continue;
    }
    IncludePoint(bounds, contour_bounds.min);
    IncludePoint(bounds, contour_bounds.max);
  }

  return bounds;
}

ImportedArtworkBounds
ComputeImportedArtworkBounds(const ImportedArtwork &artwork) {
  ImportedArtworkBounds bounds;
  for (const ImportedPath &path : artwork.paths) {
    for (const ImportedPathSegment &segment : path.segments) {
      IncludePoint(bounds, segment.start);
      if (segment.kind == ImportedPathSegmentKind::CubicBezier) {
        IncludePoint(bounds, segment.control1);
        IncludePoint(bounds, segment.control2);
      }
      IncludePoint(bounds, segment.end);
    }
  }

  for (const ImportedDxfText &text : artwork.dxf_text) {
    const ImportedArtworkBounds text_bounds =
        ComputeImportedDxfTextBounds(text);
    if (!text_bounds.valid) {
      continue;
    }
    IncludePoint(bounds, text_bounds.min);
    IncludePoint(bounds, text_bounds.max);
  }

  return bounds;
}

ImVec2 ImportedArtworkLocalSize(const ImportedArtwork &artwork) {
  return ImVec2(std::max(artwork.bounds_max.x - artwork.bounds_min.x, 1.0f),
                std::max(artwork.bounds_max.y - artwork.bounds_min.y, 1.0f));
}

ImVec2 ImportedArtworkScaledSize(const ImportedArtwork &artwork) {
  const ImVec2 local_size = ImportedArtworkLocalSize(artwork);
  return ImVec2(std::max(local_size.x * artwork.scale.x, 1.0f),
                std::max(local_size.y * artwork.scale.y, 1.0f));
}

ImportedArtworkBounds ComputeImportedGroupBounds(const ImportedArtwork &artwork,
                                                 const ImportedGroup &group) {
  ImportedArtworkBounds bounds;

  for (const int path_id : group.path_ids) {
    const ImportedPath *path = FindImportedPath(artwork, path_id);
    if (path == nullptr) {
      continue;
    }

    const ImportedArtworkBounds path_bounds = ComputeImportedPathBounds(*path);
    if (!path_bounds.valid) {
      continue;
    }

    IncludePoint(bounds, path_bounds.min);
    IncludePoint(bounds, path_bounds.max);
  }

  for (const int text_id : group.dxf_text_ids) {
    const ImportedDxfText *text = FindImportedDxfText(artwork, text_id);
    if (text == nullptr) {
      continue;
    }

    const ImportedArtworkBounds text_bounds =
        ComputeImportedDxfTextBounds(*text);
    if (!text_bounds.valid) {
      continue;
    }

    IncludePoint(bounds, text_bounds.min);
    IncludePoint(bounds, text_bounds.max);
  }

  for (const int child_group_id : group.child_group_ids) {
    const ImportedGroup *child_group =
        FindImportedGroup(artwork, child_group_id);
    if (child_group == nullptr) {
      continue;
    }

    const ImportedArtworkBounds child_bounds =
        ComputeImportedGroupBounds(artwork, *child_group);
    if (!child_bounds.valid) {
      continue;
    }

    IncludePoint(bounds, child_bounds.min);
    IncludePoint(bounds, child_bounds.max);
  }

  return bounds;
}

template <typename Function>
bool TransformImportedArtwork(CanvasState &state, int imported_artwork_id,
                              Function &&transform, const char *action_name) {
  ImportedArtwork *artwork = FindImportedArtwork(state, imported_artwork_id);
  if (artwork == nullptr) {
    return false;
  }

  const ImVec2 size = ImportedArtworkLocalSize(*artwork);
  const ImVec2 original_scaled_size = ImportedArtworkScaledSize(*artwork);
  const ImVec2 world_center(artwork->origin.x + original_scaled_size.x * 0.5f,
                            artwork->origin.y + original_scaled_size.y * 0.5f);

  ForEachImportedArtworkPoint(*artwork, [&](ImVec2 &point) {
    const ImVec2 local(point.x - artwork->bounds_min.x,
                       point.y - artwork->bounds_min.y);
    point = transform(local, size);
  });

  RecomputeImportedArtworkBounds(*artwork);
  RecomputeImportedHierarchyBounds(*artwork);
  const ImVec2 new_scaled_size = ImportedArtworkScaledSize(*artwork);
  artwork->origin = ImVec2(world_center.x - new_scaled_size.x * 0.5f,
                           world_center.y - new_scaled_size.y * 0.5f);

  log::GetLogger()->info("{} imported artwork id={} name='{}'", action_name,
                         artwork->id, artwork->name);
  return true;
}

ExportArea *FindExportAreaBySourceWorkingAreaId(CanvasState &state,
                                                int working_area_id) {
  auto it =
      std::find_if(state.export_areas.begin(), state.export_areas.end(),
                   [working_area_id](const ExportArea &area) {
                     return area.source_working_area_id == working_area_id;
                   });
  return it == state.export_areas.end() ? nullptr : &(*it);
}

} // namespace

ImVec2 ImportedArtworkPointToWorld(const ImportedArtwork &artwork,
                                   const ImVec2 &point) {
  return ImVec2(
      artwork.origin.x + (point.x - artwork.bounds_min.x) * artwork.scale.x,
      artwork.origin.y + (point.y - artwork.bounds_min.y) * artwork.scale.y);
}

void ImportedLocalBoundsToWorldBounds(const ImportedArtwork &artwork,
                                      const ImVec2 &local_min,
                                      const ImVec2 &local_max,
                                      ImVec2 *world_min, ImVec2 *world_max) {
  const ImVec2 world_min_value =
      ImportedArtworkPointToWorld(artwork, local_min);
  const ImVec2 world_max_value =
      ImportedArtworkPointToWorld(artwork, local_max);
  if (world_min != nullptr) {
    *world_min = ImVec2(std::min(world_min_value.x, world_max_value.x),
                        std::min(world_min_value.y, world_max_value.y));
  }
  if (world_max != nullptr) {
    *world_max = ImVec2(std::max(world_min_value.x, world_max_value.x),
                        std::max(world_min_value.y, world_max_value.y));
  }
}

void ClearImportedDebugSelection(CanvasState &state) {
  state.selected_imported_debug = {};
}

void ClearSelectedImportedElements(CanvasState &state) {
  state.selected_imported_elements.clear();
}

bool IsImportedElementSelected(const CanvasState &state, int artwork_id,
                               ImportedElementKind kind, int item_id) {
  if (state.selected_imported_artwork_id != artwork_id) {
    return false;
  }

  return std::any_of(
      state.selected_imported_elements.begin(),
      state.selected_imported_elements.end(),
      [kind, item_id](const ImportedElementSelection &selection) {
        return selection.kind == kind && selection.item_id == item_id;
      });
}

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
  const bool lock_ratio = IsImportedArtworkScaleRatioLocked(artwork);

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
  const ImVec2 clamped_scale(
      std::max(target_scale.x, kMinimumImportedArtworkScale),
      std::max(target_scale.y, kMinimumImportedArtworkScale));
  if (!IsImportedArtworkScaleRatioLocked(artwork)) {
    artwork.scale = clamped_scale;
    return;
  }

  const float old_x = std::max(artwork.scale.x, kMinimumImportedArtworkScale);
  const float old_y = std::max(artwork.scale.y, kMinimumImportedArtworkScale);
  const float factor_x = clamped_scale.x / old_x;
  const float factor_y = clamped_scale.y / old_y;
  const float chosen_factor =
      std::abs(factor_x - 1.0f) >= std::abs(factor_y - 1.0f) ? factor_x
                                                             : factor_y;
  artwork.scale.x =
      std::max(old_x * chosen_factor, kMinimumImportedArtworkScale);
  artwork.scale.y =
      std::max(old_y * chosen_factor, kMinimumImportedArtworkScale);
}

Guide *FindGuide(CanvasState &state, int guide_id) {
  auto it = std::find_if(
      state.guides.begin(), state.guides.end(),
      [guide_id](const Guide &guide) { return guide.id == guide_id; });
  return it == state.guides.end() ? nullptr : &(*it);
}

const Guide *FindGuide(const CanvasState &state, int guide_id) {
  auto it = std::find_if(
      state.guides.begin(), state.guides.end(),
      [guide_id](const Guide &guide) { return guide.id == guide_id; });
  return it == state.guides.end() ? nullptr : &(*it);
}

ImportedArtwork *FindImportedArtwork(CanvasState &state,
                                     int imported_artwork_id) {
  auto it =
      std::find_if(state.imported_artwork.begin(), state.imported_artwork.end(),
                   [imported_artwork_id](const ImportedArtwork &artwork) {
                     return artwork.id == imported_artwork_id;
                   });
  return it == state.imported_artwork.end() ? nullptr : &(*it);
}

const ImportedArtwork *FindImportedArtwork(const CanvasState &state,
                                           int imported_artwork_id) {
  auto it =
      std::find_if(state.imported_artwork.begin(), state.imported_artwork.end(),
                   [imported_artwork_id](const ImportedArtwork &artwork) {
                     return artwork.id == imported_artwork_id;
                   });
  return it == state.imported_artwork.end() ? nullptr : &(*it);
}

ImportedGroup *FindImportedGroup(ImportedArtwork &artwork, int group_id) {
  auto it = std::find_if(
      artwork.groups.begin(), artwork.groups.end(),
      [group_id](const ImportedGroup &group) { return group.id == group_id; });
  return it == artwork.groups.end() ? nullptr : &(*it);
}

const ImportedGroup *FindImportedGroup(const ImportedArtwork &artwork,
                                       int group_id) {
  auto it = std::find_if(
      artwork.groups.begin(), artwork.groups.end(),
      [group_id](const ImportedGroup &group) { return group.id == group_id; });
  return it == artwork.groups.end() ? nullptr : &(*it);
}

ImportedPath *FindImportedPath(ImportedArtwork &artwork, int path_id) {
  auto it = std::find_if(
      artwork.paths.begin(), artwork.paths.end(),
      [path_id](const ImportedPath &path) { return path.id == path_id; });
  return it == artwork.paths.end() ? nullptr : &(*it);
}

const ImportedPath *FindImportedPath(const ImportedArtwork &artwork,
                                     int path_id) {
  auto it = std::find_if(
      artwork.paths.begin(), artwork.paths.end(),
      [path_id](const ImportedPath &path) { return path.id == path_id; });
  return it == artwork.paths.end() ? nullptr : &(*it);
}

ImportedDxfText *FindImportedDxfText(ImportedArtwork &artwork, int text_id) {
  auto it = std::find_if(
      artwork.dxf_text.begin(), artwork.dxf_text.end(),
      [text_id](const ImportedDxfText &text) { return text.id == text_id; });
  return it == artwork.dxf_text.end() ? nullptr : &(*it);
}

const ImportedDxfText *FindImportedDxfText(const ImportedArtwork &artwork,
                                           int text_id) {
  auto it = std::find_if(
      artwork.dxf_text.begin(), artwork.dxf_text.end(),
      [text_id](const ImportedDxfText &text) { return text.id == text_id; });
  return it == artwork.dxf_text.end() ? nullptr : &(*it);
}

WorkingArea *FindWorkingArea(CanvasState &state, int working_area_id) {
  auto it = std::find_if(state.working_areas.begin(), state.working_areas.end(),
                         [working_area_id](const WorkingArea &area) {
                           return area.id == working_area_id;
                         });
  return it == state.working_areas.end() ? nullptr : &(*it);
}

const WorkingArea *FindWorkingArea(const CanvasState &state,
                                   int working_area_id) {
  auto it = std::find_if(state.working_areas.begin(), state.working_areas.end(),
                         [working_area_id](const WorkingArea &area) {
                           return area.id == working_area_id;
                         });
  return it == state.working_areas.end() ? nullptr : &(*it);
}

void SyncExportAreaFromWorkingArea(CanvasState &state, int working_area_id) {
  const WorkingArea *working_area = FindWorkingArea(state, working_area_id);
  ExportArea *export_area =
      FindExportAreaBySourceWorkingAreaId(state, working_area_id);
  if (working_area == nullptr || export_area == nullptr) {
    return;
  }

  export_area->origin = working_area->origin;
  export_area->size = working_area->size;
  export_area->visible = working_area->visible;
}

int AddWorkingArea(CanvasState &state,
                   const WorkingAreaCreateInfo &create_info) {
  WorkingArea area;
  area.id = state.next_working_area_id++;
  area.name = create_info.name.empty()
                  ? "Working Area " + std::to_string(area.id)
                  : create_info.name;
  area.size = ImVec2(std::max(create_info.size_pixels.x, 1.0f),
                     std::max(create_info.size_pixels.y, 1.0f));
  area.flags = create_info.flags;
  const float stagger = 32.0f * static_cast<float>(state.working_areas.size());
  area.origin = ImVec2(stagger, stagger);
  state.working_areas.push_back(area);

  ExportArea export_area;
  export_area.id = state.next_export_area_id++;
  export_area.source_working_area_id = area.id;
  export_area.origin = area.origin;
  export_area.size = area.size;
  state.export_areas.push_back(export_area);
  return area.id;
}

int AppendImportedArtwork(CanvasState &state, ImportedArtwork artwork) {
  artwork.id = state.next_imported_artwork_id++;
  if (artwork.name.empty()) {
    artwork.name = "Artwork " + std::to_string(artwork.id);
  }

  if (!state.working_areas.empty()) {
    const float stagger =
        24.0f * static_cast<float>(state.imported_artwork.size());
    artwork.origin.x += state.working_areas.front().origin.x + stagger;
    artwork.origin.y += state.working_areas.front().origin.y + stagger;
  }

  RecomputeImportedHierarchyBounds(artwork);

  state.imported_artwork.push_back(std::move(artwork));
  return state.imported_artwork.back().id;
}

void ClearImportedArtwork(CanvasState &state) {
  state.imported_artwork.clear();
  state.selected_imported_artwork_id = 0;
  ClearImportedDebugSelection(state);
  ClearSelectedImportedElements(state);
}

void RecomputeImportedArtworkBounds(ImportedArtwork &artwork) {
  const ImportedArtworkBounds bounds = ComputeImportedArtworkBounds(artwork);
  if (!bounds.valid) {
    artwork.bounds_min = ImVec2(0.0f, 0.0f);
    artwork.bounds_max = ImVec2(1.0f, 1.0f);
    return;
  }

  const ImVec2 offset = bounds.min;
  ForEachImportedArtworkPoint(artwork, [&offset](ImVec2 &point) {
    point.x -= offset.x;
    point.y -= offset.y;
  });

  artwork.bounds_min = ImVec2(0.0f, 0.0f);
  artwork.bounds_max = ImVec2(std::max(bounds.max.x - bounds.min.x, 1.0f),
                              std::max(bounds.max.y - bounds.min.y, 1.0f));
}

void RecomputeImportedHierarchyBounds(ImportedArtwork &artwork) {
  for (ImportedPath &path : artwork.paths) {
    const ImportedArtworkBounds path_bounds = ComputeImportedPathBounds(path);
    if (path_bounds.valid) {
      path.bounds_min = path_bounds.min;
      path.bounds_max = path_bounds.max;
    } else {
      path.bounds_min = ImVec2(0.0f, 0.0f);
      path.bounds_max = ImVec2(0.0f, 0.0f);
    }
  }

  for (ImportedDxfText &text : artwork.dxf_text) {
    for (ImportedTextGlyph &glyph : text.glyphs) {
      ImportedArtworkBounds glyph_bounds;
      for (ImportedTextContour &contour : glyph.contours) {
        const ImportedArtworkBounds contour_bounds =
            ComputeImportedTextContourBounds(contour);
        if (contour_bounds.valid) {
          contour.bounds_min = contour_bounds.min;
          contour.bounds_max = contour_bounds.max;
          IncludePoint(glyph_bounds, contour_bounds.min);
          IncludePoint(glyph_bounds, contour_bounds.max);
        } else {
          contour.bounds_min = ImVec2(0.0f, 0.0f);
          contour.bounds_max = ImVec2(0.0f, 0.0f);
        }
      }

      if (glyph_bounds.valid) {
        glyph.bounds_min = glyph_bounds.min;
        glyph.bounds_max = glyph_bounds.max;
      } else {
        glyph.bounds_min = ImVec2(0.0f, 0.0f);
        glyph.bounds_max = ImVec2(0.0f, 0.0f);
      }
    }

    for (ImportedTextContour &contour : text.placeholder_contours) {
      const ImportedArtworkBounds contour_bounds =
          ComputeImportedTextContourBounds(contour);
      if (contour_bounds.valid) {
        contour.bounds_min = contour_bounds.min;
        contour.bounds_max = contour_bounds.max;
      } else {
        contour.bounds_min = ImVec2(0.0f, 0.0f);
        contour.bounds_max = ImVec2(0.0f, 0.0f);
      }
    }

    const ImportedArtworkBounds text_bounds =
        ComputeImportedDxfTextBounds(text);
    if (text_bounds.valid) {
      text.bounds_min = text_bounds.min;
      text.bounds_max = text_bounds.max;
    } else {
      text.bounds_min = ImVec2(0.0f, 0.0f);
      text.bounds_max = ImVec2(0.0f, 0.0f);
    }
  }

  for (ImportedGroup &group : artwork.groups) {
    const ImportedArtworkBounds group_bounds =
        ComputeImportedGroupBounds(artwork, group);
    if (group_bounds.valid) {
      group.bounds_min = group_bounds.min;
      group.bounds_max = group_bounds.max;
    } else {
      group.bounds_min = ImVec2(0.0f, 0.0f);
      group.bounds_max = ImVec2(0.0f, 0.0f);
    }
  }
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
UpdateImportedArtworkOutlineColor(CanvasState &state, int imported_artwork_id,
                                  const ImVec4 &stroke_color) {
  ImportedArtworkOperationResult result;
  result.artwork_id = imported_artwork_id;

  ImportedArtwork *artwork = FindImportedArtwork(state, imported_artwork_id);
  if (artwork == nullptr) {
    result.message = "Imported artwork was not found.";
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  for (ImportedPath &path : artwork->paths) {
    path.stroke_color = stroke_color;
  }
  for (ImportedDxfText &text : artwork->dxf_text) {
    text.stroke_color = stroke_color;
  }

  result.success = true;
  result.message = "Updated outline color for the selected imported artwork.";
  SetLastImportedArtworkOperation(state, result);
  return result;
}

ImportedArtworkOperationResult
PrepareImportedArtworkForCutting(CanvasState &state, int imported_artwork_id,
                                 float weld_tolerance) {
  ImportedArtworkOperationResult result;
  result.artwork_id = imported_artwork_id;

  ImportedArtwork *artwork = FindImportedArtwork(state, imported_artwork_id);
  if (artwork == nullptr) {
    result.message = "Imported artwork was not found.";
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  for (ImportedPath &path : artwork->paths) {
    if (path.segments.empty() || path.closed) {
      continue;
    }
    if (PointsNear(path.segments.front().start, path.segments.back().end,
                   weld_tolerance)) {
      path.segments.back().end = path.segments.front().start;
      path.closed = true;
    }
  }

  bool merged_any = true;
  while (merged_any) {
    merged_any = false;
    for (size_t first_index = 0; first_index < artwork->paths.size();
         ++first_index) {
      ImportedPath &first_path = artwork->paths[first_index];
      if (first_path.closed || first_path.segments.empty()) {
        continue;
      }

      for (size_t second_index = first_index + 1;
           second_index < artwork->paths.size(); ++second_index) {
        ImportedPath &second_path = artwork->paths[second_index];
        if (second_path.closed || second_path.segments.empty() ||
            second_path.parent_group_id != first_path.parent_group_id) {
          continue;
        }

        ImportedPath oriented_first;
        ImportedPath oriented_second;
        if (!TryOrientPathsForMerge(first_path, second_path, weld_tolerance,
                                    &oriented_first, &oriented_second)) {
          continue;
        }

        oriented_second.segments.front().start =
            oriented_first.segments.back().end;
        oriented_first.segments.insert(oriented_first.segments.end(),
                                       oriented_second.segments.begin(),
                                       oriented_second.segments.end());
        if (PointsNear(oriented_first.segments.front().start,
                       oriented_first.segments.back().end, weld_tolerance)) {
          oriented_first.segments.back().end =
              oriented_first.segments.front().start;
          oriented_first.closed = true;
        }

        const int removed_path_id = second_path.id;
        artwork->paths[first_index] = std::move(oriented_first);
        artwork->paths.erase(artwork->paths.begin() +
                             static_cast<std::ptrdiff_t>(second_index));
        for (ImportedGroup &group : artwork->groups) {
          std::erase(group.path_ids, removed_path_id);
        }
        result.stitched_count += 1;
        merged_any = true;
        break;
      }

      if (merged_any) {
        break;
      }
    }
  }

  for (const ImportedPath &path : artwork->paths) {
    if (path.closed) {
      result.closed_count += 1;
    } else {
      result.open_count += 1;
    }
  }

  RecomputeImportedArtworkBounds(*artwork);
  RecomputeImportedHierarchyBounds(*artwork);

  result.success = true;
  result.message = "Prepared imported artwork: stitched " +
                   std::to_string(result.stitched_count) + ", closed " +
                   std::to_string(result.closed_count) + ", open " +
                   std::to_string(result.open_count) + ".";
  SetLastImportedArtworkOperation(state, result);
  return result;
}

ImportedArtworkOperationResult SelectImportedElementsInWorldRect(
    CanvasState &state, int imported_artwork_id, const ImVec2 &world_start,
    const ImVec2 &world_end, ImportedArtworkEditMode mode) {
  ImportedArtworkOperationResult result;
  result.artwork_id = imported_artwork_id;

  ImportedArtwork *artwork = FindImportedArtwork(state, imported_artwork_id);
  if (artwork == nullptr) {
    result.message = "Imported artwork was not found.";
    SetLastImportedArtworkOperation(state, result);
    return result;
  }
  if (mode == ImportedArtworkEditMode::None) {
    result.message = "Imported artwork selection mode is not active.";
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  const SelectionRect selection_rect = NormalizeRect(world_start, world_end);
  ClearSelectedImportedElements(state);

  std::vector<ImVec2> sample_points;
  for (const ImportedPath &path : artwork->paths) {
    sample_points.clear();
    AppendPathSamplePointsWorld(*artwork, path, &sample_points);
    if (sample_points.empty()) {
      continue;
    }

    int inside_count = 0;
    for (const ImVec2 &point : sample_points) {
      inside_count += PointInsideSelection(selection_rect, mode, point) ? 1 : 0;
    }
    if (inside_count == static_cast<int>(sample_points.size())) {
      state.selected_imported_elements.push_back(
          {ImportedElementKind::Path, path.id});
    } else if (inside_count != 0) {
      result.skipped_count += 1;
    }
  }

  for (const ImportedDxfText &text : artwork->dxf_text) {
    sample_points.clear();
    AppendTextSamplePointsWorld(*artwork, text, &sample_points);
    if (sample_points.empty()) {
      continue;
    }

    int inside_count = 0;
    for (const ImVec2 &point : sample_points) {
      inside_count += PointInsideSelection(selection_rect, mode, point) ? 1 : 0;
    }
    if (inside_count == static_cast<int>(sample_points.size())) {
      state.selected_imported_elements.push_back(
          {ImportedElementKind::DxfText, text.id});
    } else if (inside_count != 0) {
      result.skipped_count += 1;
    }
  }

  result.selected_count =
      static_cast<int>(state.selected_imported_elements.size());
  result.success = result.selected_count > 0;
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
  SetLastImportedArtworkOperation(state, result);
  return result;
}

ImportedArtworkOperationResult
ExtractSelectedImportedElements(CanvasState &state, int imported_artwork_id) {
  std::unordered_set<int> path_ids;
  std::unordered_set<int> text_ids;
  for (const ImportedElementSelection &selection :
       state.selected_imported_elements) {
    if (selection.kind == ImportedElementKind::Path) {
      path_ids.insert(selection.item_id);
    } else {
      text_ids.insert(selection.item_id);
    }
  }

  ImportedArtworkOperationResult result =
      MoveImportedElementsToNewArtwork(state, imported_artwork_id, path_ids,
                                       text_ids, " Extracted", "Extracted");
  result.selected_count = static_cast<int>(path_ids.size() + text_ids.size());
  SetLastImportedArtworkOperation(state, result);
  return result;
}

ImportedArtworkOperationResult
SeparateImportedArtworkByGuide(CanvasState &state, int imported_artwork_id,
                               int guide_id) {
  ImportedArtworkOperationResult result;
  result.artwork_id = imported_artwork_id;

  ImportedArtwork *artwork = FindImportedArtwork(state, imported_artwork_id);
  const Guide *guide = FindGuide(state, guide_id);
  if (artwork == nullptr || guide == nullptr) {
    result.message = "Guide split requires a valid guide and imported artwork.";
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  std::unordered_set<int> positive_path_ids;
  std::unordered_set<int> positive_text_ids;
  int negative_count = 0;
  std::vector<ImVec2> sample_points;

  for (const ImportedPath &path : artwork->paths) {
    sample_points.clear();
    AppendPathSamplePointsWorld(*artwork, path, &sample_points);
    switch (ClassifyGuideSide(sample_points, *guide)) {
    case GuideSideClassification::Positive:
      positive_path_ids.insert(path.id);
      break;
    case GuideSideClassification::Negative:
      negative_count += 1;
      break;
    case GuideSideClassification::Crossing:
      result.skipped_count += 1;
      break;
    case GuideSideClassification::Empty:
      break;
    }
  }

  for (const ImportedDxfText &text : artwork->dxf_text) {
    sample_points.clear();
    AppendTextSamplePointsWorld(*artwork, text, &sample_points);
    switch (ClassifyGuideSide(sample_points, *guide)) {
    case GuideSideClassification::Positive:
      positive_text_ids.insert(text.id);
      break;
    case GuideSideClassification::Negative:
      negative_count += 1;
      break;
    case GuideSideClassification::Crossing:
      result.skipped_count += 1;
      break;
    case GuideSideClassification::Empty:
      break;
    }
  }

  const int positive_count =
      static_cast<int>(positive_path_ids.size() + positive_text_ids.size());
  if (positive_count == 0 || negative_count == 0) {
    result.message =
        "Guide split needs movable content on both sides of the guide.";
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  const int skipped_count = result.skipped_count;
  result = MoveImportedElementsToNewArtwork(
      state, imported_artwork_id, positive_path_ids, positive_text_ids,
      " Guide Split", "Split");
  result.skipped_count += skipped_count;
  result.message +=
      " Skipped " + std::to_string(result.skipped_count) + " crossing element" +
      (result.skipped_count == 1 ? std::string() : std::string("s")) + ".";
  SetLastImportedArtworkOperation(state, result);
  return result;
}

bool DeleteImportedArtwork(CanvasState &state, int imported_artwork_id) {
  auto it =
      std::find_if(state.imported_artwork.begin(), state.imported_artwork.end(),
                   [imported_artwork_id](const ImportedArtwork &artwork) {
                     return artwork.id == imported_artwork_id;
                   });
  if (it == state.imported_artwork.end()) {
    return false;
  }

  log::GetLogger()->info("Deleted imported artwork id={} name='{}'", it->id,
                         it->name);
  state.imported_artwork.erase(it);
  if (state.selected_imported_artwork_id == imported_artwork_id) {
    state.selected_imported_artwork_id = 0;
    ClearSelectedImportedElements(state);
  }
  if (state.selected_imported_debug.artwork_id == imported_artwork_id) {
    ClearImportedDebugSelection(state);
  }
  return true;
}

void InitializeDefaultDocument(CanvasState &state) {
  if (state.layers.empty()) {
    state.layers.push_back(Layer{state.next_layer_id++, "Root", true, false});
  }

  if (state.working_areas.empty()) {
    WorkingAreaCreateInfo create_info;
    create_info.name = "Working Area 1";
    create_info.size_pixels = ImVec2(
        UnitsToPixels(210.0f, MeasurementUnit::Millimeters, state.calibration),
        UnitsToPixels(297.0f, MeasurementUnit::Millimeters, state.calibration));
    create_info.flags = kDefaultWorkingAreaFlags;
    AddWorkingArea(state, create_info);
  }
}

} // namespace im2d