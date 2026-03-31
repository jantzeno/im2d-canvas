#include "im2d_canvas_snap.h"

#include "im2d_canvas_document.h"
#include "im2d_canvas_units.h"

#include <cmath>

namespace im2d {

namespace {

void ConsiderCandidate(const CanvasState &state, float raw_value,
                       float candidate_value, SnapTargetKind target,
                       int guide_id, SnapResult &result) {
  const float distance_screen =
      std::fabs(candidate_value - raw_value) * state.view.zoom;
  if (distance_screen > state.snapping.screen_threshold) {
    return;
  }

  const float current_best =
      result.snapped ? std::fabs(result.value - raw_value) * state.view.zoom
                     : state.snapping.screen_threshold + 1.0f;
  if (!result.snapped || distance_screen < current_best) {
    result.value = candidate_value;
    result.snapped = true;
    result.target = target;
    result.guide_id = guide_id;
  }
}

} // namespace

SnapResult SnapAxisCoordinate(const CanvasState &state,
                              GuideOrientation orientation, float value,
                              int ignored_guide_id) {
  SnapResult result;
  result.value = value;

  if (state.snapping.to_guides) {
    for (const Guide &guide : state.guides) {
      if (guide.id == ignored_guide_id || guide.orientation != orientation) {
        continue;
      }
      ConsiderCandidate(state, value, guide.position, SnapTargetKind::Guide,
                        guide.id, result);
    }
  }

  const float major_spacing =
      UnitsToPixels(state.grid.spacing, state.grid.unit, state.calibration);
  if (state.snapping.to_grid_major && major_spacing > 0.0f) {
    const float snapped = std::round(value / major_spacing) * major_spacing;
    ConsiderCandidate(state, value, snapped, SnapTargetKind::GridMajor, 0,
                      result);
  }

  if (state.snapping.to_grid_minor && state.grid.subdivisions > 0 &&
      major_spacing > 0.0f) {
    const float minor_spacing = major_spacing / state.grid.subdivisions;
    const float snapped = std::round(value / minor_spacing) * minor_spacing;
    ConsiderCandidate(state, value, snapped, SnapTargetKind::GridMinor, 0,
                      result);
  }

  if (state.snapping.to_margins) {
    for (const ExportArea &area : state.export_areas) {
      if (!area.visible) {
        continue;
      }

      if (orientation == GuideOrientation::Vertical) {
        ConsiderCandidate(state, value, area.origin.x, SnapTargetKind::Margin, 0,
                          result);
        ConsiderCandidate(state, value, area.origin.x + area.size.x,
                          SnapTargetKind::Margin, 0, result);
      } else {
        ConsiderCandidate(state, value, area.origin.y, SnapTargetKind::Margin, 0,
                          result);
        ConsiderCandidate(state, value, area.origin.y + area.size.y,
                          SnapTargetKind::Margin, 0, result);
      }
    }
  }

  return result;
}

ImVec2 SnapPoint(const CanvasState &state, const ImVec2 &point,
                 int ignored_vertical_guide_id, int ignored_horizontal_guide_id) {
  const SnapResult snapped_x =
      SnapAxisCoordinate(state, GuideOrientation::Vertical, point.x,
                         ignored_vertical_guide_id);
  const SnapResult snapped_y =
      SnapAxisCoordinate(state, GuideOrientation::Horizontal, point.y,
                         ignored_horizontal_guide_id);
  return ImVec2(snapped_x.value, snapped_y.value);
}

} // namespace im2d
