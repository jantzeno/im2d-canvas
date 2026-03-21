#pragma once

#include "im2d_canvas_types.h"

namespace im2d {

SnapResult SnapAxisCoordinate(const CanvasState &state,
                              GuideOrientation orientation, float value,
                              int ignored_guide_id = 0);
ImVec2 SnapPoint(const CanvasState &state, const ImVec2 &point,
                 int ignored_vertical_guide_id = 0,
                 int ignored_horizontal_guide_id = 0);

} // namespace im2d