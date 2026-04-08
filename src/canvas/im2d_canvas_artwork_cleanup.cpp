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
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace im2d {
namespace detail {

constexpr int kClipperDecimalPrecision = 3;
constexpr size_t kMaxLegacyFallbackOpenPaths = 512;

int64_t PackEndpointBucketKey(int bucket_x, int bucket_y) {
  return (static_cast<int64_t>(bucket_x) << 32) ^
         static_cast<uint32_t>(bucket_y);
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

struct AutoClosePolylineStageResult {
  std::unordered_set<int> newly_closed_path_ids;
};

enum class ImportedPathEndpointKind {
  Start,
  End,
};

struct ImportedPathEndpointKey {
  int path_id = 0;
  ImportedPathEndpointKind endpoint = ImportedPathEndpointKind::Start;
};

struct OpenPathEndpoint {
  ImportedPathEndpointKey key;
  ImVec2 point = ImVec2(0.0f, 0.0f);
};

struct AutoCloseGraphEdge {
  int path_id = 0;
  int start_cluster_id = -1;
  int end_cluster_id = -1;
};

struct AutoCloseAssemblyPlan {
  std::vector<int> ordered_path_ids;
  std::vector<bool> reverse_path;
  bool closed = false;
};

struct AutoCloseProfilingSummary {
  int endpoint_count = 0;
  int cluster_count = 0;
  int compatible_group_count = 0;
  int component_count = 0;
  int assembly_pass_count = 0;
  float elapsed_ms = 0.0f;
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

// Vector math functions from im2d_vector_math.h are used directly via
// the im2d namespace (using-declarations below bring them into scope).
using im2d::CrossProduct;
using im2d::DotProduct;
using im2d::Midpoint;
using im2d::NormalizeVector;
using im2d::SubtractPoints;
using im2d::VectorLengthSquared;

bool TryComputeLineJoinPoint(const ImportedPathSegment &incoming,
                             const ImportedPathSegment &outgoing,
                             float tolerance, ImVec2 *join_point) {
  if (join_point == nullptr || incoming.kind != ImportedPathSegmentKind::Line ||
      outgoing.kind != ImportedPathSegmentKind::Line) {
    return false;
  }

  const ImVec2 incoming_direction(incoming.end.x - incoming.start.x,
                                  incoming.end.y - incoming.start.y);
  const ImVec2 outgoing_direction(outgoing.end.x - outgoing.start.x,
                                  outgoing.end.y - outgoing.start.y);
  const float determinant =
      CrossProduct(incoming_direction, outgoing_direction);
  if (std::fabs(determinant) <= 0.000001f) {
    return false;
  }

  const ImVec2 origin_delta(outgoing.start.x - incoming.start.x,
                            outgoing.start.y - incoming.start.y);
  const float incoming_scale =
      CrossProduct(origin_delta, outgoing_direction) / determinant;
  const float outgoing_scale =
      CrossProduct(origin_delta, incoming_direction) / determinant;

  if (incoming_scale < 1.0f - kLineJoinScaleTolerance ||
      outgoing_scale > kLineJoinScaleTolerance) {
    return false;
  }

  const ImVec2 candidate(
      incoming.start.x + incoming_direction.x * incoming_scale,
      incoming.start.y + incoming_direction.y * incoming_scale);
  if (!detail::PointsNear(candidate, incoming.end, tolerance) ||
      !detail::PointsNear(candidate, outgoing.start, tolerance)) {
    return false;
  }

  *join_point = candidate;
  return true;
}

bool HasAcceptableLinearContinuation(const ImportedPathSegment &incoming,
                                     const ImportedPathSegment &outgoing) {
  if (incoming.kind != ImportedPathSegmentKind::Line ||
      outgoing.kind != ImportedPathSegmentKind::Line) {
    return true;
  }

  ImVec2 incoming_direction;
  ImVec2 outgoing_direction;
  if (!NormalizeVector(ImVec2(incoming.end.x - incoming.start.x,
                              incoming.end.y - incoming.start.y),
                       &incoming_direction) ||
      !NormalizeVector(ImVec2(outgoing.end.x - outgoing.start.x,
                              outgoing.end.y - outgoing.start.y),
                       &outgoing_direction)) {
    return false;
  }

  return DotProduct(incoming_direction, outgoing_direction) >=
         kStraightJoinDotThreshold;
}

bool TryGetSegmentTerminalDirection(const ImportedPathSegment &segment,
                                    ImportedPathEndpointKind endpoint,
                                    ImVec2 *direction) {
  if (direction == nullptr) {
    return false;
  }

  if (segment.kind == ImportedPathSegmentKind::Line) {
    if (endpoint == ImportedPathEndpointKind::Start) {
      return NormalizeVector(SubtractPoints(segment.end, segment.start),
                             direction);
    }
    return NormalizeVector(SubtractPoints(segment.end, segment.start),
                           direction);
  }

  if (endpoint == ImportedPathEndpointKind::Start) {
    if (NormalizeVector(SubtractPoints(segment.control1, segment.start),
                        direction)) {
      return true;
    }
    if (NormalizeVector(SubtractPoints(segment.control2, segment.start),
                        direction)) {
      return true;
    }
    return NormalizeVector(SubtractPoints(segment.end, segment.start),
                           direction);
  }

  if (NormalizeVector(SubtractPoints(segment.end, segment.control2),
                      direction)) {
    return true;
  }
  if (NormalizeVector(SubtractPoints(segment.end, segment.control1),
                      direction)) {
    return true;
  }
  return NormalizeVector(SubtractPoints(segment.end, segment.start), direction);
}

bool HasAcceptableTangentContinuation(const ImportedPathSegment &incoming,
                                      const ImportedPathSegment &outgoing) {
  ImVec2 incoming_direction;
  ImVec2 outgoing_direction;
  if (!TryGetSegmentTerminalDirection(incoming, ImportedPathEndpointKind::End,
                                      &incoming_direction) ||
      !TryGetSegmentTerminalDirection(outgoing, ImportedPathEndpointKind::Start,
                                      &outgoing_direction)) {
    return false;
  }

  return DotProduct(incoming_direction, outgoing_direction) >=
         kTangentJoinDotThreshold;
}

bool TryComputeTangentJoinPoint(const ImportedPathSegment &incoming,
                                const ImportedPathSegment &outgoing,
                                float tolerance, ImVec2 *join_point) {
  if (join_point == nullptr) {
    return false;
  }

  ImVec2 incoming_direction;
  ImVec2 outgoing_direction;
  if (!TryGetSegmentTerminalDirection(incoming, ImportedPathEndpointKind::End,
                                      &incoming_direction) ||
      !TryGetSegmentTerminalDirection(outgoing, ImportedPathEndpointKind::Start,
                                      &outgoing_direction)) {
    return false;
  }

  const ImVec2 outgoing_join_direction(-outgoing_direction.x,
                                       -outgoing_direction.y);
  const float determinant =
      CrossProduct(incoming_direction, outgoing_join_direction);
  if (std::fabs(determinant) <= 0.000001f) {
    return false;
  }

  const ImVec2 origin_delta(outgoing.start.x - incoming.end.x,
                            outgoing.start.y - incoming.end.y);
  const float incoming_scale =
      CrossProduct(origin_delta, outgoing_join_direction) / determinant;
  const float outgoing_scale =
      CrossProduct(origin_delta, incoming_direction) / determinant;

  if (incoming_scale < -kLineJoinScaleTolerance ||
      outgoing_scale < -kLineJoinScaleTolerance) {
    return false;
  }

  const ImVec2 candidate(incoming.end.x + incoming_direction.x * incoming_scale,
                         incoming.end.y +
                             incoming_direction.y * incoming_scale);
  const float max_join_distance = std::max(tolerance, 0.35f);
  if (!detail::PointsNear(candidate, incoming.end, max_join_distance) ||
      !detail::PointsNear(candidate, outgoing.start, max_join_distance)) {
    return false;
  }

  *join_point = candidate;
  return true;
}

bool IsSmallJoinGap(const ImportedPathSegment &incoming,
                    const ImportedPathSegment &outgoing, float tolerance) {
  const float max_gap = std::max(0.05f, tolerance * 0.2f);
  return detail::PointsNear(incoming.end, outgoing.start, max_gap);
}

bool TryComputeJoinPoint(const ImportedPathSegment &incoming,
                         const ImportedPathSegment &outgoing, float tolerance,
                         ImVec2 *join_point) {
  if (join_point == nullptr) {
    return false;
  }
  if (TryComputeLineJoinPoint(incoming, outgoing, tolerance, join_point)) {
    return true;
  }
  if (TryComputeTangentJoinPoint(incoming, outgoing, tolerance, join_point)) {
    return true;
  }
  if (incoming.kind == ImportedPathSegmentKind::Line &&
      outgoing.kind == ImportedPathSegmentKind::Line &&
      !HasAcceptableLinearContinuation(incoming, outgoing)) {
    return false;
  }
  if (!IsSmallJoinGap(incoming, outgoing, tolerance) &&
      !HasAcceptableTangentContinuation(incoming, outgoing)) {
    return false;
  }
  *join_point = Midpoint(incoming.end, outgoing.start);
  return true;
}

float ComputeJoinDirectionDot(const ImportedPathSegment &incoming,
                              const ImportedPathSegment &outgoing) {
  ImVec2 incoming_direction;
  ImVec2 outgoing_direction;
  if (!TryGetSegmentTerminalDirection(incoming, ImportedPathEndpointKind::End,
                                      &incoming_direction) ||
      !TryGetSegmentTerminalDirection(outgoing, ImportedPathEndpointKind::Start,
                                      &outgoing_direction)) {
    return -1.0f;
  }
  return DotProduct(incoming_direction, outgoing_direction);
}

int ComputeJoinQualityRank(const ImportedPathSegment &incoming,
                           const ImportedPathSegment &outgoing,
                           float tolerance) {
  ImVec2 join_point;
  if (TryComputeLineJoinPoint(incoming, outgoing, tolerance, &join_point)) {
    return 3;
  }
  if (TryComputeTangentJoinPoint(incoming, outgoing, tolerance, &join_point)) {
    return 2;
  }
  if (IsSmallJoinGap(incoming, outgoing, tolerance)) {
    return 1;
  }
  return 0;
}

struct LegacyMergeCandidate {
  size_t second_index = 0;
  ImportedPath first;
  ImportedPath second;
  float gap_distance_squared = 0.0f;
  float direction_dot = -1.0f;
  int quality_rank = -1;
};

bool IsBetterLegacyMergeCandidate(const LegacyMergeCandidate &candidate,
                                  const LegacyMergeCandidate &best_candidate) {
  const bool candidate_is_straight = candidate.direction_dot >= 0.985f;
  const bool best_is_straight = best_candidate.direction_dot >= 0.985f;
  if (candidate_is_straight != best_is_straight) {
    return candidate_is_straight;
  }
  if (candidate.quality_rank != best_candidate.quality_rank) {
    return candidate.quality_rank > best_candidate.quality_rank;
  }
  if (std::fabs(candidate.direction_dot - best_candidate.direction_dot) >
      0.0001f) {
    return candidate.direction_dot > best_candidate.direction_dot;
  }
  if (std::fabs(candidate.gap_distance_squared -
                best_candidate.gap_distance_squared) > 0.000001f) {
    return candidate.gap_distance_squared < best_candidate.gap_distance_squared;
  }
  return candidate.second.id < best_candidate.second.id;
}

bool AlignMergeJoinPoint(ImportedPath *incoming, ImportedPath *outgoing,
                         float tolerance) {
  if (incoming == nullptr || outgoing == nullptr ||
      incoming->segments.empty() || outgoing->segments.empty()) {
    return false;
  }
  ImVec2 join_point;
  if (!TryComputeJoinPoint(incoming->segments.back(),
                           outgoing->segments.front(), tolerance,
                           &join_point)) {
    return false;
  }
  incoming->segments.back().end = join_point;
  outgoing->segments.front().start = join_point;
  return true;
}

bool AlignClosureJoinPoint(ImportedPath *path, float tolerance) {
  if (path == nullptr || path->segments.empty()) {
    return false;
  }
  ImVec2 join_point;
  if (!TryComputeJoinPoint(path->segments.back(), path->segments.front(),
                           tolerance, &join_point)) {
    return false;
  }
  path->segments.front().start = join_point;
  path->segments.back().end = join_point;
  return true;
}

bool AutoClosePathsCompatible(const ImportedPath &first,
                              const ImportedPath &second) {
  return first.parent_group_id == second.parent_group_id &&
         first.flags == second.flags &&
         std::fabs(first.stroke_width - second.stroke_width) <= 0.0001f &&
         std::fabs(first.stroke_color.x - second.stroke_color.x) <= 0.0001f &&
         std::fabs(first.stroke_color.y - second.stroke_color.y) <= 0.0001f &&
         std::fabs(first.stroke_color.z - second.stroke_color.z) <= 0.0001f &&
         std::fabs(first.stroke_color.w - second.stroke_color.w) <= 0.0001f;
}

void GroupCompatibleOpenPathIds(const ImportedArtwork &artwork,
                                std::vector<std::vector<int>> *path_groups) {
  path_groups->clear();
  std::vector<const ImportedPath *> representative_paths;
  for (const ImportedPath &path : artwork.paths) {
    if (path.closed || path.segments.empty()) {
      continue;
    }

    bool added = false;
    for (size_t group_index = 0; group_index < path_groups->size();
         ++group_index) {
      if (AutoClosePathsCompatible(*representative_paths[group_index], path)) {
        path_groups->at(group_index).push_back(path.id);
        added = true;
        break;
      }
    }

    if (!added) {
      path_groups->push_back({path.id});
      representative_paths.push_back(&path);
    }
  }
}

void CollectOpenPathEndpoints(
    const ImportedArtwork &artwork, const std::vector<int> &path_ids,
    const std::unordered_map<int, size_t> &path_index_by_id,
    std::vector<OpenPathEndpoint> *endpoints) {
  endpoints->clear();
  endpoints->reserve(path_ids.size() * 2);
  for (const int path_id : path_ids) {
    const auto path_it = path_index_by_id.find(path_id);
    if (path_it == path_index_by_id.end()) {
      continue;
    }
    const ImportedPath &path = artwork.paths[path_it->second];
    if (path.closed || path.segments.empty()) {
      continue;
    }
    endpoints->push_back({{path.id, ImportedPathEndpointKind::Start},
                          path.segments.front().start});
    endpoints->push_back(
        {{path.id, ImportedPathEndpointKind::End}, path.segments.back().end});
  }
}

void ComputeEndpointClusters(const std::vector<OpenPathEndpoint> &endpoints,
                             float tolerance, std::vector<int> *cluster_ids,
                             std::vector<std::vector<size_t>> *clusters) {
  cluster_ids->assign(endpoints.size(), -1);
  clusters->clear();
  if (endpoints.empty()) {
    return;
  }
  if (tolerance <= 0.0f) {
    clusters->reserve(endpoints.size());
    for (size_t index = 0; index < endpoints.size(); ++index) {
      cluster_ids->at(index) = static_cast<int>(index);
      clusters->push_back({index});
    }
    return;
  }

  std::unordered_map<int64_t, std::vector<size_t>> bucket_points;
  bucket_points.reserve(endpoints.size());
  for (size_t index = 0; index < endpoints.size(); ++index) {
    const int bucket_x =
        static_cast<int>(std::floor(endpoints[index].point.x / tolerance));
    const int bucket_y =
        static_cast<int>(std::floor(endpoints[index].point.y / tolerance));
    bucket_points[PackEndpointBucketKey(bucket_x, bucket_y)].push_back(index);
  }

  std::vector<bool> visited(endpoints.size(), false);
  for (size_t index = 0; index < endpoints.size(); ++index) {
    if (visited[index]) {
      continue;
    }

    std::vector<size_t> stack = {index};
    std::vector<size_t> cluster_indices;
    visited[index] = true;
    while (!stack.empty()) {
      const size_t current = stack.back();
      stack.pop_back();
      cluster_indices.push_back(current);
      const int bucket_x =
          static_cast<int>(std::floor(endpoints[current].point.x / tolerance));
      const int bucket_y =
          static_cast<int>(std::floor(endpoints[current].point.y / tolerance));
      for (int offset_y = -1; offset_y <= 1; ++offset_y) {
        for (int offset_x = -1; offset_x <= 1; ++offset_x) {
          const auto bucket_it = bucket_points.find(
              PackEndpointBucketKey(bucket_x + offset_x, bucket_y + offset_y));
          if (bucket_it == bucket_points.end()) {
            continue;
          }
          for (const size_t candidate : bucket_it->second) {
            if (visited[candidate]) {
              continue;
            }
            if (!detail::PointsNear(endpoints[current].point,
                                    endpoints[candidate].point, tolerance)) {
              continue;
            }
            visited[candidate] = true;
            stack.push_back(candidate);
          }
        }
      }
    }

    const int cluster_id = static_cast<int>(clusters->size());
    clusters->push_back(cluster_indices);
    for (const size_t cluster_index : cluster_indices) {
      cluster_ids->at(cluster_index) = cluster_id;
    }
  }
}

bool BuildAssemblyPlanForComponent(
    const std::vector<AutoCloseGraphEdge> &edges,
    const std::unordered_map<int, std::vector<int>> &adjacent_edge_ids,
    const std::unordered_map<int, int> &cluster_sizes,
    const std::vector<int> &component_edge_ids,
    const std::vector<int> &component_cluster_ids,
    AutoCloseAssemblyPlan *plan) {
  if (plan == nullptr || component_edge_ids.size() <= 1) {
    return false;
  }

  int degree_one_count = 0;
  for (const int cluster_id : component_cluster_ids) {
    const auto size_it = cluster_sizes.find(cluster_id);
    if (size_it == cluster_sizes.end()) {
      return false;
    }
    if (size_it->second > 2) {
      return false;
    }
    if (size_it->second == 1) {
      degree_one_count += 1;
    }
  }
  if (degree_one_count != 0 && degree_one_count != 2) {
    return false;
  }

  const bool closed_component = degree_one_count == 0;
  int current_cluster_id = -1;
  if (closed_component) {
    current_cluster_id = *std::min_element(component_cluster_ids.begin(),
                                           component_cluster_ids.end());
  } else {
    for (const int cluster_id : component_cluster_ids) {
      const auto size_it = cluster_sizes.find(cluster_id);
      if (size_it != cluster_sizes.end() && size_it->second == 1) {
        current_cluster_id = cluster_id;
        break;
      }
    }
  }
  if (current_cluster_id < 0) {
    return false;
  }
  const int start_cluster_id = current_cluster_id;

  std::unordered_set<int> unused_edge_ids(component_edge_ids.begin(),
                                          component_edge_ids.end());
  plan->ordered_path_ids.clear();
  plan->reverse_path.clear();
  plan->closed = closed_component;

  while (!unused_edge_ids.empty()) {
    const auto adjacent_it = adjacent_edge_ids.find(current_cluster_id);
    if (adjacent_it == adjacent_edge_ids.end()) {
      return false;
    }

    int next_edge_id = -1;
    for (const int edge_id : adjacent_it->second) {
      if (!unused_edge_ids.contains(edge_id)) {
        continue;
      }
      if (next_edge_id < 0 ||
          edges[edge_id].path_id < edges[next_edge_id].path_id) {
        next_edge_id = edge_id;
      }
    }
    if (next_edge_id < 0) {
      return false;
    }

    const AutoCloseGraphEdge &edge = edges[next_edge_id];
    const bool reverse_path = edge.end_cluster_id == current_cluster_id;
    const int next_cluster_id =
        reverse_path ? edge.start_cluster_id : edge.end_cluster_id;
    plan->ordered_path_ids.push_back(edge.path_id);
    plan->reverse_path.push_back(reverse_path);
    unused_edge_ids.erase(next_edge_id);
    current_cluster_id = next_cluster_id;
  }

  if (plan->ordered_path_ids.size() != component_edge_ids.size()) {
    return false;
  }
  if (closed_component && current_cluster_id != start_cluster_id) {
    return false;
  }
  return true;
}

bool ApplyGraphAutoCloseAssemblies(
    ImportedArtwork &artwork, float weld_tolerance,
    ImportedArtworkOperationResult *result,
    std::unordered_set<int> *newly_closed_path_ids,
    AutoCloseProfilingSummary *profiling_summary) {
  std::unordered_map<int, size_t> path_index_by_id;
  path_index_by_id.reserve(artwork.paths.size());
  for (size_t index = 0; index < artwork.paths.size(); ++index) {
    path_index_by_id.emplace(artwork.paths[index].id, index);
  }

  std::vector<std::vector<int>> compatible_path_groups;
  GroupCompatibleOpenPathIds(artwork, &compatible_path_groups);
  if (profiling_summary != nullptr) {
    profiling_summary->assembly_pass_count += 1;
    profiling_summary->compatible_group_count +=
        static_cast<int>(compatible_path_groups.size());
  }

  std::unordered_map<int, ImportedPath> replacement_paths;
  std::unordered_set<int> removed_path_ids;
  bool merged_any = false;

  for (const std::vector<int> &path_group : compatible_path_groups) {
    if (path_group.size() <= 1) {
      continue;
    }

    std::vector<OpenPathEndpoint> endpoints;
    CollectOpenPathEndpoints(artwork, path_group, path_index_by_id, &endpoints);
    if (profiling_summary != nullptr) {
      profiling_summary->endpoint_count += static_cast<int>(endpoints.size());
    }
    if (endpoints.size() <= 2) {
      continue;
    }

    std::vector<int> endpoint_cluster_ids;
    std::vector<std::vector<size_t>> endpoint_clusters;
    ComputeEndpointClusters(endpoints, weld_tolerance, &endpoint_cluster_ids,
                            &endpoint_clusters);
    if (profiling_summary != nullptr) {
      profiling_summary->cluster_count +=
          static_cast<int>(endpoint_clusters.size());
    }

    std::unordered_map<int, int> cluster_sizes;
    cluster_sizes.reserve(endpoint_clusters.size());
    std::unordered_map<int, int> start_cluster_by_path_id;
    std::unordered_map<int, int> end_cluster_by_path_id;
    for (size_t endpoint_index = 0; endpoint_index < endpoints.size();
         ++endpoint_index) {
      const int cluster_id = endpoint_cluster_ids[endpoint_index];
      cluster_sizes[cluster_id] =
          static_cast<int>(endpoint_clusters[cluster_id].size());
      const ImportedPathEndpointKey &key = endpoints[endpoint_index].key;
      if (key.endpoint == ImportedPathEndpointKind::Start) {
        start_cluster_by_path_id[key.path_id] = cluster_id;
      } else {
        end_cluster_by_path_id[key.path_id] = cluster_id;
      }
    }

    std::vector<AutoCloseGraphEdge> edges;
    edges.reserve(path_group.size());
    for (const int path_id : path_group) {
      const auto start_it = start_cluster_by_path_id.find(path_id);
      const auto end_it = end_cluster_by_path_id.find(path_id);
      if (start_it == start_cluster_by_path_id.end() ||
          end_it == end_cluster_by_path_id.end()) {
        continue;
      }
      edges.push_back({path_id, start_it->second, end_it->second});
    }
    if (edges.size() <= 1) {
      continue;
    }

    std::unordered_map<int, std::vector<int>> adjacent_edge_ids;
    adjacent_edge_ids.reserve(endpoint_clusters.size());
    for (size_t edge_index = 0; edge_index < edges.size(); ++edge_index) {
      adjacent_edge_ids[edges[edge_index].start_cluster_id].push_back(
          static_cast<int>(edge_index));
      adjacent_edge_ids[edges[edge_index].end_cluster_id].push_back(
          static_cast<int>(edge_index));
    }
    for (auto &[cluster_id, edge_ids] : adjacent_edge_ids) {
      std::sort(edge_ids.begin(), edge_ids.end(),
                [&edges](int left, int right) {
                  return edges[left].path_id < edges[right].path_id;
                });
    }

    std::vector<bool> edge_visited(edges.size(), false);
    for (size_t edge_index = 0; edge_index < edges.size(); ++edge_index) {
      if (edge_visited[edge_index]) {
        continue;
      }
      if (profiling_summary != nullptr) {
        profiling_summary->component_count += 1;
      }

      std::vector<int> component_edge_ids;
      std::vector<int> component_cluster_ids;
      std::unordered_set<int> component_cluster_set;
      std::vector<int> cluster_stack = {edges[edge_index].start_cluster_id,
                                        edges[edge_index].end_cluster_id};

      while (!cluster_stack.empty()) {
        const int cluster_id = cluster_stack.back();
        cluster_stack.pop_back();
        if (!component_cluster_set.insert(cluster_id).second) {
          continue;
        }
        component_cluster_ids.push_back(cluster_id);

        const auto adjacent_it = adjacent_edge_ids.find(cluster_id);
        if (adjacent_it == adjacent_edge_ids.end()) {
          continue;
        }
        for (const int adjacent_edge_id : adjacent_it->second) {
          if (!edge_visited[adjacent_edge_id]) {
            edge_visited[adjacent_edge_id] = true;
            component_edge_ids.push_back(adjacent_edge_id);
          }

          const AutoCloseGraphEdge &adjacent_edge = edges[adjacent_edge_id];
          if (!component_cluster_set.contains(adjacent_edge.start_cluster_id)) {
            cluster_stack.push_back(adjacent_edge.start_cluster_id);
          }
          if (!component_cluster_set.contains(adjacent_edge.end_cluster_id)) {
            cluster_stack.push_back(adjacent_edge.end_cluster_id);
          }
        }
      }

      AutoCloseAssemblyPlan plan;
      if (!BuildAssemblyPlanForComponent(edges, adjacent_edge_ids,
                                         cluster_sizes, component_edge_ids,
                                         component_cluster_ids, &plan)) {
        continue;
      }

      const auto base_path_it =
          path_index_by_id.find(plan.ordered_path_ids.front());
      if (base_path_it == path_index_by_id.end()) {
        continue;
      }

      ImportedPath merged_path = artwork.paths[base_path_it->second];
      if (plan.reverse_path.front()) {
        merged_path = ReverseImportedPathCopy(merged_path);
      }
      uint32_t merged_issue_flags = merged_path.issue_flags;
      std::vector<ImportedSourceReference> merged_provenance =
          merged_path.provenance;
      for (size_t plan_index = 1; plan_index < plan.ordered_path_ids.size();
           ++plan_index) {
        const auto next_path_it =
            path_index_by_id.find(plan.ordered_path_ids[plan_index]);
        if (next_path_it == path_index_by_id.end()) {
          merged_path.segments.clear();
          break;
        }

        ImportedPath next_path = artwork.paths[next_path_it->second];
        if (plan.reverse_path[plan_index]) {
          next_path = ReverseImportedPathCopy(next_path);
        }
        if (merged_path.segments.empty() || next_path.segments.empty()) {
          merged_path.segments.clear();
          break;
        }

        if (!AlignMergeJoinPoint(&merged_path, &next_path, weld_tolerance)) {
          merged_path.segments.clear();
          break;
        }
        merged_path.segments.insert(merged_path.segments.end(),
                                    next_path.segments.begin(),
                                    next_path.segments.end());
        merged_provenance.insert(merged_provenance.end(),
                                 next_path.provenance.begin(),
                                 next_path.provenance.end());
        merged_issue_flags |= next_path.issue_flags;
      }
      if (merged_path.segments.empty()) {
        continue;
      }

      merged_path.issue_flags = merged_issue_flags;
      merged_path.provenance = std::move(merged_provenance);
      NormalizeImportedSourceReferences(&merged_path.provenance);
      merged_path.closed = plan.closed;
      if (plan.closed && AlignClosureJoinPoint(&merged_path, weld_tolerance)) {
        newly_closed_path_ids->insert(merged_path.id);
      } else if (plan.closed) {
        continue;
      }

      replacement_paths[merged_path.id] = std::move(merged_path);
      for (size_t plan_index = 1; plan_index < plan.ordered_path_ids.size();
           ++plan_index) {
        removed_path_ids.insert(plan.ordered_path_ids[plan_index]);
      }
      result->stitched_count +=
          static_cast<int>(plan.ordered_path_ids.size()) - 1;
      merged_any = true;
    }
  }

  if (!merged_any) {
    return false;
  }

  for (ImportedPath &path : artwork.paths) {
    const auto replacement_it = replacement_paths.find(path.id);
    if (replacement_it != replacement_paths.end()) {
      path = replacement_it->second;
    }
  }
  std::erase_if(artwork.paths, [&removed_path_ids](const ImportedPath &path) {
    return removed_path_ids.contains(path.id);
  });
  for (ImportedGroup &group : artwork.groups) {
    std::erase_if(group.path_ids, [&removed_path_ids](int path_id) {
      return removed_path_ids.contains(path_id);
    });
  }
  return true;
}

ImportedPath BuildImportedPolylinePath(const ImportedPath &source,
                                       const std::vector<ImVec2> &points,
                                       bool closed) {
  ImportedPath polyline_path = source;
  polyline_path.closed = closed;
  polyline_path.segments.clear();
  if (points.size() < 2) {
    return polyline_path;
  }

  const size_t segment_count = closed ? points.size() : points.size() - 1;
  polyline_path.segments.reserve(segment_count);
  for (size_t index = 0; index < points.size() - 1; ++index) {
    ImportedPathSegment segment;
    segment.kind = ImportedPathSegmentKind::Line;
    segment.start = points[index];
    segment.end = points[index + 1];
    polyline_path.segments.push_back(segment);
  }
  if (closed) {
    ImportedPathSegment segment;
    segment.kind = ImportedPathSegmentKind::Line;
    segment.start = points.back();
    segment.end = points.front();
    polyline_path.segments.push_back(segment);
  }

  return polyline_path;
}

void ConvertImportedPathToPolylineInPlace(ImportedPath *path) {
  if (path == nullptr || path->segments.empty()) {
    return;
  }

  std::vector<ImVec2> sampled_points;
  detail::AppendSampledSegmentPointsLocal(path->segments, &sampled_points);
  if (sampled_points.size() <= 1) {
    return;
  }

  if (path->closed && detail::PointsNear(sampled_points.front(),
                                         sampled_points.back(), 0.0001f)) {
    sampled_points.pop_back();
  }

  *path = BuildImportedPolylinePath(*path, sampled_points, path->closed);
}

void LegacyCloseAndMergeOpenPathsInPlace(
    ImportedArtwork &artwork, float weld_tolerance,
    ImportedArtworkOperationResult *result,
    std::unordered_set<int> *newly_closed_path_ids) {
  for (ImportedPath &path : artwork.paths) {
    if (path.segments.empty() || path.closed) {
      continue;
    }
    if (detail::PointsNear(path.segments.front().start,
                           path.segments.back().end, weld_tolerance)) {
      if (AlignClosureJoinPoint(&path, weld_tolerance)) {
        path.closed = true;
        newly_closed_path_ids->insert(path.id);
      }
    }
  }

  ApplyGraphAutoCloseAssemblies(artwork, weld_tolerance, result,
                                newly_closed_path_ids, nullptr);

  size_t remaining_open_path_count = 0;
  for (const ImportedPath &path : artwork.paths) {
    if (!path.closed && !path.segments.empty()) {
      remaining_open_path_count += 1;
    }
  }
  if (remaining_open_path_count > kMaxLegacyFallbackOpenPaths) {
    TraceImportedArtworkStep(
        std::string(
            "Prepare: skipping greedy fallback, remaining open paths=") +
        std::to_string(remaining_open_path_count));
    return;
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

      bool found_candidate = false;
      LegacyMergeCandidate best_candidate;
      for (size_t second_index = first_index + 1;
           second_index < artwork.paths.size(); ++second_index) {
        ImportedPath &second_path = artwork.paths[second_index];
        if (second_path.closed || second_path.segments.empty() ||
            second_path.parent_group_id != first_path.parent_group_id) {
          continue;
        }

        const std::array<ImportedPath, 2> first_variants = {
            first_path, ReverseImportedPathCopy(first_path)};
        const std::array<ImportedPath, 2> second_variants = {
            second_path, ReverseImportedPathCopy(second_path)};
        for (const ImportedPath &first_variant : first_variants) {
          for (const ImportedPath &second_variant : second_variants) {
            if (first_variant.segments.empty() ||
                second_variant.segments.empty()) {
              continue;
            }
            const ImVec2 gap =
                SubtractPoints(first_variant.segments.back().end,
                               second_variant.segments.front().start);
            const float gap_distance_squared = VectorLengthSquared(gap);
            if (gap_distance_squared > weld_tolerance * weld_tolerance) {
              continue;
            }

            ImVec2 join_point;
            if (!TryComputeJoinPoint(first_variant.segments.back(),
                                     second_variant.segments.front(),
                                     weld_tolerance, &join_point)) {
              continue;
            }

            LegacyMergeCandidate candidate;
            candidate.second_index = second_index;
            candidate.first = first_variant;
            candidate.second = second_variant;
            candidate.gap_distance_squared = gap_distance_squared;
            candidate.direction_dot = ComputeJoinDirectionDot(
                first_variant.segments.back(), second_variant.segments.front());
            candidate.quality_rank = ComputeJoinQualityRank(
                first_variant.segments.back(), second_variant.segments.front(),
                weld_tolerance);
            if (!found_candidate ||
                IsBetterLegacyMergeCandidate(candidate, best_candidate)) {
              best_candidate = std::move(candidate);
              found_candidate = true;
            }
          }
        }
      }

      if (!found_candidate) {
        continue;
      }

      ImportedPath oriented_first = std::move(best_candidate.first);
      ImportedPath oriented_second = std::move(best_candidate.second);
      if (!AlignMergeJoinPoint(&oriented_first, &oriented_second,
                               weld_tolerance)) {
        continue;
      }
      oriented_first.segments.insert(oriented_first.segments.end(),
                                     oriented_second.segments.begin(),
                                     oriented_second.segments.end());
      if (detail::PointsNear(oriented_first.segments.front().start,
                             oriented_first.segments.back().end,
                             weld_tolerance)) {
        if (AlignClosureJoinPoint(&oriented_first, weld_tolerance)) {
          oriented_first.closed = true;
          newly_closed_path_ids->insert(oriented_first.id);
        }
      }

      const int removed_path_id = artwork.paths[best_candidate.second_index].id;
      artwork.paths[first_index] = std::move(oriented_first);
      artwork.paths.erase(
          artwork.paths.begin() +
          static_cast<std::ptrdiff_t>(best_candidate.second_index));
      for (ImportedGroup &group : artwork.groups) {
        std::erase(group.path_ids, removed_path_id);
      }
      result->stitched_count += 1;
      merged_any = true;

      if (merged_any) {
        break;
      }
    }
  }
}

AutoClosePolylineStageResult AutoCloseImportedArtworkToPolylineInPlace(
    ImportedArtwork &artwork, float weld_tolerance,
    ImportedArtworkOperationResult *result) {
  AutoClosePolylineStageResult stage_result;
  AutoCloseProfilingSummary profiling_summary;
  const auto start_time = std::chrono::steady_clock::now();

  for (ImportedPath &path : artwork.paths) {
    if (path.segments.empty() || path.closed) {
      continue;
    }
    if (detail::PointsNear(path.segments.front().start,
                           path.segments.back().end, weld_tolerance)) {
      if (AlignClosureJoinPoint(&path, weld_tolerance)) {
        path.closed = true;
        stage_result.newly_closed_path_ids.insert(path.id);
      }
    }
  }

  ApplyGraphAutoCloseAssemblies(artwork, weld_tolerance, result,
                                &stage_result.newly_closed_path_ids,
                                &profiling_summary);

  for (ImportedPath &path : artwork.paths) {
    if (path.segments.empty()) {
      continue;
    }
    const bool should_polyline =
        !path.closed || stage_result.newly_closed_path_ids.contains(path.id);
    if (!should_polyline) {
      continue;
    }
    ConvertImportedPathToPolylineInPlace(&path);
  }

  const auto end_time = std::chrono::steady_clock::now();
  profiling_summary.elapsed_ms =
      std::chrono::duration<float, std::milli>(end_time - start_time).count();
  result->auto_close_endpoint_count = profiling_summary.endpoint_count;
  result->auto_close_cluster_count = profiling_summary.cluster_count;
  result->auto_close_group_count = profiling_summary.compatible_group_count;
  result->auto_close_component_count = profiling_summary.component_count;
  result->auto_close_pass_count = profiling_summary.assembly_pass_count;
  result->auto_close_elapsed_ms = profiling_summary.elapsed_ms;

  return stage_result;
}

} // namespace detail

using namespace detail;

ImportedArtworkOperationResult
AutoCloseImportedArtworkToPolyline(CanvasState &state, int imported_artwork_id,
                                   float weld_tolerance) {
  ImportedArtworkOperationResult result;
  result.artwork_id = imported_artwork_id;

  ImportedArtwork *artwork = FindImportedArtwork(state, imported_artwork_id);
  if (artwork == nullptr) {
    result.message = "Imported artwork was not found.";
    SetLastImportedOperationIssueElements(state, 0, {});
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  PushUndoSnapshot(state, "Auto close imported artwork");
  SetLastImportedOperationIssueElements(state, 0, {});
  ClearImportedArtworkPreviewStatesForArtwork(state, imported_artwork_id);
  AutoCloseImportedArtworkToPolylineInPlace(*artwork, weld_tolerance, &result);

  RecomputeImportedArtworkBounds(*artwork);
  RecomputeImportedHierarchyBounds(*artwork);
  RefreshImportedArtworkPartMetadata(*artwork);
  PopulateOperationReadiness(&result, *artwork);

  std::vector<ImportedElementSelection> unresolved_elements;
  for (const ImportedPath &path : artwork->paths) {
    if (!path.segments.empty() && !path.closed) {
      unresolved_elements.push_back({ImportedElementKind::Path, path.id});
    }
  }

  result.success = true;
  result.message =
      "Auto Close To Polyline: stitched " +
      std::to_string(result.stitched_count) + ", closed " +
      std::to_string(result.closed_count) + ", open " +
      std::to_string(result.open_count) + ". Profile: endpoints=" +
      std::to_string(result.auto_close_endpoint_count) +
      ", clusters=" + std::to_string(result.auto_close_cluster_count) +
      ", groups=" + std::to_string(result.auto_close_group_count) +
      ", components=" + std::to_string(result.auto_close_component_count) +
      ", passes=" + std::to_string(result.auto_close_pass_count) +
      ", ms=" + std::to_string(result.auto_close_elapsed_ms) + ".";
  SetLastImportedOperationIssueElements(state, artwork->id,
                                        std::move(unresolved_elements));
  SetLastImportedArtworkOperation(state, result);
  return result;
}

ImportedArtworkOperationResult
JoinImportedArtworkOpenSegments(CanvasState &state, int imported_artwork_id,
                                float weld_tolerance) {
  ImportedArtworkOperationResult result = AutoCloseImportedArtworkToPolyline(
      state, imported_artwork_id, weld_tolerance);
  if (!result.success) {
    return result;
  }

  result.message = "Join Open Segments: stitched " +
                   std::to_string(result.stitched_count) + ", closed " +
                   std::to_string(result.closed_count) + ", remaining open " +
                   std::to_string(result.open_count) + ".";
  SetLastImportedArtworkOperation(state, result);
  return result;
}

ImportedArtworkOperationResult
AnalyzeImportedArtworkContours(CanvasState &state, int imported_artwork_id) {
  ImportedArtworkOperationResult result;
  result.artwork_id = imported_artwork_id;

  ImportedArtwork *artwork = FindImportedArtwork(state, imported_artwork_id);
  if (artwork == nullptr) {
    result.message = "Imported artwork was not found.";
    SetLastImportedOperationIssueElements(state, 0, {});
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  RefreshImportedArtworkPartMetadata(*artwork);
  PopulateOperationReadiness(&result, *artwork);

  std::vector<ImportedElementSelection> issue_elements;
  CollectImportedIssueElements(*artwork, &issue_elements);
  SetLastImportedOperationIssueElements(state, artwork->id,
                                        std::move(issue_elements), false);

  result.success = true;
  result.message = "Analyze Contours: open " +
                   std::to_string(result.open_count) + ", outer " +
                   std::to_string(result.outer_count) + ", holes " +
                   std::to_string(result.hole_count) + ", orphan holes " +
                   std::to_string(result.orphan_hole_count) + ".";
  SetLastImportedArtworkOperation(state, result);
  ShowCanvasNotification(state, "Contour Analysis Ready", result.message,
                         CanvasNotificationDismissMode::UserClosable);
  return result;
}

ImportedArtworkOperationResult
RepairImportedArtworkOrphanHoles(CanvasState &state, int imported_artwork_id) {
  ImportedArtworkOperationResult result;
  result.artwork_id = imported_artwork_id;

  ImportedArtwork *artwork = FindImportedArtwork(state, imported_artwork_id);
  if (artwork == nullptr) {
    result.message = "Imported artwork was not found.";
    SetLastImportedOperationIssueElements(state, 0, {});
    SetLastImportedArtworkOperation(state, result);
    return result;
  }

  PushUndoSnapshot(state, "Repair imported artwork orphan holes");
  ClearImportedArtworkPreviewStatesForArtwork(state, imported_artwork_id);
  RefreshImportedArtworkPartMetadata(*artwork);

  int repaired_hole_count = 0;
  for (const ImportedContourReference &hole_ref : artwork->part.orphan_holes) {
    if (hole_ref.kind == ImportedElementKind::Path) {
      ImportedPath *path = FindImportedPath(*artwork, hole_ref.item_id);
      if (path == nullptr || !path->closed || path->segments.empty()) {
        continue;
      }

      path->flags &= ~static_cast<uint32_t>(ImportedPathFlagHoleContour);
      if (ImportedPathSignedArea(*path) < 0.0) {
        *path = ReverseImportedPathCopy(*path);
      }
      repaired_hole_count += 1;
      continue;
    }

    ImportedDxfText *text = FindImportedDxfText(*artwork, hole_ref.item_id);
    if (text == nullptr) {
      continue;
    }
    ImportedTextContour *contour =
        FindImportedTextContourByIndex(*text, hole_ref.contour_index);
    if (contour == nullptr) {
      continue;
    }
    contour->role = ImportedTextContourRole::Outline;
    repaired_hole_count += 1;
  }

  RecomputeImportedArtworkBounds(*artwork);
  RecomputeImportedHierarchyBounds(*artwork);
  RefreshImportedArtworkPartMetadata(*artwork);
  PopulateOperationReadiness(&result, *artwork);
  result.repaired_hole_count = repaired_hole_count;

  std::vector<ImportedElementSelection> issue_elements;
  CollectImportedIssueElements(*artwork, &issue_elements);
  SetLastImportedOperationIssueElements(state, artwork->id,
                                        std::move(issue_elements));

  result.success = repaired_hole_count > 0 || result.orphan_hole_count == 0;
  result.message = "Repair Orphan Holes: reclassified " +
                   std::to_string(result.repaired_hole_count) +
                   " orphan contours, remaining orphan holes " +
                   std::to_string(result.orphan_hole_count) + ".";
  SetLastImportedArtworkOperation(state, result);
  return result;
}

ImportedArtworkOperationResult PrepareImportedArtworkForCutting(
    CanvasState &state, int imported_artwork_id, float weld_tolerance,
    ImportedArtworkPrepareMode mode, bool auto_close_to_polyline) {
  if (FindImportedArtwork(state, imported_artwork_id) != nullptr) {
    PushUndoSnapshot(state, "Prepare imported artwork for cutting");
    ClearImportedArtworkPreviewStatesForArtwork(state, imported_artwork_id);
  }

  ImportedArtworkOperationResult result =
      operations::detail::PrepareImportedArtworkForCuttingShared(
          state, imported_artwork_id, weld_tolerance,
          [mode, auto_close_to_polyline](
              ImportedArtwork &artwork, float callback_weld_tolerance,
              ImportedArtworkOperationResult *result) {
            result->prepare_mode = mode;
            std::unordered_set<int> newly_closed_path_ids;
            TraceImportedArtworkStep(
                std::string("Prepare: open-path recovery start, paths=") +
                std::to_string(artwork.paths.size()));
            if (auto_close_to_polyline) {
              AutoClosePolylineStageResult auto_close_stage =
                  AutoCloseImportedArtworkToPolylineInPlace(
                      artwork, callback_weld_tolerance, result);
              newly_closed_path_ids =
                  std::move(auto_close_stage.newly_closed_path_ids);
              LegacyCloseAndMergeOpenPathsInPlace(
                  artwork, callback_weld_tolerance, result,
                  &newly_closed_path_ids);
            } else {
              LegacyCloseAndMergeOpenPathsInPlace(
                  artwork, callback_weld_tolerance, result,
                  &newly_closed_path_ids);
            }
            TraceImportedArtworkStep(
                std::string("Prepare: open-path recovery complete, paths=") +
                std::to_string(artwork.paths.size()));

            std::vector<ClosedCleanupGroup> cleanup_groups;
            TraceImportedArtworkStep("Prepare: collect cleanup groups start");
            for (const ImportedPath &path : artwork.paths) {
              if (!path.closed || path.segments.empty() ||
                  !SupportsClipperCleanup(path)) {
                continue;
              }

              const bool should_clean =
                  mode == ImportedArtworkPrepareMode::AggressiveCleanup ||
                  newly_closed_path_ids.contains(path.id);
              if (!should_clean) {
                result->preserved_count += 1;
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
              group->requires_clipper_cleanup = true;
            }
            TraceImportedArtworkStep(
                std::string(
                    "Prepare: collect cleanup groups complete, groups=") +
                std::to_string(cleanup_groups.size()));

            std::unordered_set<int> replaced_path_ids;
            std::vector<ImportedPath> cleaned_paths;
            TraceImportedArtworkStep("Prepare: Clipper cleanup start");
            for (ClosedCleanupGroup &group : cleanup_groups) {
              if (mode == ImportedArtworkPrepareMode::FidelityFirst &&
                  !group.requires_clipper_cleanup) {
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
                      inflated,
                      static_cast<double>(-callback_weld_tolerance) * 0.5,
                      Clipper2Lib::JoinType::Round,
                      Clipper2Lib::EndType::Polygon, 2.0,
                      kClipperDecimalPrecision);
                  if (!deflated.empty()) {
                    prepared_paths = std::move(deflated);
                  }
                }
              }

              Clipper2Lib::PathsD unified_paths = Clipper2Lib::Union(
                  prepared_paths, Clipper2Lib::FillRule::NonZero,
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
                  ambiguous_cleanup
                      ? static_cast<uint32_t>(
                            ImportedElementIssueFlagAmbiguousCleanup)
                      : ImportedElementIssueFlagNone;
              for (const Clipper2Lib::PathD &unified_path : unified_paths) {
                if (unified_path.size() < 3 ||
                    std::fabs(Clipper2Lib::Area(unified_path)) <
                        kMinimumPreparedContourArea) {
                  continue;
                }

                group_cleaned_paths.push_back(
                    BuildImportedPathFromClipperContour(
                        unified_path, group.template_path,
                        artwork.next_path_id++, group.provenance,
                        output_issue_flags));
              }

              if (group_cleaned_paths.empty()) {
                continue;
              }

              for (int source_path_id : group.source_path_ids) {
                replaced_path_ids.insert(source_path_id);
              }
              result->cleaned_count +=
                  static_cast<int>(group_cleaned_paths.size());
              if (ambiguous_cleanup) {
                result->ambiguous_count +=
                    static_cast<int>(group_cleaned_paths.size());
              }
              std::move(group_cleaned_paths.begin(), group_cleaned_paths.end(),
                        std::back_inserter(cleaned_paths));
            }
            TraceImportedArtworkStep(
                std::string("Prepare: Clipper cleanup complete, cleaned=") +
                std::to_string(cleaned_paths.size()));

            if (!replaced_path_ids.empty()) {
              std::erase_if(artwork.paths,
                            [&replaced_path_ids](const ImportedPath &path) {
                              return replaced_path_ids.contains(path.id);
                            });

              for (ImportedGroup &group : artwork.groups) {
                std::erase_if(group.path_ids,
                              [&replaced_path_ids](int path_id) {
                                return replaced_path_ids.contains(path_id);
                              });
              }

              for (ImportedPath &cleaned_path : cleaned_paths) {
                if (ImportedGroup *group = FindImportedGroup(
                        artwork, cleaned_path.parent_group_id);
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
  if (result.success && auto_close_to_polyline) {
    result.message += " Auto-close to polyline ran before cleanup.";
    SetLastImportedArtworkOperation(state, result);
  }
  return result;
}

} // namespace im2d
