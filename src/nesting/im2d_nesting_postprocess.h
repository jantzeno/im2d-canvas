#pragma once

#include "im2d_nesting_types.h"

#include <vector>

namespace im2d::nesting {

struct ToolpathSegment {
  PointD start;
  PointD end;
  int source_contour_index = -1;
  int source_edge_index = -1;
};

std::vector<ToolpathSegment>
ExtractToolpathSegments(const std::vector<Contour> &contours);
std::vector<ToolpathSegment> EliminateFullyCoveredCollinearSegments(
    const std::vector<ToolpathSegment> &segments, double epsilon = 1e-9);

} // namespace im2d::nesting