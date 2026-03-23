#include "im2d_operations.h"

#include "im2d_operations_shared.h"

#include "../canvas/im2d_canvas_document.h"
#include "../canvas/im2d_canvas_imported_artwork_ops.h"

#include <clipper2/clipper.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace {

constexpr float kMinimumImportedArtworkScale = 0.01f;
constexpr float kImportedCurveFlatnessTolerance = 0.05f;
constexpr int kImportedCurveMaxSubdivisionDepth = 10;
constexpr int kClipperDecimalPrecision = 3;
constexpr double kMinimumPreparedContourArea = 0.01;
constexpr int kConservativeRefitSampleCount = 64;

float DistanceSquared(const ImVec2 &a, const ImVec2 &b) {
  const float dx = a.x - b.x;
  const float dy = a.y - b.y;
  return dx * dx + dy * dy;
}

bool PointsNear(const ImVec2 &a, const ImVec2 &b, float tolerance) {
  return DistanceSquared(a, b) <= tolerance * tolerance;
}

bool NearlyEqual(float a, float b, float epsilon = 0.0001f) {
  return std::fabs(a - b) <= epsilon;
}

float Distance(const ImVec2 &a, const ImVec2 &b) {
  return std::sqrt(DistanceSquared(a, b));
}

ImVec2 Midpoint(const ImVec2 &a, const ImVec2 &b) {
  return ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
}

float DistancePointToLineSquared(const ImVec2 &point, const ImVec2 &line_start,
                                 const ImVec2 &line_end) {
  const float dx = line_end.x - line_start.x;
  const float dy = line_end.y - line_start.y;
  const float line_length_squared = dx * dx + dy * dy;
  if (line_length_squared <= 0.0000001f) {
    return DistanceSquared(point, line_start);
  }

  const float numerator =
      std::fabs(dy * point.x - dx * point.y + line_end.x * line_start.y -
                line_end.y * line_start.x);
  return (numerator * numerator) / line_length_squared;
}

bool CubicBezierFlatEnough(const ImVec2 &start, const ImVec2 &control1,
                           const ImVec2 &control2, const ImVec2 &end,
                           float tolerance_squared) {
  return DistancePointToLineSquared(control1, start, end) <=
             tolerance_squared &&
         DistancePointToLineSquared(control2, start, end) <= tolerance_squared;
}

void AppendAdaptiveCubicPoints(const ImVec2 &start, const ImVec2 &control1,
                               const ImVec2 &control2, const ImVec2 &end,
                               int depth, std::vector<ImVec2> *sample_points) {
  const float tolerance_squared =
      kImportedCurveFlatnessTolerance * kImportedCurveFlatnessTolerance;
  if (depth >= kImportedCurveMaxSubdivisionDepth ||
      CubicBezierFlatEnough(start, control1, control2, end,
                            tolerance_squared)) {
    sample_points->push_back(end);
    return;
  }

  const ImVec2 start_control1_mid = Midpoint(start, control1);
  const ImVec2 control1_control2_mid = Midpoint(control1, control2);
  const ImVec2 control2_end_mid = Midpoint(control2, end);
  const ImVec2 left_control2 =
      Midpoint(start_control1_mid, control1_control2_mid);
  const ImVec2 right_control1 =
      Midpoint(control1_control2_mid, control2_end_mid);
  const ImVec2 split_point = Midpoint(left_control2, right_control1);

  AppendAdaptiveCubicPoints(start, start_control1_mid, left_control2,
                            split_point, depth + 1, sample_points);
  AppendAdaptiveCubicPoints(split_point, right_control1, control2_end_mid, end,
                            depth + 1, sample_points);
}

void AppendSampledSegmentPointsLocal(
    const std::vector<im2d::ImportedPathSegment> &segments,
    std::vector<ImVec2> *sample_points) {
  if (segments.empty()) {
    return;
  }

  sample_points->push_back(segments.front().start);
  for (const im2d::ImportedPathSegment &segment : segments) {
    if (segment.kind == im2d::ImportedPathSegmentKind::Line) {
      sample_points->push_back(segment.end);
      continue;
    }

    AppendAdaptiveCubicPoints(segment.start, segment.control1, segment.control2,
                              segment.end, 0, sample_points);
  }
}

bool SameImportedSourceReference(const im2d::ImportedSourceReference &a,
                                 const im2d::ImportedSourceReference &b) {
  return a.source_artwork_id == b.source_artwork_id && a.kind == b.kind &&
         a.item_id == b.item_id;
}

bool ImportedSourceReferenceLess(const im2d::ImportedSourceReference &a,
                                 const im2d::ImportedSourceReference &b) {
  if (a.source_artwork_id != b.source_artwork_id) {
    return a.source_artwork_id < b.source_artwork_id;
  }
  if (a.kind != b.kind) {
    return static_cast<int>(a.kind) < static_cast<int>(b.kind);
  }
  return a.item_id < b.item_id;
}

void NormalizeImportedSourceReferences(
    std::vector<im2d::ImportedSourceReference> *references) {
  std::sort(references->begin(), references->end(),
            ImportedSourceReferenceLess);
  references->erase(std::unique(references->begin(), references->end(),
                                SameImportedSourceReference),
                    references->end());
}

im2d::ImportedPath ReverseImportedPathCopy(const im2d::ImportedPath &path) {
  im2d::ImportedPath reversed = path;
  reversed.segments.clear();
  reversed.segments.reserve(path.segments.size());
  for (auto it = path.segments.rbegin(); it != path.segments.rend(); ++it) {
    im2d::ImportedPathSegment segment = *it;
    std::swap(segment.start, segment.end);
    std::swap(segment.control1, segment.control2);
    reversed.segments.push_back(segment);
  }
  return reversed;
}

bool SupportsClipperCleanup(const im2d::ImportedPath &path) {
  return !im2d::HasImportedPathFlag(path.flags,
                                    im2d::ImportedPathFlagTextPlaceholder) &&
         !im2d::HasImportedPathFlag(path.flags,
                                    im2d::ImportedPathFlagFilledText) &&
         !im2d::HasImportedPathFlag(path.flags,
                                    im2d::ImportedPathFlagHoleContour);
}

Clipper2Lib::PathD
SampleImportedPathToClipperPath(const im2d::ImportedPath &path) {
  std::vector<ImVec2> sampled_points;
  AppendSampledSegmentPointsLocal(path.segments, &sampled_points);
  if (sampled_points.size() > 1 &&
      PointsNear(sampled_points.front(), sampled_points.back(), 0.0001f)) {
    sampled_points.pop_back();
  }

  Clipper2Lib::PathD clipper_path;
  clipper_path.reserve(sampled_points.size());
  for (const ImVec2 &point : sampled_points) {
    clipper_path.emplace_back(static_cast<double>(point.x),
                              static_cast<double>(point.y));
  }
  return clipper_path;
}

struct ClosedCleanupGroup {
  int parent_group_id = 0;
  im2d::ImportedPath template_path;
  std::unordered_set<int> source_path_ids;
  std::vector<im2d::ImportedSourceReference> provenance;
  Clipper2Lib::PathsD clipper_paths;
  bool requires_clipper_cleanup = false;
};

struct CollectedClosedContour {
  int parent_group_id = 0;
  im2d::ImportedPath template_path;
  int source_path_id = 0;
  std::vector<im2d::ImportedSourceReference> provenance;
  Clipper2Lib::PathD sampled_path;
  bool requires_cleanup = false;
};

enum class CanonicalRepairSegmentKind {
  Line,
  CubicBezier,
};

struct CanonicalRepairSegment {
  CanonicalRepairSegmentKind kind = CanonicalRepairSegmentKind::Line;
  ImVec2 start = ImVec2(0.0f, 0.0f);
  ImVec2 control1 = ImVec2(0.0f, 0.0f);
  ImVec2 control2 = ImVec2(0.0f, 0.0f);
  ImVec2 end = ImVec2(0.0f, 0.0f);
};

struct CanonicalRepairContour {
  im2d::ImportedPath template_path;
  std::vector<im2d::ImportedSourceReference> provenance;
  std::vector<CanonicalRepairSegment> segments;
  bool exact_polyline = true;
  uint32_t issue_flags = im2d::ImportedElementIssueFlagNone;
};

struct PreparedCleanupOutput {
  std::unordered_set<int> newly_closed_path_ids;
  std::unordered_set<int> replaced_path_ids;
  std::vector<CanonicalRepairContour> repaired_closed_contours;
  int stitched_count = 0;
  int cleaned_count = 0;
  int ambiguous_count = 0;
};

struct PathAttachmentCandidate {
  size_t path_index = 0;
  bool attach_to_front = false;
  bool reverse_path = false;
  float distance_squared = std::numeric_limits<float>::max();
  bool valid = false;
};

struct ContourMatchMetrics {
  float max_distance = 0.0f;
  float average_distance = 0.0f;
};

bool MatchesCleanupGroup(const ClosedCleanupGroup &group,
                         const im2d::ImportedPath &path) {
  return group.parent_group_id == path.parent_group_id &&
         group.template_path.flags == path.flags &&
         NearlyEqual(group.template_path.stroke_width, path.stroke_width) &&
         NearlyEqual(group.template_path.stroke_color.x, path.stroke_color.x) &&
         NearlyEqual(group.template_path.stroke_color.y, path.stroke_color.y) &&
         NearlyEqual(group.template_path.stroke_color.z, path.stroke_color.z) &&
         NearlyEqual(group.template_path.stroke_color.w, path.stroke_color.w);
}

ClosedCleanupGroup *
FindOrAddCleanupGroup(std::vector<ClosedCleanupGroup> *groups,
                      const im2d::ImportedPath &path) {
  for (ClosedCleanupGroup &group : *groups) {
    if (MatchesCleanupGroup(group, path)) {
      return &group;
    }
  }

  groups->push_back({path.parent_group_id, path, {}, {}, {}, false});
  return &groups->back();
}

bool PathStylesMatchForAssembly(const im2d::ImportedPath &first,
                                const im2d::ImportedPath &second) {
  return first.parent_group_id == second.parent_group_id &&
         first.flags == second.flags &&
         NearlyEqual(first.stroke_width, second.stroke_width) &&
         NearlyEqual(first.stroke_color.x, second.stroke_color.x) &&
         NearlyEqual(first.stroke_color.y, second.stroke_color.y) &&
         NearlyEqual(first.stroke_color.z, second.stroke_color.z) &&
         NearlyEqual(first.stroke_color.w, second.stroke_color.w);
}

void MergeImportedPathMetadata(im2d::ImportedPath *target,
                               const im2d::ImportedPath &source) {
  target->issue_flags |= source.issue_flags;
  target->provenance.insert(target->provenance.end(), source.provenance.begin(),
                            source.provenance.end());
  NormalizeImportedSourceReferences(&target->provenance);
}

PathAttachmentCandidate
FindBestPathAttachment(const im2d::ImportedPath &chain,
                       const std::vector<im2d::ImportedPath> &open_paths,
                       const std::vector<bool> &consumed,
                       float weld_tolerance) {
  PathAttachmentCandidate best_candidate;
  const float max_distance_squared = weld_tolerance * weld_tolerance;
  const ImVec2 chain_start = chain.segments.front().start;
  const ImVec2 chain_end = chain.segments.back().end;

  for (size_t path_index = 0; path_index < open_paths.size(); ++path_index) {
    if (consumed[path_index]) {
      continue;
    }

    const im2d::ImportedPath &candidate = open_paths[path_index];
    if (candidate.segments.empty() ||
        !PathStylesMatchForAssembly(chain, candidate)) {
      continue;
    }

    const std::array<std::pair<bool, im2d::ImportedPath>, 2> variants = {
        std::pair<bool, im2d::ImportedPath>{false, candidate},
        std::pair<bool, im2d::ImportedPath>{
            true, ReverseImportedPathCopy(candidate)}};
    for (const auto &[reverse_path, oriented_candidate] : variants) {
      const float append_distance_squared =
          DistanceSquared(chain_end, oriented_candidate.segments.front().start);
      if (append_distance_squared <= max_distance_squared &&
          append_distance_squared < best_candidate.distance_squared) {
        best_candidate = {path_index, false, reverse_path,
                          append_distance_squared, true};
      }

      const float prepend_distance_squared =
          DistanceSquared(oriented_candidate.segments.back().end, chain_start);
      if (prepend_distance_squared <= max_distance_squared &&
          prepend_distance_squared < best_candidate.distance_squared) {
        best_candidate = {path_index, true, reverse_path,
                          prepend_distance_squared, true};
      }
    }
  }

  return best_candidate;
}

void ApplyPathAttachment(im2d::ImportedPath *chain,
                         const PathAttachmentCandidate &attachment,
                         const im2d::ImportedPath &candidate) {
  im2d::ImportedPath oriented_candidate =
      attachment.reverse_path ? ReverseImportedPathCopy(candidate) : candidate;

  if (attachment.attach_to_front) {
    oriented_candidate.segments.back().end = chain->segments.front().start;
    std::vector<im2d::ImportedPathSegment> combined_segments;
    combined_segments.reserve(oriented_candidate.segments.size() +
                              chain->segments.size());
    combined_segments.insert(combined_segments.end(),
                             oriented_candidate.segments.begin(),
                             oriented_candidate.segments.end());
    combined_segments.insert(combined_segments.end(), chain->segments.begin(),
                             chain->segments.end());
    chain->segments = std::move(combined_segments);
  } else {
    oriented_candidate.segments.front().start = chain->segments.back().end;
    chain->segments.insert(chain->segments.end(),
                           oriented_candidate.segments.begin(),
                           oriented_candidate.segments.end());
  }

  MergeImportedPathMetadata(chain, oriented_candidate);
}

std::vector<ImVec2>
BuildClosedPolylinePoints(const std::vector<CanonicalRepairSegment> &segments) {
  std::vector<ImVec2> points;
  if (segments.empty()) {
    return points;
  }

  points.reserve(segments.size());
  points.push_back(segments.front().start);
  for (const CanonicalRepairSegment &segment : segments) {
    points.push_back(segment.end);
  }
  if (points.size() > 1 && PointsNear(points.front(), points.back(), 0.0001f)) {
    points.pop_back();
  }
  return points;
}

float ClosedPolylinePerimeter(const std::vector<ImVec2> &points) {
  if (points.size() < 2) {
    return 0.0f;
  }

  float perimeter = 0.0f;
  for (size_t index = 0; index < points.size(); ++index) {
    perimeter += Distance(points[index], points[(index + 1) % points.size()]);
  }
  return perimeter;
}

ImVec2 SampleClosedPolylineAtDistance(const std::vector<ImVec2> &points,
                                      float distance_along) {
  if (points.empty()) {
    return ImVec2(0.0f, 0.0f);
  }
  if (points.size() == 1) {
    return points.front();
  }

  const float perimeter = ClosedPolylinePerimeter(points);
  if (perimeter <= 0.000001f) {
    return points.front();
  }

  float remaining = std::fmod(distance_along, perimeter);
  if (remaining < 0.0f) {
    remaining += perimeter;
  }
  for (size_t index = 0; index < points.size(); ++index) {
    const ImVec2 &start = points[index];
    const ImVec2 &end = points[(index + 1) % points.size()];
    const float segment_length = Distance(start, end);
    if (segment_length <= 0.000001f) {
      continue;
    }
    if (remaining <= segment_length) {
      const float t = remaining / segment_length;
      return ImVec2(start.x + (end.x - start.x) * t,
                    start.y + (end.y - start.y) * t);
    }
    remaining -= segment_length;
  }

  return points.front();
}

std::vector<ImVec2> ResampleClosedPolyline(const std::vector<ImVec2> &points,
                                           int sample_count) {
  std::vector<ImVec2> samples;
  if (points.empty() || sample_count <= 0) {
    return samples;
  }

  const float perimeter = ClosedPolylinePerimeter(points);
  if (perimeter <= 0.000001f) {
    return {points.front()};
  }

  samples.reserve(static_cast<size_t>(sample_count));
  const float step = perimeter / static_cast<float>(sample_count);
  for (int sample_index = 0; sample_index < sample_count; ++sample_index) {
    samples.push_back(
        SampleClosedPolylineAtDistance(points, step * sample_index));
  }
  return samples;
}

ContourMatchMetrics
MeasureClosedContourMatch(const std::vector<ImVec2> &reference_samples,
                          const std::vector<ImVec2> &candidate_samples) {
  ContourMatchMetrics best_metrics;
  best_metrics.max_distance = std::numeric_limits<float>::max();
  best_metrics.average_distance = std::numeric_limits<float>::max();
  if (reference_samples.size() != candidate_samples.size() ||
      reference_samples.empty()) {
    return best_metrics;
  }

  const size_t sample_count = reference_samples.size();
  for (int reverse = 0; reverse < 2; ++reverse) {
    for (size_t offset = 0; offset < sample_count; ++offset) {
      float max_distance = 0.0f;
      float total_distance = 0.0f;
      for (size_t index = 0; index < sample_count; ++index) {
        const size_t candidate_index =
            reverse == 0 ? (index + offset) % sample_count
                         : (sample_count + offset - index) % sample_count;
        const float distance = Distance(reference_samples[index],
                                        candidate_samples[candidate_index]);
        max_distance = std::max(max_distance, distance);
        total_distance += distance;
      }

      const float average_distance = total_distance / sample_count;
      if (max_distance < best_metrics.max_distance ||
          (NearlyEqual(max_distance, best_metrics.max_distance, 0.0001f) &&
           average_distance < best_metrics.average_distance)) {
        best_metrics.max_distance = max_distance;
        best_metrics.average_distance = average_distance;
      }
    }
  }

  return best_metrics;
}

std::vector<CanonicalRepairSegment>
BuildCanonicalSegmentsFromImportedPath(const im2d::ImportedPath &path) {
  std::vector<CanonicalRepairSegment> segments;
  segments.reserve(path.segments.size());
  for (const im2d::ImportedPathSegment &path_segment : path.segments) {
    CanonicalRepairSegment segment;
    segment.kind =
        path_segment.kind == im2d::ImportedPathSegmentKind::CubicBezier
            ? CanonicalRepairSegmentKind::CubicBezier
            : CanonicalRepairSegmentKind::Line;
    segment.start = path_segment.start;
    segment.control1 = path_segment.control1;
    segment.control2 = path_segment.control2;
    segment.end = path_segment.end;
    segments.push_back(segment);
  }
  return segments;
}

void ApplyConservativeTemplateRefit(CanonicalRepairContour *contour,
                                    float weld_tolerance) {
  const im2d::ImportedPath &template_path = contour->template_path;
  const bool has_cubic_segment = std::any_of(
      template_path.segments.begin(), template_path.segments.end(),
      [](const im2d::ImportedPathSegment &segment) {
        return segment.kind == im2d::ImportedPathSegmentKind::CubicBezier;
      });
  if (!has_cubic_segment || contour->segments.empty()) {
    return;
  }

  std::vector<ImVec2> template_points;
  AppendSampledSegmentPointsLocal(template_path.segments, &template_points);
  if (template_points.size() > 1 &&
      PointsNear(template_points.front(), template_points.back(), 0.0001f)) {
    template_points.pop_back();
  }

  const std::vector<ImVec2> repaired_points =
      BuildClosedPolylinePoints(contour->segments);
  if (template_points.size() < 3 || repaired_points.size() < 3) {
    return;
  }

  const std::vector<ImVec2> template_samples =
      ResampleClosedPolyline(template_points, kConservativeRefitSampleCount);
  const std::vector<ImVec2> repaired_samples =
      ResampleClosedPolyline(repaired_points, kConservativeRefitSampleCount);
  if (template_samples.size() != repaired_samples.size() ||
      template_samples.empty()) {
    return;
  }

  const ContourMatchMetrics metrics =
      MeasureClosedContourMatch(template_samples, repaired_samples);
  const float max_allowed_distance = std::max(0.03f, weld_tolerance * 0.15f);
  const float average_allowed_distance =
      std::max(0.01f, weld_tolerance * 0.05f);
  if (metrics.max_distance > max_allowed_distance ||
      metrics.average_distance > average_allowed_distance) {
    return;
  }

  contour->segments = BuildCanonicalSegmentsFromImportedPath(template_path);
  contour->exact_polyline = false;
}

void RebuildOpenPathsByGroup(
    im2d::ImportedArtwork &artwork,
    const std::unordered_map<int, std::vector<im2d::ImportedPath>>
        &rebuilt_group_paths,
    const std::unordered_set<int> &replaced_path_ids) {
  if (rebuilt_group_paths.empty()) {
    return;
  }

  std::erase_if(artwork.paths,
                [&replaced_path_ids](const im2d::ImportedPath &path) {
                  return replaced_path_ids.contains(path.id);
                });

  for (im2d::ImportedGroup &group : artwork.groups) {
    auto rebuilt_it = rebuilt_group_paths.find(group.id);
    if (rebuilt_it == rebuilt_group_paths.end()) {
      continue;
    }

    std::erase_if(group.path_ids, [&replaced_path_ids](int path_id) {
      return replaced_path_ids.contains(path_id);
    });
    for (const im2d::ImportedPath &path : rebuilt_it->second) {
      group.path_ids.push_back(path.id);
    }
  }

  for (const auto &[group_id, group_paths] : rebuilt_group_paths) {
    (void)group_id;
    artwork.paths.insert(artwork.paths.end(), group_paths.begin(),
                         group_paths.end());
  }
}

void CloseNearlyClosedOpenPaths(
    im2d::ImportedArtwork &artwork, float weld_tolerance,
    std::unordered_set<int> *newly_closed_path_ids) {
  for (im2d::ImportedPath &path : artwork.paths) {
    if (path.segments.empty() || path.closed) {
      continue;
    }
    if (!PointsNear(path.segments.front().start, path.segments.back().end,
                    weld_tolerance)) {
      continue;
    }

    path.segments.back().end = path.segments.front().start;
    path.closed = true;
    newly_closed_path_ids->insert(path.id);
  }
}

int StitchOpenPaths(im2d::ImportedArtwork &artwork, float weld_tolerance,
                    std::unordered_set<int> *newly_closed_path_ids) {
  int stitched_count = 0;
  std::unordered_map<int, std::vector<im2d::ImportedPath>> rebuilt_group_paths;
  std::unordered_set<int> replaced_path_ids;

  for (const im2d::ImportedGroup &group : artwork.groups) {
    std::vector<im2d::ImportedPath> open_paths;
    for (const im2d::ImportedPath &path : artwork.paths) {
      if (path.parent_group_id == group.id && !path.closed &&
          !path.segments.empty()) {
        open_paths.push_back(path);
      }
    }
    if (open_paths.empty()) {
      continue;
    }

    for (const im2d::ImportedPath &path : open_paths) {
      replaced_path_ids.insert(path.id);
    }

    std::vector<bool> consumed(open_paths.size(), false);
    std::vector<im2d::ImportedPath> rebuilt_paths;
    rebuilt_paths.reserve(open_paths.size());
    for (size_t start_index = 0; start_index < open_paths.size();
         ++start_index) {
      if (consumed[start_index]) {
        continue;
      }

      im2d::ImportedPath chain = open_paths[start_index];
      consumed[start_index] = true;
      int source_count = 1;

      while (true) {
        const PathAttachmentCandidate attachment =
            FindBestPathAttachment(chain, open_paths, consumed, weld_tolerance);
        if (!attachment.valid) {
          break;
        }

        ApplyPathAttachment(&chain, attachment,
                            open_paths[attachment.path_index]);
        consumed[attachment.path_index] = true;
        source_count += 1;
      }

      if (source_count > 1) {
        stitched_count += source_count - 1;
      }

      if (PointsNear(chain.segments.front().start, chain.segments.back().end,
                     weld_tolerance)) {
        chain.segments.back().end = chain.segments.front().start;
        chain.closed = true;
        newly_closed_path_ids->insert(chain.id);
      }

      rebuilt_paths.push_back(std::move(chain));
    }

    rebuilt_group_paths.insert_or_assign(group.id, std::move(rebuilt_paths));
  }

  RebuildOpenPathsByGroup(artwork, rebuilt_group_paths, replaced_path_ids);

  return stitched_count;
}

std::vector<CollectedClosedContour>
CollectClosedContours(const im2d::ImportedArtwork &artwork,
                      const std::unordered_set<int> &newly_closed_path_ids) {
  std::vector<CollectedClosedContour> collected_contours;
  for (const im2d::ImportedPath &path : artwork.paths) {
    if (!path.closed || path.segments.empty() ||
        !SupportsClipperCleanup(path)) {
      continue;
    }

    Clipper2Lib::PathD clipper_path = SampleImportedPathToClipperPath(path);
    if (clipper_path.size() < 3) {
      continue;
    }

    collected_contours.push_back({path.parent_group_id, path, path.id,
                                  path.provenance, std::move(clipper_path),
                                  newly_closed_path_ids.contains(path.id)});
  }

  return collected_contours;
}

std::vector<ClosedCleanupGroup> CollectClosedCleanupGroups(
    const std::vector<CollectedClosedContour> &collected_contours) {
  std::vector<ClosedCleanupGroup> cleanup_groups;
  for (const CollectedClosedContour &contour : collected_contours) {
    ClosedCleanupGroup *group =
        FindOrAddCleanupGroup(&cleanup_groups, contour.template_path);
    group->source_path_ids.insert(contour.source_path_id);
    group->provenance.insert(group->provenance.end(),
                             contour.provenance.begin(),
                             contour.provenance.end());
    group->clipper_paths.push_back(contour.sampled_path);
    group->requires_clipper_cleanup =
        group->requires_clipper_cleanup || contour.requires_cleanup;
  }

  return cleanup_groups;
}

void RunClosedPathCleanup(im2d::ImportedArtwork &artwork, float weld_tolerance,
                          std::vector<ClosedCleanupGroup> *cleanup_groups,
                          PreparedCleanupOutput *output) {
  for (ClosedCleanupGroup &group : *cleanup_groups) {
    if (!group.requires_clipper_cleanup && group.clipper_paths.size() < 2) {
      continue;
    }

    Clipper2Lib::PathsD prepared_paths = group.clipper_paths;
    if (weld_tolerance > 0.0f) {
      Clipper2Lib::PathsD inflated = Clipper2Lib::InflatePaths(
          prepared_paths, static_cast<double>(weld_tolerance) * 0.5,
          Clipper2Lib::JoinType::Round, Clipper2Lib::EndType::Polygon, 2.0,
          kClipperDecimalPrecision);
      if (!inflated.empty()) {
        Clipper2Lib::PathsD deflated = Clipper2Lib::InflatePaths(
            inflated, static_cast<double>(-weld_tolerance) * 0.5,
            Clipper2Lib::JoinType::Round, Clipper2Lib::EndType::Polygon, 2.0,
            kClipperDecimalPrecision);
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

    NormalizeImportedSourceReferences(&group.provenance);
    const bool ambiguous_cleanup =
        group.source_path_ids.size() > 1 ||
        unified_paths.size() != group.source_path_ids.size();
    const uint32_t output_issue_flags =
        ambiguous_cleanup ? static_cast<uint32_t>(
                                im2d::ImportedElementIssueFlagAmbiguousCleanup)
                          : im2d::ImportedElementIssueFlagNone;
    const int repaired_start_index =
        static_cast<int>(output->repaired_closed_contours.size());
    for (const Clipper2Lib::PathD &unified_path : unified_paths) {
      if (unified_path.size() < 3 ||
          std::fabs(Clipper2Lib::Area(unified_path)) <
              kMinimumPreparedContourArea) {
        continue;
      }

      CanonicalRepairContour repaired_contour;
      repaired_contour.template_path = group.template_path;
      repaired_contour.provenance = group.provenance;
      repaired_contour.exact_polyline = true;
      repaired_contour.issue_flags = output_issue_flags;
      repaired_contour.segments.reserve(unified_path.size());
      for (size_t index = 0; index < unified_path.size(); ++index) {
        const Clipper2Lib::PointD &start = unified_path[index];
        const Clipper2Lib::PointD &end =
            unified_path[(index + 1) % unified_path.size()];
        CanonicalRepairSegment segment;
        segment.kind = CanonicalRepairSegmentKind::Line;
        segment.start =
            ImVec2(static_cast<float>(start.x), static_cast<float>(start.y));
        segment.end =
            ImVec2(static_cast<float>(end.x), static_cast<float>(end.y));
        repaired_contour.segments.push_back(segment);
      }
      output->repaired_closed_contours.push_back(std::move(repaired_contour));
      ApplyConservativeTemplateRefit(&output->repaired_closed_contours.back(),
                                     weld_tolerance);
    }

    const int repaired_count =
        static_cast<int>(output->repaired_closed_contours.size()) -
        repaired_start_index;
    if (repaired_count == 0) {
      continue;
    }

    for (int source_path_id : group.source_path_ids) {
      output->replaced_path_ids.insert(source_path_id);
    }
    output->cleaned_count += repaired_count;
    if (ambiguous_cleanup) {
      output->ambiguous_count += repaired_count;
    }
  }
}

std::vector<im2d::ImportedPath>
MaterializeRepairedClosedContours(im2d::ImportedArtwork &artwork,
                                  const PreparedCleanupOutput &output) {
  std::vector<im2d::ImportedPath> cleaned_paths;
  cleaned_paths.reserve(output.repaired_closed_contours.size());
  for (const CanonicalRepairContour &contour :
       output.repaired_closed_contours) {
    im2d::ImportedPath cleaned_path = contour.template_path;
    cleaned_path.id = artwork.next_path_id++;
    cleaned_path.closed = true;
    cleaned_path.flags &=
        ~static_cast<uint32_t>(im2d::ImportedPathFlagHoleContour);
    cleaned_path.issue_flags = contour.issue_flags;
    cleaned_path.provenance = contour.provenance;
    NormalizeImportedSourceReferences(&cleaned_path.provenance);
    cleaned_path.segments.clear();
    cleaned_path.segments.reserve(contour.segments.size());

    for (const CanonicalRepairSegment &segment : contour.segments) {
      im2d::ImportedPathSegment imported_segment;
      imported_segment.kind =
          segment.kind == CanonicalRepairSegmentKind::CubicBezier
              ? im2d::ImportedPathSegmentKind::CubicBezier
              : im2d::ImportedPathSegmentKind::Line;
      imported_segment.start = segment.start;
      imported_segment.control1 = segment.control1;
      imported_segment.control2 = segment.control2;
      imported_segment.end = segment.end;
      cleaned_path.segments.push_back(imported_segment);
    }

    cleaned_paths.push_back(std::move(cleaned_path));
  }
  return cleaned_paths;
}

void ApplyCleanedPaths(im2d::ImportedArtwork &artwork,
                       const PreparedCleanupOutput &output) {
  if (output.replaced_path_ids.empty()) {
    return;
  }

  const std::vector<im2d::ImportedPath> cleaned_paths =
      MaterializeRepairedClosedContours(artwork, output);

  std::erase_if(artwork.paths, [&output](const im2d::ImportedPath &path) {
    return output.replaced_path_ids.contains(path.id);
  });

  for (im2d::ImportedGroup &group : artwork.groups) {
    std::erase_if(group.path_ids, [&output](int path_id) {
      return output.replaced_path_ids.contains(path_id);
    });
  }

  for (const im2d::ImportedPath &cleaned_path : cleaned_paths) {
    if (im2d::ImportedGroup *group =
            im2d::FindImportedGroup(artwork, cleaned_path.parent_group_id);
        group != nullptr) {
      group->path_ids.push_back(cleaned_path.id);
    }
    artwork.paths.push_back(cleaned_path);
  }
}

void CountPreparedPathStates(const im2d::ImportedArtwork &artwork,
                             im2d::ImportedArtworkOperationResult *result) {
  result->closed_count = 0;
  result->open_count = 0;
  for (const im2d::ImportedPath &path : artwork.paths) {
    if (path.closed) {
      result->closed_count += 1;
    } else {
      result->open_count += 1;
    }
  }
}

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
    std::vector<im2d::ImportedElementSelection> issue_elements) {
  state.last_imported_operation_issue_artwork_id = artwork_id;
  state.last_imported_operation_issue_elements = std::move(issue_elements);
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
    }
  }
  for (const im2d::ImportedDxfText &text : source.dxf_text) {
    if (text_ids.contains(text.id)) {
      subset.dxf_text.push_back(text);
    }
  }

  ResetImportedArtworkCounters(subset);
  im2d::RecomputeImportedArtworkBounds(subset);
  im2d::RecomputeImportedHierarchyBounds(subset);
  im2d::RefreshImportedArtworkPartMetadata(subset);
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
      im2d::AppendImportedArtwork(state, std::move(subset));
  if (source_empty) {
    im2d::DeleteImportedArtwork(state, imported_artwork_id);
  }

  im2d::ClearSelectedImportedElements(state);
  state.selected_imported_artwork_id = created_artwork_id;
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
PrepareImportedArtworkForCutting(CanvasState &state, int imported_artwork_id,
                                 float weld_tolerance) {
  return detail::PrepareImportedArtworkForCuttingShared(
      state, imported_artwork_id, weld_tolerance,
      [](ImportedArtwork &artwork, float callback_weld_tolerance,
         ImportedArtworkOperationResult *result) {
        PreparedCleanupOutput cleanup_output;

        CloseNearlyClosedOpenPaths(artwork, callback_weld_tolerance,
                                   &cleanup_output.newly_closed_path_ids);
        cleanup_output.stitched_count =
            StitchOpenPaths(artwork, callback_weld_tolerance,
                            &cleanup_output.newly_closed_path_ids);

        const std::vector<CollectedClosedContour> collected_contours =
            CollectClosedContours(artwork,
                                  cleanup_output.newly_closed_path_ids);
        std::vector<ClosedCleanupGroup> cleanup_groups =
            CollectClosedCleanupGroups(collected_contours);
        RunClosedPathCleanup(artwork, callback_weld_tolerance, &cleanup_groups,
                             &cleanup_output);
        ApplyCleanedPaths(artwork, cleanup_output);

        result->stitched_count = cleanup_output.stitched_count;
        result->cleaned_count = cleanup_output.cleaned_count;
        result->ambiguous_count = cleanup_output.ambiguous_count;
      },
      [](const ImportedArtwork &artwork,
         ImportedArtworkOperationResult *result) {
        CountPreparedPathStates(artwork, result);
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
SeparateImportedArtworkByGuide(CanvasState &state, int imported_artwork_id,
                               int guide_id) {
  return detail::SeparateImportedArtworkByGuideShared(
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
  return detail::DeleteImportedArtworkShared(state, imported_artwork_id);
}

} // namespace im2d::operations