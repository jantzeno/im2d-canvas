#include "im2d_nesting_postprocess.h"

#include "im2d_nesting_geometry.h"

#include <vector>

namespace im2d::nesting {

namespace {

double SegmentLengthSquared(const ToolpathSegment &segment) {
  return LengthSquared(PointD{.x = segment.end.x - segment.start.x,
                              .y = segment.end.y - segment.start.y});
}

bool IsFullyCoveredBy(const ToolpathSegment &candidate,
                      const ToolpathSegment &covering, double epsilon) {
  return IsPointOnSegment(candidate.start, covering.start, covering.end,
                          epsilon) &&
         IsPointOnSegment(candidate.end, covering.start, covering.end, epsilon);
}

bool AreCoincidentSegments(const ToolpathSegment &left,
                           const ToolpathSegment &right, double epsilon) {
  if (!IsPointOnSegment(left.start, right.start, right.end, epsilon) ||
      !IsPointOnSegment(left.end, right.start, right.end, epsilon)) {
    return false;
  }
  return IsPointOnSegment(right.start, left.start, left.end, epsilon) &&
         IsPointOnSegment(right.end, left.start, left.end, epsilon);
}

} // namespace

std::vector<ToolpathSegment>
ExtractToolpathSegments(const std::vector<Contour> &contours) {
  std::vector<ToolpathSegment> segments;
  for (size_t contour_index = 0; contour_index < contours.size();
       ++contour_index) {
    const Contour &contour = contours[contour_index];
    if (contour.size() < 2) {
      continue;
    }

    for (size_t edge_index = 0; edge_index < contour.size(); ++edge_index) {
      const PointD &start = contour[edge_index];
      const PointD &end = contour[(edge_index + 1) % contour.size()];
      if (LengthSquared(PointD{.x = end.x - start.x, .y = end.y - start.y}) <=
          1e-18) {
        continue;
      }

      segments.push_back(ToolpathSegment{
          .start = start,
          .end = end,
          .source_contour_index = static_cast<int>(contour_index),
          .source_edge_index = static_cast<int>(edge_index)});
    }
  }

  return segments;
}

std::vector<ToolpathSegment> EliminateFullyCoveredCollinearSegments(
    const std::vector<ToolpathSegment> &segments, double epsilon) {
  std::vector<bool> removed(segments.size(), false);

  for (size_t index = 0; index < segments.size(); ++index) {
    if (removed[index]) {
      continue;
    }

    const double candidate_length = SegmentLengthSquared(segments[index]);
    for (size_t other_index = 0; other_index < segments.size(); ++other_index) {
      if (index == other_index || removed[index]) {
        continue;
      }

      const ToolpathSegment &candidate = segments[index];
      const ToolpathSegment &other = segments[other_index];
      const bool candidate_on_other =
          IsFullyCoveredBy(candidate, other, epsilon);
      if (!candidate_on_other) {
        continue;
      }

      const bool other_on_candidate =
          IsFullyCoveredBy(other, candidate, epsilon);
      if (other_on_candidate) {
        if (AreCoincidentSegments(candidate, other, epsilon) &&
            other_index < index) {
          removed[index] = true;
        }
        continue;
      }

      const double other_length = SegmentLengthSquared(other);
      if (other_length > candidate_length + epsilon) {
        removed[index] = true;
      }
    }
  }

  std::vector<ToolpathSegment> filtered;
  filtered.reserve(segments.size());
  for (size_t index = 0; index < segments.size(); ++index) {
    if (!removed[index]) {
      filtered.push_back(segments[index]);
    }
  }
  return filtered;
}

} // namespace im2d::nesting