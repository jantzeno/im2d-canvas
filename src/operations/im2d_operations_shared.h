#pragma once

#include "../canvas/im2d_canvas_document.h"
#include "../common/im2d_log.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <vector>

namespace im2d::operations::detail {

constexpr float kSharedMinimumImportedArtworkScale = 0.01f;
constexpr float kSharedCurveFlatnessTolerance = 0.05f;
constexpr int kSharedCurveMaxSubdivisionDepth = 10;
constexpr float kSharedGuideClassificationEpsilon = 0.25f;

enum class SharedGuideSideClassification {
  Empty,
  Negative,
  Positive,
  Crossing,
};

inline float DistanceSquaredShared(const ImVec2 &a, const ImVec2 &b) {
  const float dx = a.x - b.x;
  const float dy = a.y - b.y;
  return dx * dx + dy * dy;
}

inline bool PointsNearShared(const ImVec2 &a, const ImVec2 &b,
                             float tolerance) {
  return DistanceSquaredShared(a, b) <= tolerance * tolerance;
}

inline ImVec2 MidpointShared(const ImVec2 &a, const ImVec2 &b) {
  return ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
}

inline float DistancePointToLineSquaredShared(const ImVec2 &point,
                                              const ImVec2 &line_start,
                                              const ImVec2 &line_end) {
  const float dx = line_end.x - line_start.x;
  const float dy = line_end.y - line_start.y;
  const float line_length_squared = dx * dx + dy * dy;
  if (line_length_squared <= 0.0000001f) {
    return DistanceSquaredShared(point, line_start);
  }

  const float numerator =
      std::fabs(dy * point.x - dx * point.y + line_end.x * line_start.y -
                line_end.y * line_start.x);
  return (numerator * numerator) / line_length_squared;
}

inline bool CubicBezierFlatEnoughShared(const ImVec2 &start,
                                        const ImVec2 &control1,
                                        const ImVec2 &control2,
                                        const ImVec2 &end,
                                        float tolerance_squared) {
  return DistancePointToLineSquaredShared(control1, start, end) <=
             tolerance_squared &&
         DistancePointToLineSquaredShared(control2, start, end) <=
             tolerance_squared;
}

inline void
AppendAdaptiveCubicPointsShared(const ImVec2 &start, const ImVec2 &control1,
                                const ImVec2 &control2, const ImVec2 &end,
                                int depth, std::vector<ImVec2> *sample_points) {
  const float tolerance_squared =
      kSharedCurveFlatnessTolerance * kSharedCurveFlatnessTolerance;
  if (depth >= kSharedCurveMaxSubdivisionDepth ||
      CubicBezierFlatEnoughShared(start, control1, control2, end,
                                  tolerance_squared)) {
    sample_points->push_back(end);
    return;
  }

  const ImVec2 start_control1_mid = MidpointShared(start, control1);
  const ImVec2 control1_control2_mid = MidpointShared(control1, control2);
  const ImVec2 control2_end_mid = MidpointShared(control2, end);
  const ImVec2 left_control2 =
      MidpointShared(start_control1_mid, control1_control2_mid);
  const ImVec2 right_control1 =
      MidpointShared(control1_control2_mid, control2_end_mid);
  const ImVec2 split_point = MidpointShared(left_control2, right_control1);

  AppendAdaptiveCubicPointsShared(start, start_control1_mid, left_control2,
                                  split_point, depth + 1, sample_points);
  AppendAdaptiveCubicPointsShared(split_point, right_control1, control2_end_mid,
                                  end, depth + 1, sample_points);
}

inline void AppendSampledSegmentPointsLocalShared(
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

    AppendAdaptiveCubicPointsShared(segment.start, segment.control1,
                                    segment.control2, segment.end, 0,
                                    sample_points);
  }
}

inline void
AppendPathSamplePointsWorldShared(const ImportedArtwork &artwork,
                                  const ImportedPath &path,
                                  std::vector<ImVec2> *sample_points) {
  const size_t start_index = sample_points->size();
  AppendSampledSegmentPointsLocalShared(path.segments, sample_points);
  for (size_t index = start_index; index < sample_points->size(); ++index) {
    sample_points->at(index) =
        ImportedArtworkPointToWorld(artwork, sample_points->at(index));
  }
}

inline void
AppendTextSamplePointsWorldShared(const ImportedArtwork &artwork,
                                  const ImportedDxfText &text,
                                  std::vector<ImVec2> *sample_points) {
  for (const ImportedTextGlyph &glyph : text.glyphs) {
    for (const ImportedTextContour &contour : glyph.contours) {
      const size_t start_index = sample_points->size();
      AppendSampledSegmentPointsLocalShared(contour.segments, sample_points);
      for (size_t index = start_index; index < sample_points->size(); ++index) {
        sample_points->at(index) =
            ImportedArtworkPointToWorld(artwork, sample_points->at(index));
      }
    }
  }

  for (const ImportedTextContour &contour : text.placeholder_contours) {
    const size_t start_index = sample_points->size();
    AppendSampledSegmentPointsLocalShared(contour.segments, sample_points);
    for (size_t index = start_index; index < sample_points->size(); ++index) {
      sample_points->at(index) =
          ImportedArtworkPointToWorld(artwork, sample_points->at(index));
    }
  }
}

inline SharedGuideSideClassification
ClassifyGuideSideShared(const std::vector<ImVec2> &points, const Guide &guide) {
  if (points.empty()) {
    return SharedGuideSideClassification::Empty;
  }

  bool has_negative = false;
  bool has_positive = false;
  for (const ImVec2 &point : points) {
    const float delta = guide.orientation == GuideOrientation::Vertical
                            ? point.x - guide.position
                            : point.y - guide.position;
    if (delta < -kSharedGuideClassificationEpsilon) {
      has_negative = true;
    } else if (delta > kSharedGuideClassificationEpsilon) {
      has_positive = true;
    } else {
      has_negative = true;
      has_positive = true;
    }

    if (has_negative && has_positive) {
      return SharedGuideSideClassification::Crossing;
    }
  }

  if (has_negative) {
    return SharedGuideSideClassification::Negative;
  }
  if (has_positive) {
    return SharedGuideSideClassification::Positive;
  }
  return SharedGuideSideClassification::Crossing;
}

inline void
UpdateImportedArtworkScaleFromTargetShared(ImportedArtwork &artwork,
                                           const ImVec2 &target_scale) {
  const ImVec2 clamped_scale(
      std::max(target_scale.x, kSharedMinimumImportedArtworkScale),
      std::max(target_scale.y, kSharedMinimumImportedArtworkScale));
  if (!HasImportedArtworkFlag(artwork.flags,
                              ImportedArtworkFlagLockScaleRatio)) {
    artwork.scale = clamped_scale;
    return;
  }

  const float old_x =
      std::max(artwork.scale.x, kSharedMinimumImportedArtworkScale);
  const float old_y =
      std::max(artwork.scale.y, kSharedMinimumImportedArtworkScale);
  const float factor_x = clamped_scale.x / old_x;
  const float factor_y = clamped_scale.y / old_y;
  const float chosen_factor =
      std::abs(factor_x - 1.0f) >= std::abs(factor_y - 1.0f) ? factor_x
                                                             : factor_y;
  artwork.scale.x =
      std::max(old_x * chosen_factor, kSharedMinimumImportedArtworkScale);
  artwork.scale.y =
      std::max(old_y * chosen_factor, kSharedMinimumImportedArtworkScale);
}

inline bool DeleteImportedArtworkShared(CanvasState &state,
                                        int imported_artwork_id) {
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
  if (state.imported_artwork_separation_preview.active &&
      state.imported_artwork_separation_preview.artwork_id ==
          imported_artwork_id) {
    state.imported_artwork_separation_preview = {};
  }
  if (state.imported_artwork_auto_cut_preview.active &&
      state.imported_artwork_auto_cut_preview.artwork_id ==
          imported_artwork_id) {
    state.imported_artwork_auto_cut_preview = {};
  }
  if (state.selected_imported_artwork_id == imported_artwork_id) {
    im2d::RemoveSelectedImportedArtworkObject(state, imported_artwork_id);
    im2d::ClearSelectedImportedElements(state);
  } else {
    im2d::RemoveSelectedImportedArtworkObject(state, imported_artwork_id);
  }
  if (state.selected_imported_debug.artwork_id == imported_artwork_id) {
    im2d::ClearImportedDebugSelection(state);
  }
  return true;
}

template <typename RunStagesFn, typename CountStatesFn, typename PopulateFn,
          typename SetIssuesFn, typename SetLastFn>
inline ImportedArtworkOperationResult PrepareImportedArtworkForCuttingShared(
    CanvasState &state, int imported_artwork_id, float weld_tolerance,
    RunStagesFn run_stages_fn, CountStatesFn count_states_fn,
    PopulateFn populate_fn, SetIssuesFn set_issues_fn, SetLastFn set_last_fn) {
  ImportedArtworkOperationResult result;
  result.artwork_id = imported_artwork_id;

  ImportedArtwork *artwork = FindImportedArtwork(state, imported_artwork_id);
  if (artwork == nullptr) {
    result.message = "Imported artwork was not found.";
    set_issues_fn(state, 0, {});
    set_last_fn(state, result);
    return result;
  }

  set_issues_fn(state, 0, {});
  run_stages_fn(*artwork, weld_tolerance, &result);
  count_states_fn(*artwork, &result);

  RecomputeImportedArtworkBounds(*artwork);
  RecomputeImportedHierarchyBounds(*artwork);
  RefreshImportedArtworkPartMetadata(*artwork);

  result.success = true;
  populate_fn(&result, *artwork);
  const char *mode_label =
      result.prepare_mode == ImportedArtworkPrepareMode::AggressiveCleanup
          ? "Prepare + Weld Cleanup"
          : "Prepare For Cutting";
  result.message = std::string(mode_label) + ": preserved " +
                   std::to_string(result.preserved_count) + ", stitched " +
                   std::to_string(result.stitched_count) + ", cleaned " +
                   std::to_string(result.cleaned_count) + ", closed " +
                   std::to_string(result.closed_count) + ", open " +
                   std::to_string(result.open_count) + ", placeholder " +
                   std::to_string(result.placeholder_count) + ", ambiguous " +
                   std::to_string(result.ambiguous_count) + ".";
  set_last_fn(state, result);
  return result;
}

inline void CollectImportedGroupElementIdsShared(
    const ImportedArtwork &artwork, int group_id,
    std::unordered_set<int> *path_ids, std::unordered_set<int> *text_ids,
    std::unordered_set<int> *visited_group_ids) {
  if (path_ids == nullptr || text_ids == nullptr ||
      visited_group_ids == nullptr ||
      !visited_group_ids->insert(group_id).second) {
    return;
  }

  const ImportedGroup *group = FindImportedGroup(artwork, group_id);
  if (group == nullptr) {
    return;
  }

  path_ids->insert(group->path_ids.begin(), group->path_ids.end());
  text_ids->insert(group->dxf_text_ids.begin(), group->dxf_text_ids.end());
  for (const int child_group_id : group->child_group_ids) {
    CollectImportedGroupElementIdsShared(artwork, child_group_id, path_ids,
                                         text_ids, visited_group_ids);
  }
}

inline void ResolveExtractedElementIdsShared(
    const CanvasState &state, int imported_artwork_id,
    std::unordered_set<int> *path_ids, std::unordered_set<int> *text_ids) {
  if (path_ids == nullptr || text_ids == nullptr) {
    return;
  }

  if (state.selected_imported_artwork_id == imported_artwork_id &&
      !state.selected_imported_elements.empty()) {
    for (const ImportedElementSelection &selection :
         state.selected_imported_elements) {
      if (selection.kind == ImportedElementKind::Path) {
        path_ids->insert(selection.item_id);
      } else {
        text_ids->insert(selection.item_id);
      }
    }
  }
  if (!path_ids->empty() || !text_ids->empty()) {
    return;
  }

  if (state.selected_imported_debug.artwork_id != imported_artwork_id) {
    return;
  }

  const ImportedArtwork *artwork =
      FindImportedArtwork(state, imported_artwork_id);
  if (artwork == nullptr) {
    return;
  }

  switch (state.selected_imported_debug.kind) {
  case ImportedDebugSelectionKind::Group: {
    std::unordered_set<int> visited_group_ids;
    CollectImportedGroupElementIdsShared(
        *artwork, state.selected_imported_debug.item_id, path_ids, text_ids,
        &visited_group_ids);
    return;
  }
  case ImportedDebugSelectionKind::Path:
    if (FindImportedPath(*artwork, state.selected_imported_debug.item_id) !=
        nullptr) {
      path_ids->insert(state.selected_imported_debug.item_id);
    }
    return;
  case ImportedDebugSelectionKind::DxfText:
    if (FindImportedDxfText(*artwork, state.selected_imported_debug.item_id) !=
        nullptr) {
      text_ids->insert(state.selected_imported_debug.item_id);
    }
    return;
  case ImportedDebugSelectionKind::Artwork:
  case ImportedDebugSelectionKind::None:
    return;
  }
}

template <typename MoveFn, typename SetIssuesFn, typename SetLastFn>
inline ImportedArtworkOperationResult ExtractSelectedImportedElementsShared(
    CanvasState &state, int imported_artwork_id, MoveFn move_fn,
    SetIssuesFn set_issues_fn, SetLastFn set_last_fn) {
  std::unordered_set<int> path_ids;
  std::unordered_set<int> text_ids;
  ResolveExtractedElementIdsShared(state, imported_artwork_id, &path_ids,
                                   &text_ids);

  ImportedArtworkOperationResult result =
      move_fn(state, imported_artwork_id, path_ids, text_ids, " Extracted",
              "Extracted");
  result.selected_count = static_cast<int>(path_ids.size() + text_ids.size());
  set_issues_fn(state, 0, {});
  set_last_fn(state, result);
  return result;
}

template <typename MoveFn, typename PopulateFn, typename SetIssuesFn,
          typename SetLastFn>
inline ImportedArtworkOperationResult SeparateImportedArtworkByGuideShared(
    CanvasState &state, int imported_artwork_id, int guide_id, MoveFn move_fn,
    PopulateFn populate_fn, SetIssuesFn set_issues_fn, SetLastFn set_last_fn) {
  ImportedArtworkOperationResult result;
  result.artwork_id = imported_artwork_id;

  ImportedArtwork *artwork = FindImportedArtwork(state, imported_artwork_id);
  const Guide *guide = FindGuide(state, guide_id);
  if (artwork == nullptr || guide == nullptr) {
    result.message = "Guide split requires a valid guide and imported artwork.";
    set_issues_fn(state, 0, {});
    set_last_fn(state, result);
    return result;
  }

  std::unordered_set<int> positive_path_ids;
  std::unordered_set<int> positive_text_ids;
  int negative_count = 0;
  std::vector<ImVec2> sample_points;
  std::vector<ImportedElementSelection> skipped_elements;

  for (const ImportedPath &path : artwork->paths) {
    sample_points.clear();
    AppendPathSamplePointsWorldShared(*artwork, path, &sample_points);
    switch (ClassifyGuideSideShared(sample_points, *guide)) {
    case SharedGuideSideClassification::Positive:
      positive_path_ids.insert(path.id);
      break;
    case SharedGuideSideClassification::Negative:
      negative_count += 1;
      break;
    case SharedGuideSideClassification::Crossing:
      result.skipped_count += 1;
      skipped_elements.push_back({ImportedElementKind::Path, path.id});
      break;
    case SharedGuideSideClassification::Empty:
      break;
    }
  }

  for (const ImportedDxfText &text : artwork->dxf_text) {
    sample_points.clear();
    AppendTextSamplePointsWorldShared(*artwork, text, &sample_points);
    switch (ClassifyGuideSideShared(sample_points, *guide)) {
    case SharedGuideSideClassification::Positive:
      positive_text_ids.insert(text.id);
      break;
    case SharedGuideSideClassification::Negative:
      negative_count += 1;
      break;
    case SharedGuideSideClassification::Crossing:
      result.skipped_count += 1;
      skipped_elements.push_back({ImportedElementKind::DxfText, text.id});
      break;
    case SharedGuideSideClassification::Empty:
      break;
    }
  }

  const int positive_count =
      static_cast<int>(positive_path_ids.size() + positive_text_ids.size());
  if (positive_count == 0 || negative_count == 0) {
    populate_fn(&result, *artwork);
    result.message =
        "Guide split needs movable content on both sides of the guide.";
    set_issues_fn(state, artwork->id, std::move(skipped_elements));
    set_last_fn(state, result);
    return result;
  }

  const int skipped_count = result.skipped_count;
  result = move_fn(state, imported_artwork_id, positive_path_ids,
                   positive_text_ids, " Guide Split", "Split");
  result.skipped_count += skipped_count;
  result.message +=
      " Skipped " + std::to_string(result.skipped_count) + " crossing element" +
      (result.skipped_count == 1 ? std::string() : std::string("s")) + ".";
  set_issues_fn(state, imported_artwork_id, std::move(skipped_elements));
  set_last_fn(state, result);
  return result;
}

} // namespace im2d::operations::detail