#include "im2d_canvas_widget.h"

#include "im2d_canvas_document.h"
#include "im2d_canvas_imported_artwork_ops.h"
#include "im2d_canvas_snap.h"
#include "im2d_canvas_units.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include <imgui_internal.h>

namespace im2d {

namespace {

enum class WorkingAreaHitZone {
  None,
  Body,
  ResizeHandle,
};

enum class ImportedArtworkHitZone {
  None,
  Body,
  ResizeHandle,
};

struct WorkingAreaHit {
  int id = 0;
  WorkingAreaHitZone zone = WorkingAreaHitZone::None;
};

struct ImportedArtworkHit {
  int id = 0;
  ImportedArtworkHitZone zone = ImportedArtworkHitZone::None;
};

struct TransientCanvasState {
  bool creating_guide = false;
  bool marquee_selecting = false;
  GuideOrientation pending_orientation = GuideOrientation::Vertical;
  float pending_position = 0.0f;
  int dragging_guide_id = 0;
  int context_guide_id = 0;
  int context_imported_artwork_id = 0;
  int dragging_imported_artwork_id = 0;
  int resizing_imported_artwork_id = 0;
  ImVec2 imported_artwork_resize_initial_scale = ImVec2(1.0f, 1.0f);
  int selected_working_area_id = 0;
  int dragging_working_area_id = 0;
  int resizing_working_area_id = 0;
  ImVec2 imported_artwork_drag_offset = ImVec2(0.0f, 0.0f);
  ImVec2 working_area_drag_offset = ImVec2(0.0f, 0.0f);
  ImVec2 marquee_start_world = ImVec2(0.0f, 0.0f);
  ImVec2 marquee_end_world = ImVec2(0.0f, 0.0f);
};

TransientCanvasState &GetTransientCanvasState() {
  static TransientCanvasState state;
  return state;
}

float ClampZoom(float zoom) { return std::clamp(zoom, 0.1f, 8.0f); }

ImVec2 WorldToScreen(const CanvasState &state, const ImVec2 &canvas_min,
                     const ImVec2 &point) {
  return ImVec2(canvas_min.x + state.view.pan.x + point.x * state.view.zoom,
                canvas_min.y + state.view.pan.y + point.y * state.view.zoom);
}

ImVec2 ScreenToWorld(const CanvasState &state, const ImVec2 &canvas_min,
                     const ImVec2 &point) {
  return ImVec2((point.x - canvas_min.x - state.view.pan.x) / state.view.zoom,
                (point.y - canvas_min.y - state.view.pan.y) / state.view.zoom);
}

ImRect WorkingAreaScreenRect(const CanvasState &state, const ImVec2 &canvas_min,
                             const WorkingArea &area) {
  return ImRect(WorldToScreen(state, canvas_min, area.origin),
                WorldToScreen(state, canvas_min,
                              ImVec2(area.origin.x + area.size.x,
                                     area.origin.y + area.size.y)));
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

ImRect ImportedArtworkScreenRect(const CanvasState &state,
                                 const ImVec2 &canvas_min,
                                 const ImportedArtwork &artwork) {
  const ImVec2 size = ImportedArtworkScaledSize(artwork);
  return ImRect(WorldToScreen(state, canvas_min, artwork.origin),
                WorldToScreen(state, canvas_min,
                              ImVec2(artwork.origin.x + size.x,
                                     artwork.origin.y + size.y)));
}

ImRect WorldRectToScreenRect(const CanvasState &state, const ImVec2 &canvas_min,
                             const ImRect &world_rect) {
  const ImVec2 min = WorldToScreen(state, canvas_min, world_rect.Min);
  const ImVec2 max = WorldToScreen(state, canvas_min, world_rect.Max);
  return ImRect(ImVec2(std::min(min.x, max.x), std::min(min.y, max.y)),
                ImVec2(std::max(min.x, max.x), std::max(min.y, max.y)));
}

ImRect ImportedElementScreenRect(const CanvasState &state,
                                 const ImVec2 &canvas_min,
                                 const ImportedArtwork &artwork,
                                 const ImVec2 &bounds_min,
                                 const ImVec2 &bounds_max) {
  ImVec2 world_min;
  ImVec2 world_max;
  ImportedLocalBoundsToWorldBounds(artwork, bounds_min, bounds_max, &world_min,
                                   &world_max);
  return WorldRectToScreenRect(state, canvas_min, ImRect(world_min, world_max));
}

ImU32 ImportedIssueOverlayColor(uint32_t issue_flags) {
  if (HasImportedElementIssueFlag(issue_flags,
                                  ImportedElementIssueFlagAmbiguousCleanup)) {
    return IM_COL32(62, 176, 166, 255);
  }
  if (HasImportedElementIssueFlag(issue_flags,
                                  ImportedElementIssueFlagOrphanHole)) {
    return IM_COL32(232, 84, 62, 255);
  }
  if (HasImportedElementIssueFlag(issue_flags,
                                  ImportedElementIssueFlagPlaceholderText)) {
    return IM_COL32(232, 171, 62, 255);
  }
  return IM_COL32(240, 146, 61, 255);
}

bool TryGetImportedDebugScreenRect(const CanvasState &state,
                                   const ImVec2 &canvas_min,
                                   const ImportedArtwork &artwork,
                                   ImRect *screen_rect) {
  if (state.selected_imported_debug.artwork_id != artwork.id) {
    return false;
  }

  switch (state.selected_imported_debug.kind) {
  case ImportedDebugSelectionKind::Artwork:
    *screen_rect = ImportedArtworkScreenRect(state, canvas_min, artwork);
    return true;
  case ImportedDebugSelectionKind::Group: {
    const ImportedGroup *group =
        FindImportedGroup(artwork, state.selected_imported_debug.item_id);
    if (group == nullptr) {
      return false;
    }
    ImVec2 world_min;
    ImVec2 world_max;
    ImportedLocalBoundsToWorldBounds(artwork, group->bounds_min,
                                     group->bounds_max, &world_min, &world_max);
    *screen_rect =
        WorldRectToScreenRect(state, canvas_min, ImRect(world_min, world_max));
    return true;
  }
  case ImportedDebugSelectionKind::Path: {
    const ImportedPath *path =
        FindImportedPath(artwork, state.selected_imported_debug.item_id);
    if (path == nullptr) {
      return false;
    }
    ImVec2 world_min;
    ImVec2 world_max;
    ImportedLocalBoundsToWorldBounds(artwork, path->bounds_min,
                                     path->bounds_max, &world_min, &world_max);
    *screen_rect =
        WorldRectToScreenRect(state, canvas_min, ImRect(world_min, world_max));
    return true;
  }
  case ImportedDebugSelectionKind::DxfText: {
    const ImportedDxfText *text =
        FindImportedDxfText(artwork, state.selected_imported_debug.item_id);
    if (text == nullptr) {
      return false;
    }
    ImVec2 world_min;
    ImVec2 world_max;
    ImportedLocalBoundsToWorldBounds(artwork, text->bounds_min,
                                     text->bounds_max, &world_min, &world_max);
    *screen_rect =
        WorldRectToScreenRect(state, canvas_min, ImRect(world_min, world_max));
    return true;
  }
  case ImportedDebugSelectionKind::None:
    break;
  }

  return false;
}

float NiceStep(float raw_value) {
  if (raw_value <= 0.0f) {
    return 1.0f;
  }

  const float exponent = std::floor(std::log10(raw_value));
  const float base = std::pow(10.0f, exponent);
  const float fraction = raw_value / base;

  float nice_fraction = 1.0f;
  if (fraction <= 1.0f) {
    nice_fraction = 1.0f;
  } else if (fraction <= 2.0f) {
    nice_fraction = 2.0f;
  } else if (fraction <= 5.0f) {
    nice_fraction = 5.0f;
  } else {
    nice_fraction = 10.0f;
  }

  return nice_fraction * base;
}

int FindHoveredGuide(const CanvasState &state, const ImVec2 &canvas_min,
                     const ImRect &canvas_rect, const ImVec2 &mouse_pos) {
  if (!canvas_rect.Contains(mouse_pos)) {
    return 0;
  }

  constexpr float kGuideHitRadius = 5.0f;
  int hovered_id = 0;
  float closest_distance = kGuideHitRadius;

  for (const Guide &guide : state.guides) {
    float distance = 0.0f;
    if (guide.orientation == GuideOrientation::Vertical) {
      const float guide_screen_x =
          WorldToScreen(state, canvas_min, ImVec2(guide.position, 0.0f)).x;
      distance = std::fabs(mouse_pos.x - guide_screen_x);
    } else {
      const float guide_screen_y =
          WorldToScreen(state, canvas_min, ImVec2(0.0f, guide.position)).y;
      distance = std::fabs(mouse_pos.y - guide_screen_y);
    }

    if (distance <= closest_distance) {
      closest_distance = distance;
      hovered_id = guide.id;
    }
  }

  return hovered_id;
}

WorkingAreaHit FindHoveredWorkingArea(const CanvasState &state,
                                      const ImVec2 &canvas_min,
                                      const ImRect &canvas_rect,
                                      const ImVec2 &mouse_pos,
                                      float resize_handle_size) {
  if (!canvas_rect.Contains(mouse_pos)) {
    return {};
  }

  for (auto it = state.working_areas.rbegin(); it != state.working_areas.rend();
       ++it) {
    const WorkingArea &area = *it;
    if (!area.visible) {
      continue;
    }

    const ImRect screen_rect = WorkingAreaScreenRect(state, canvas_min, area);
    if (!screen_rect.Contains(mouse_pos)) {
      continue;
    }

    if (HasWorkingAreaFlag(area.flags, WorkingAreaFlagResizable)) {
      const ImRect handle_rect(ImVec2(screen_rect.Max.x - resize_handle_size,
                                      screen_rect.Max.y - resize_handle_size),
                               screen_rect.Max);
      if (handle_rect.Contains(mouse_pos)) {
        return {area.id, WorkingAreaHitZone::ResizeHandle};
      }
    }

    return {area.id, WorkingAreaHitZone::Body};
  }

  return {};
}

ImportedArtworkHit FindHoveredImportedArtwork(const CanvasState &state,
                                              const ImVec2 &canvas_min,
                                              const ImRect &canvas_rect,
                                              const ImVec2 &mouse_pos,
                                              float resize_handle_size) {
  if (!canvas_rect.Contains(mouse_pos)) {
    return {};
  }

  for (auto it = state.imported_artwork.rbegin();
       it != state.imported_artwork.rend(); ++it) {
    const ImportedArtwork &artwork = *it;
    if (!artwork.visible) {
      continue;
    }

    const ImRect screen_rect =
        ImportedArtworkScreenRect(state, canvas_min, artwork);
    if (!screen_rect.Contains(mouse_pos)) {
      continue;
    }

    if (HasImportedArtworkFlag(artwork.flags, ImportedArtworkFlagResizable)) {
      const ImRect handle_rect(ImVec2(screen_rect.Max.x - resize_handle_size,
                                      screen_rect.Max.y - resize_handle_size),
                               screen_rect.Max);
      if (handle_rect.Contains(mouse_pos)) {
        return {artwork.id, ImportedArtworkHitZone::ResizeHandle};
      }
    }

    return {artwork.id, ImportedArtworkHitZone::Body};
  }

  return {};
}

bool IsLastOperationIssueElement(const CanvasState &state, int artwork_id,
                                 ImportedElementKind kind, int item_id) {
  if (state.last_imported_operation_issue_artwork_id != artwork_id) {
    return false;
  }

  return std::any_of(
      state.last_imported_operation_issue_elements.begin(),
      state.last_imported_operation_issue_elements.end(),
      [kind, item_id](const ImportedElementSelection &selection) {
        return selection.kind == kind && selection.item_id == item_id;
      });
}

void RemoveGuide(CanvasState &state, int guide_id) {
  std::erase_if(state.guides, [guide_id](const Guide &guide) {
    return guide.id == guide_id;
  });
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

void FlattenImportedTextContour(const ImportedTextContour &contour,
                                std::vector<ImVec2> *points) {
  points->clear();
  if (contour.segments.empty()) {
    return;
  }

  points->push_back(contour.segments.front().start);
  for (const ImportedPathSegment &segment : contour.segments) {
    if (segment.kind == ImportedPathSegmentKind::Line) {
      points->push_back(segment.end);
      continue;
    }

    constexpr int kCurveSamples = 12;
    for (int sample_index = 1; sample_index <= kCurveSamples; ++sample_index) {
      const float t =
          static_cast<float>(sample_index) / static_cast<float>(kCurveSamples);
      points->push_back(CubicBezierPoint(segment.start, segment.control1,
                                         segment.control2, segment.end, t));
    }
  }

  if (points->size() > 1) {
    const ImVec2 &first = points->front();
    const ImVec2 &last = points->back();
    if (std::abs(first.x - last.x) < 0.001f &&
        std::abs(first.y - last.y) < 0.001f) {
      points->pop_back();
    }
  }
}

float PolygonSignedArea(const std::vector<ImVec2> &polygon) {
  if (polygon.size() < 3) {
    return 0.0f;
  }

  float twice_area = 0.0f;
  for (size_t index = 0; index < polygon.size(); ++index) {
    const ImVec2 &current = polygon[index];
    const ImVec2 &next = polygon[(index + 1) % polygon.size()];
    twice_area += current.x * next.y - next.x * current.y;
  }
  return twice_area * 0.5f;
}

void EnsureCounterClockwise(std::vector<ImVec2> *polygon) {
  if (PolygonSignedArea(*polygon) < 0.0f) {
    std::reverse(polygon->begin(), polygon->end());
  }
}

void EnsureClockwise(std::vector<ImVec2> *polygon) {
  if (PolygonSignedArea(*polygon) > 0.0f) {
    std::reverse(polygon->begin(), polygon->end());
  }
}

bool PointInPolygon(const std::vector<ImVec2> &polygon, const ImVec2 &point) {
  bool inside = false;
  for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
    const ImVec2 &a = polygon[i];
    const ImVec2 &b = polygon[j];
    const bool intersects =
        ((a.y > point.y) != (b.y > point.y)) &&
        (point.x < (b.x - a.x) * (point.y - a.y) / (b.y - a.y + 1e-6f) + a.x);
    if (intersects) {
      inside = !inside;
    }
  }
  return inside;
}

size_t FindNearestPolygonVertex(const std::vector<ImVec2> &polygon,
                                const ImVec2 &point) {
  size_t best_index = 0;
  float best_distance_sq = std::numeric_limits<float>::max();
  for (size_t index = 0; index < polygon.size(); ++index) {
    const float dx = polygon[index].x - point.x;
    const float dy = polygon[index].y - point.y;
    const float distance_sq = dx * dx + dy * dy;
    if (distance_sq < best_distance_sq) {
      best_distance_sq = distance_sq;
      best_index = index;
    }
  }
  return best_index;
}

size_t FindRightmostVertex(const std::vector<ImVec2> &polygon) {
  size_t best_index = 0;
  for (size_t index = 1; index < polygon.size(); ++index) {
    if (polygon[index].x > polygon[best_index].x ||
        (polygon[index].x == polygon[best_index].x &&
         polygon[index].y < polygon[best_index].y)) {
      best_index = index;
    }
  }
  return best_index;
}

void BridgeHoleIntoPolygon(std::vector<ImVec2> *polygon,
                           const std::vector<ImVec2> &hole) {
  if (polygon->empty() || hole.empty()) {
    return;
  }

  const size_t hole_index = FindRightmostVertex(hole);
  const size_t polygon_index =
      FindNearestPolygonVertex(*polygon, hole[hole_index]);

  std::vector<ImVec2> bridged;
  bridged.reserve(polygon->size() + hole.size() + 2);
  bridged.insert(bridged.end(), polygon->begin(),
                 polygon->begin() +
                     static_cast<std::ptrdiff_t>(polygon_index + 1));
  for (size_t offset = 0; offset < hole.size(); ++offset) {
    const size_t index = (hole_index + offset) % hole.size();
    bridged.push_back(hole[index]);
  }
  bridged.push_back(hole[hole_index]);
  bridged.push_back((*polygon)[polygon_index]);
  bridged.insert(bridged.end(),
                 polygon->begin() +
                     static_cast<std::ptrdiff_t>(polygon_index + 1),
                 polygon->end());
  *polygon = std::move(bridged);
}

std::vector<std::vector<ImVec2>>
BuildGlyphFillPolygons(const ImportedTextGlyph &glyph) {
  struct OuterPolygon {
    std::vector<ImVec2> outline;
    std::vector<std::vector<ImVec2>> holes;
  };

  std::vector<OuterPolygon> outers;
  std::vector<std::vector<ImVec2>> holes;
  std::vector<ImVec2> flattened;

  for (const ImportedTextContour &contour : glyph.contours) {
    if (!contour.closed) {
      continue;
    }
    FlattenImportedTextContour(contour, &flattened);
    if (flattened.size() < 3) {
      continue;
    }

    if (contour.role == ImportedTextContourRole::Hole) {
      EnsureClockwise(&flattened);
      holes.push_back(flattened);
      continue;
    }

    EnsureCounterClockwise(&flattened);
    outers.push_back(OuterPolygon{flattened, {}});
  }

  for (const std::vector<ImVec2> &hole : holes) {
    bool assigned = false;
    for (OuterPolygon &outer : outers) {
      if (!outer.outline.empty() &&
          PointInPolygon(outer.outline, hole.front())) {
        outer.holes.push_back(hole);
        assigned = true;
        break;
      }
    }
    if (!assigned && !outers.empty()) {
      outers.front().holes.push_back(hole);
    }
  }

  std::vector<std::vector<ImVec2>> polygons;
  polygons.reserve(outers.size());
  for (OuterPolygon &outer : outers) {
    std::vector<ImVec2> polygon = outer.outline;
    for (const std::vector<ImVec2> &hole : outer.holes) {
      BridgeHoleIntoPolygon(&polygon, hole);
    }
    if (polygon.size() >= 3) {
      polygons.push_back(std::move(polygon));
    }
  }
  return polygons;
}

void AppendImportedTextContourPath(ImDrawList *draw_list,
                                   const CanvasState &state,
                                   const ImRect &canvas_rect,
                                   const ImportedArtwork &artwork,
                                   const ImportedTextContour &contour) {
  if (contour.segments.empty()) {
    return;
  }

  draw_list->PathClear();
  const ImVec2 first_world =
      ImportedArtworkPointToWorld(artwork, contour.segments.front().start);
  draw_list->PathLineTo(WorldToScreen(state, canvas_rect.Min, first_world));
  for (const ImportedPathSegment &segment : contour.segments) {
    const ImVec2 end_world = ImportedArtworkPointToWorld(artwork, segment.end);
    if (segment.kind == ImportedPathSegmentKind::Line) {
      draw_list->PathLineTo(WorldToScreen(state, canvas_rect.Min, end_world));
      continue;
    }

    const ImVec2 control1_world =
        ImportedArtworkPointToWorld(artwork, segment.control1);
    const ImVec2 control2_world =
        ImportedArtworkPointToWorld(artwork, segment.control2);
    draw_list->PathBezierCubicCurveTo(
        WorldToScreen(state, canvas_rect.Min, control1_world),
        WorldToScreen(state, canvas_rect.Min, control2_world),
        WorldToScreen(state, canvas_rect.Min, end_world));
  }
}

void DrawImportedDxfText(ImDrawList *draw_list, const CanvasState &state,
                         const ImRect &canvas_rect,
                         const ImportedArtwork &artwork,
                         const ImportedDxfText &text) {
  const ImU32 packed_color = ImGui::ColorConvertFloat4ToU32(text.stroke_color);
  const float outline_thickness =
      text.placeholder_only ? std::max(1.0f, text.stroke_width)
                            : std::max(1.0f, text.stroke_width * 0.35f);

  if (!text.placeholder_only) {
    for (const ImportedTextGlyph &glyph : text.glyphs) {
      const std::vector<std::vector<ImVec2>> polygons =
          BuildGlyphFillPolygons(glyph);
      for (const std::vector<ImVec2> &local_polygon : polygons) {
        if (local_polygon.size() < 3) {
          continue;
        }

        std::vector<ImVec2> screen_polygon;
        screen_polygon.reserve(local_polygon.size());
        for (const ImVec2 &point : local_polygon) {
          const ImVec2 world = ImportedArtworkPointToWorld(artwork, point);
          screen_polygon.push_back(
              WorldToScreen(state, canvas_rect.Min, world));
        }
        draw_list->AddConcavePolyFilled(screen_polygon.data(),
                                        static_cast<int>(screen_polygon.size()),
                                        packed_color);
      }
    }
  }

  for (const ImportedTextGlyph &glyph : text.glyphs) {
    for (const ImportedTextContour &contour : glyph.contours) {
      AppendImportedTextContourPath(draw_list, state, canvas_rect, artwork,
                                    contour);
      draw_list->PathStroke(packed_color, contour.closed, outline_thickness);
    }
  }

  for (const ImportedTextContour &contour : text.placeholder_contours) {
    AppendImportedTextContourPath(draw_list, state, canvas_rect, artwork,
                                  contour);
    draw_list->PathStroke(packed_color, contour.closed,
                          std::max(1.0f, text.stroke_width));
  }
}

void DrawGrid(ImDrawList *draw_list, const CanvasState &state,
              const ImRect &canvas_rect) {
  if (!state.grid.visible || state.grid.spacing <= 0.0f ||
      state.grid.subdivisions <= 0) {
    return;
  }

  const float major_spacing_world =
      UnitsToPixels(state.grid.spacing, state.grid.unit, state.calibration);
  if (major_spacing_world <= 0.0f) {
    return;
  }

  const float major_spacing_screen = major_spacing_world * state.view.zoom;
  const float minor_spacing_screen =
      major_spacing_screen / static_cast<float>(state.grid.subdivisions);

  draw_list->PushClipRect(canvas_rect.Min, canvas_rect.Max, true);

  if (minor_spacing_screen >= 8.0f) {
    const float minor_spacing_world =
        major_spacing_world / static_cast<float>(state.grid.subdivisions);
    const ImU32 minor_color =
        ImGui::ColorConvertFloat4ToU32(state.theme.grid_minor);
    const ImVec2 world_min =
        ScreenToWorld(state, canvas_rect.Min, canvas_rect.Min);
    const ImVec2 world_max =
        ScreenToWorld(state, canvas_rect.Min, canvas_rect.Max);
    const float start_x =
        std::floor(world_min.x / minor_spacing_world) * minor_spacing_world;
    const float end_x =
        std::ceil(world_max.x / minor_spacing_world) * minor_spacing_world;
    const float start_y =
        std::floor(world_min.y / minor_spacing_world) * minor_spacing_world;
    const float end_y =
        std::ceil(world_max.y / minor_spacing_world) * minor_spacing_world;

    for (float x = start_x; x <= end_x; x += minor_spacing_world) {
      const float screen_x =
          WorldToScreen(state, canvas_rect.Min, ImVec2(x, 0.0f)).x;
      draw_list->AddLine(ImVec2(screen_x, canvas_rect.Min.y),
                         ImVec2(screen_x, canvas_rect.Max.y), minor_color,
                         1.0f);
    }
    for (float y = start_y; y <= end_y; y += minor_spacing_world) {
      const float screen_y =
          WorldToScreen(state, canvas_rect.Min, ImVec2(0.0f, y)).y;
      draw_list->AddLine(ImVec2(canvas_rect.Min.x, screen_y),
                         ImVec2(canvas_rect.Max.x, screen_y), minor_color,
                         1.0f);
    }
  }

  const ImU32 major_color =
      ImGui::ColorConvertFloat4ToU32(state.theme.grid_major);
  const ImVec2 world_min =
      ScreenToWorld(state, canvas_rect.Min, canvas_rect.Min);
  const ImVec2 world_max =
      ScreenToWorld(state, canvas_rect.Min, canvas_rect.Max);
  const float start_x =
      std::floor(world_min.x / major_spacing_world) * major_spacing_world;
  const float end_x =
      std::ceil(world_max.x / major_spacing_world) * major_spacing_world;
  const float start_y =
      std::floor(world_min.y / major_spacing_world) * major_spacing_world;
  const float end_y =
      std::ceil(world_max.y / major_spacing_world) * major_spacing_world;

  for (float x = start_x; x <= end_x; x += major_spacing_world) {
    const float screen_x =
        WorldToScreen(state, canvas_rect.Min, ImVec2(x, 0.0f)).x;
    draw_list->AddLine(ImVec2(screen_x, canvas_rect.Min.y),
                       ImVec2(screen_x, canvas_rect.Max.y), major_color, 1.0f);
  }
  for (float y = start_y; y <= end_y; y += major_spacing_world) {
    const float screen_y =
        WorldToScreen(state, canvas_rect.Min, ImVec2(0.0f, y)).y;
    draw_list->AddLine(ImVec2(canvas_rect.Min.x, screen_y),
                       ImVec2(canvas_rect.Max.x, screen_y), major_color, 1.0f);
  }

  draw_list->PopClipRect();
}

void DrawImportedArtwork(ImDrawList *draw_list, const CanvasState &state,
                         const ImRect &canvas_rect,
                         int selected_imported_artwork_id,
                         const CanvasWidgetOptions &options) {
  draw_list->PushClipRect(canvas_rect.Min, canvas_rect.Max, true);

  const ImU32 selected_color =
      ImGui::ColorConvertFloat4ToU32(state.theme.working_area_selected);
  const ImU32 element_selection_color =
      ImGui::ColorConvertFloat4ToU32(state.theme.guide_hovered);
  const ImU32 operation_issue_color = IM_COL32(90, 170, 255, 255);

  for (const ImportedArtwork &artwork : state.imported_artwork) {
    if (!artwork.visible) {
      continue;
    }

    const bool show_issue_overlays =
        artwork.id == selected_imported_artwork_id ||
        state.selected_imported_debug.artwork_id == artwork.id;

    for (const ImportedDxfText &text : artwork.dxf_text) {
      if (text.placeholder_only && artwork.source_format == "DXF" &&
          !state.show_imported_dxf_text) {
        continue;
      }
      DrawImportedDxfText(draw_list, state, canvas_rect, artwork, text);
      if (show_issue_overlays &&
          text.issue_flags != ImportedElementIssueFlagNone) {
        const ImRect text_rect = ImportedElementScreenRect(
            state, canvas_rect.Min, artwork, text.bounds_min, text.bounds_max);
        draw_list->AddRect(text_rect.Min, text_rect.Max,
                           ImportedIssueOverlayColor(text.issue_flags), 3.0f, 0,
                           2.0f);
      }
      if (show_issue_overlays &&
          IsLastOperationIssueElement(state, artwork.id,
                                      ImportedElementKind::DxfText, text.id)) {
        const ImRect text_rect = ImportedElementScreenRect(
            state, canvas_rect.Min, artwork, text.bounds_min, text.bounds_max);
        draw_list->AddRect(text_rect.Min, text_rect.Max, operation_issue_color,
                           5.0f, 0, 2.0f);
      }
      if (IsImportedElementSelected(state, artwork.id,
                                    ImportedElementKind::DxfText, text.id)) {
        const ImRect text_rect = ImportedElementScreenRect(
            state, canvas_rect.Min, artwork, text.bounds_min, text.bounds_max);
        draw_list->AddRect(text_rect.Min, text_rect.Max,
                           element_selection_color, 3.0f, 0, 2.0f);
      }
    }

    for (const ImportedPath &path : artwork.paths) {
      if (path.segments.empty()) {
        continue;
      }

      if (artwork.source_format == "DXF" && !state.show_imported_dxf_text &&
          HasImportedPathFlag(path.flags, ImportedPathFlagTextPlaceholder)) {
        continue;
      }

      const auto append_path = [&]() {
        draw_list->PathClear();
        const ImVec2 first_world = ::im2d::ImportedArtworkPointToWorld(
            artwork, path.segments.front().start);
        draw_list->PathLineTo(
            WorldToScreen(state, canvas_rect.Min, first_world));

        for (const ImportedPathSegment &segment : path.segments) {
          const ImVec2 end_world =
              ::im2d::ImportedArtworkPointToWorld(artwork, segment.end);
          if (segment.kind == ImportedPathSegmentKind::Line) {
            draw_list->PathLineTo(
                WorldToScreen(state, canvas_rect.Min, end_world));
            continue;
          }

          const ImVec2 control1_world =
              ::im2d::ImportedArtworkPointToWorld(artwork, segment.control1);
          const ImVec2 control2_world =
              ::im2d::ImportedArtworkPointToWorld(artwork, segment.control2);
          draw_list->PathBezierCubicCurveTo(
              WorldToScreen(state, canvas_rect.Min, control1_world),
              WorldToScreen(state, canvas_rect.Min, control2_world),
              WorldToScreen(state, canvas_rect.Min, end_world));
        }
      };

      const bool is_dxf_artwork = artwork.source_format == "DXF";
      const bool is_filled_text =
          HasImportedPathFlag(path.flags, ImportedPathFlagFilledText);
      const bool is_hole_contour =
          HasImportedPathFlag(path.flags, ImportedPathFlagHoleContour);
      const float average_scale = (artwork.scale.x + artwork.scale.y) * 0.5f;
      const float thickness =
          is_dxf_artwork ? std::max(1.0f, path.stroke_width)
                         : std::max(1.0f, path.stroke_width *
                                              std::max(average_scale, 0.1f) *
                                              state.view.zoom);
      const ImU32 packed_color =
          ImGui::ColorConvertFloat4ToU32(path.stroke_color);
      const ImU32 background_color =
          ImGui::ColorConvertFloat4ToU32(state.theme.canvas_background);

      append_path();
      if (is_filled_text && path.closed && !is_hole_contour) {
        draw_list->PathFillConcave(packed_color);
        append_path();
      }
      if (is_hole_contour && path.closed) {
        draw_list->PathFillConcave(background_color);
      } else {
        draw_list->PathStroke(packed_color, path.closed,
                              is_filled_text ? std::max(1.0f, thickness * 0.35f)
                                             : thickness);
      }

      if (show_issue_overlays &&
          path.issue_flags != ImportedElementIssueFlagNone) {
        const ImRect path_rect = ImportedElementScreenRect(
            state, canvas_rect.Min, artwork, path.bounds_min, path.bounds_max);
        draw_list->AddRect(path_rect.Min, path_rect.Max,
                           ImportedIssueOverlayColor(path.issue_flags), 3.0f, 0,
                           2.0f);
      }
      if (show_issue_overlays &&
          IsLastOperationIssueElement(state, artwork.id,
                                      ImportedElementKind::Path, path.id)) {
        const ImRect path_rect = ImportedElementScreenRect(
            state, canvas_rect.Min, artwork, path.bounds_min, path.bounds_max);
        draw_list->AddRect(path_rect.Min, path_rect.Max, operation_issue_color,
                           5.0f, 0, 2.0f);
      }

      if (IsImportedElementSelected(state, artwork.id,
                                    ImportedElementKind::Path, path.id)) {
        const ImRect path_rect = ImportedElementScreenRect(
            state, canvas_rect.Min, artwork, path.bounds_min, path.bounds_max);
        draw_list->AddRect(path_rect.Min, path_rect.Max,
                           element_selection_color, 3.0f, 0, 2.0f);
      }
    }

    ImRect screen_rect;
    const bool has_debug_focus = TryGetImportedDebugScreenRect(
        state, canvas_rect.Min, artwork, &screen_rect);
    if (artwork.id == selected_imported_artwork_id || has_debug_focus) {
      if (!has_debug_focus) {
        screen_rect =
            ImportedArtworkScreenRect(state, canvas_rect.Min, artwork);
      }
      draw_list->AddRect(screen_rect.Min, screen_rect.Max, selected_color, 4.0f,
                         0, 2.0f);
      if ((!has_debug_focus || state.selected_imported_debug.kind ==
                                   ImportedDebugSelectionKind::Artwork) &&
          HasImportedArtworkFlag(artwork.flags, ImportedArtworkFlagResizable)) {
        const ImRect handle_rect(
            ImVec2(screen_rect.Max.x - options.resize_handle_size,
                   screen_rect.Max.y - options.resize_handle_size),
            screen_rect.Max);
        draw_list->AddRectFilled(handle_rect.Min, handle_rect.Max,
                                 selected_color, 2.0f);
      }
    }
  }

  draw_list->PopClipRect();
}

void DrawImportedMarquee(ImDrawList *draw_list, const CanvasState &state,
                         const ImRect &canvas_rect,
                         const TransientCanvasState &transient_state) {
  if (!transient_state.marquee_selecting) {
    return;
  }

  const ImRect marquee_world_rect(
      ImVec2(std::min(transient_state.marquee_start_world.x,
                      transient_state.marquee_end_world.x),
             std::min(transient_state.marquee_start_world.y,
                      transient_state.marquee_end_world.y)),
      ImVec2(std::max(transient_state.marquee_start_world.x,
                      transient_state.marquee_end_world.x),
             std::max(transient_state.marquee_start_world.y,
                      transient_state.marquee_end_world.y)));
  const ImRect marquee_screen_rect =
      WorldRectToScreenRect(state, canvas_rect.Min, marquee_world_rect);
  const ImU32 outline_color =
      ImGui::ColorConvertFloat4ToU32(state.theme.guide_hovered);
  const ImU32 fill_color = ImGui::ColorConvertFloat4ToU32(
      ImVec4(state.theme.guide_hovered.x, state.theme.guide_hovered.y,
             state.theme.guide_hovered.z, 0.18f));

  if (state.imported_artwork_edit_mode == ImportedArtworkEditMode::SelectOval) {
    const ImVec2 center(
        (marquee_screen_rect.Min.x + marquee_screen_rect.Max.x) * 0.5f,
        (marquee_screen_rect.Min.y + marquee_screen_rect.Max.y) * 0.5f);
    const ImVec2 radius(
        (marquee_screen_rect.Max.x - marquee_screen_rect.Min.x) * 0.5f,
        (marquee_screen_rect.Max.y - marquee_screen_rect.Min.y) * 0.5f);
    draw_list->AddEllipseFilled(center, radius, fill_color);
    draw_list->AddEllipse(center, radius, outline_color, 0, 2.0f);
    return;
  }

  draw_list->AddRectFilled(marquee_screen_rect.Min, marquee_screen_rect.Max,
                           fill_color, 3.0f);
  draw_list->AddRect(marquee_screen_rect.Min, marquee_screen_rect.Max,
                     outline_color, 3.0f, 0, 2.0f);
}

void DrawWorkingAreas(ImDrawList *draw_list, const CanvasState &state,
                      const ImRect &canvas_rect, int selected_working_area_id,
                      const CanvasWidgetOptions &options) {
  draw_list->PushClipRect(canvas_rect.Min, canvas_rect.Max, true);

  const ImU32 fill_color =
      ImGui::ColorConvertFloat4ToU32(state.theme.working_area_fill);
  const ImU32 border_color =
      ImGui::ColorConvertFloat4ToU32(state.theme.working_area_border);
  const ImU32 selected_color =
      ImGui::ColorConvertFloat4ToU32(state.theme.working_area_selected);
  const ImU32 export_outline =
      ImGui::ColorConvertFloat4ToU32(state.theme.export_area_outline);

  for (const WorkingArea &area : state.working_areas) {
    if (!area.visible) {
      continue;
    }

    const ImRect screen_rect =
        WorkingAreaScreenRect(state, canvas_rect.Min, area);
    const bool selected = area.id == selected_working_area_id;
    const ImU32 active_border = selected ? selected_color : border_color;
    const float thickness = selected ? 3.0f : 2.0f;

    draw_list->AddRectFilled(screen_rect.Min, screen_rect.Max, fill_color,
                             4.0f);
    draw_list->AddRect(screen_rect.Min, screen_rect.Max, active_border, 4.0f, 0,
                       thickness);
    draw_list->AddText(
        ImVec2(screen_rect.Min.x + 8.0f, screen_rect.Min.y + 8.0f),
        active_border, area.name.c_str());

    if (selected && HasWorkingAreaFlag(area.flags, WorkingAreaFlagResizable)) {
      const ImRect handle_rect(
          ImVec2(screen_rect.Max.x - options.resize_handle_size,
                 screen_rect.Max.y - options.resize_handle_size),
          screen_rect.Max);
      draw_list->AddRectFilled(handle_rect.Min, handle_rect.Max, active_border,
                               2.0f);
    }
  }

  for (const ExportArea &area : state.export_areas) {
    if (!area.visible) {
      continue;
    }

    const ImRect screen_rect(
        WorldToScreen(state, canvas_rect.Min, area.origin),
        WorldToScreen(
            state, canvas_rect.Min,
            ImVec2(area.origin.x + area.size.x, area.origin.y + area.size.y)));
    draw_list->AddRect(screen_rect.Min, screen_rect.Max, export_outline, 0.0f,
                       0, 1.0f);
  }

  draw_list->PopClipRect();
}

void DrawGuides(ImDrawList *draw_list, const CanvasState &state,
                const ImRect &canvas_rect, int hovered_guide_id,
                const TransientCanvasState &transient_state) {
  draw_list->PushClipRect(canvas_rect.Min, canvas_rect.Max, true);

  for (const Guide &guide : state.guides) {
    ImVec4 color = guide.locked ? state.theme.guide_locked : state.theme.guide;
    if (guide.id == hovered_guide_id ||
        guide.id == transient_state.dragging_guide_id) {
      color = state.theme.guide_hovered;
    }

    const ImU32 packed = ImGui::ColorConvertFloat4ToU32(color);
    if (guide.orientation == GuideOrientation::Vertical) {
      const float x =
          WorldToScreen(state, canvas_rect.Min, ImVec2(guide.position, 0.0f)).x;
      draw_list->AddLine(ImVec2(x, canvas_rect.Min.y),
                         ImVec2(x, canvas_rect.Max.y), packed,
                         guide.locked ? 1.0f : 2.0f);
    } else {
      const float y =
          WorldToScreen(state, canvas_rect.Min, ImVec2(0.0f, guide.position)).y;
      draw_list->AddLine(ImVec2(canvas_rect.Min.x, y),
                         ImVec2(canvas_rect.Max.x, y), packed,
                         guide.locked ? 1.0f : 2.0f);
    }
  }

  if (transient_state.creating_guide) {
    const ImU32 preview_color =
        ImGui::ColorConvertFloat4ToU32(state.theme.guide_hovered);
    if (transient_state.pending_orientation == GuideOrientation::Vertical) {
      const float x =
          WorldToScreen(state, canvas_rect.Min,
                        ImVec2(transient_state.pending_position, 0.0f))
              .x;
      draw_list->AddLine(ImVec2(x, canvas_rect.Min.y),
                         ImVec2(x, canvas_rect.Max.y), preview_color, 2.0f);
    } else {
      const float y =
          WorldToScreen(state, canvas_rect.Min,
                        ImVec2(0.0f, transient_state.pending_position))
              .y;
      draw_list->AddLine(ImVec2(canvas_rect.Min.x, y),
                         ImVec2(canvas_rect.Max.x, y), preview_color, 2.0f);
    }
  }

  draw_list->PopClipRect();
}

void DrawRulerAxis(ImDrawList *draw_list, const CanvasState &state,
                   const ImRect &rect, bool horizontal) {
  const ImU32 background =
      ImGui::ColorConvertFloat4ToU32(state.theme.ruler_background);
  const ImU32 tick_color =
      ImGui::ColorConvertFloat4ToU32(state.theme.ruler_ticks);
  const ImU32 text_color =
      ImGui::ColorConvertFloat4ToU32(state.theme.ruler_text);
  draw_list->AddRectFilled(rect.Min, rect.Max, background);

  const float pixels_per_unit =
      UnitsToPixels(1.0f, state.ruler_unit, state.calibration) *
      state.view.zoom;
  if (pixels_per_unit <= 0.0f) {
    return;
  }

  const float major_step_units = NiceStep(80.0f / pixels_per_unit);
  const float minor_step_units = major_step_units / 5.0f;
  const float major_step_world =
      UnitsToPixels(major_step_units, state.ruler_unit, state.calibration);
  const float minor_step_world =
      UnitsToPixels(minor_step_units, state.ruler_unit, state.calibration);

  if (minor_step_world <= 0.0f || major_step_world <= 0.0f) {
    return;
  }

  const float visible_min_world =
      horizontal ? ScreenToWorld(state, ImVec2(rect.Min.x, rect.Max.y),
                                 ImVec2(rect.Min.x, rect.Max.y))
                       .x
                 : ScreenToWorld(state, ImVec2(rect.Max.x, rect.Min.y),
                                 ImVec2(rect.Max.x, rect.Min.y))
                       .y;
  const float visible_max_world =
      horizontal ? ScreenToWorld(state, ImVec2(rect.Min.x, rect.Max.y),
                                 ImVec2(rect.Max.x, rect.Max.y))
                       .x
                 : ScreenToWorld(state, ImVec2(rect.Max.x, rect.Min.y),
                                 ImVec2(rect.Max.x, rect.Max.y))
                       .y;

  const float min_units =
      PixelsToUnits(visible_min_world, state.ruler_unit, state.calibration);
  const float max_units =
      PixelsToUnits(visible_max_world, state.ruler_unit, state.calibration);
  const float start_units =
      std::floor(std::min(min_units, max_units) / minor_step_units) *
      minor_step_units;
  const float end_units =
      std::ceil(std::max(min_units, max_units) / minor_step_units) *
      minor_step_units;

  for (float tick_units = start_units; tick_units <= end_units;
       tick_units += minor_step_units) {
    const float tick_world =
        UnitsToPixels(tick_units, state.ruler_unit, state.calibration);
    const float remainder = std::fmod(std::fabs(tick_units), major_step_units);
    const bool major = remainder < 0.0001f ||
                       std::fabs(remainder - major_step_units) < 0.0001f;
    const float tick_length =
        major
            ? (horizontal ? rect.GetHeight() - 4.0f : rect.GetWidth() - 4.0f)
            : (horizontal ? rect.GetHeight() * 0.45f : rect.GetWidth() * 0.45f);

    if (horizontal) {
      const float x = WorldToScreen(state, ImVec2(rect.Min.x, rect.Max.y),
                                    ImVec2(tick_world, 0.0f))
                          .x;
      draw_list->AddLine(ImVec2(x, rect.Max.y),
                         ImVec2(x, rect.Max.y - tick_length), tick_color, 1.0f);
      if (major) {
        char label[32];
        std::snprintf(label, sizeof(label), "%.3g", tick_units);
        draw_list->AddText(ImVec2(x + 4.0f, rect.Min.y + 4.0f), text_color,
                           label);
      }
    } else {
      const float y = WorldToScreen(state, ImVec2(rect.Max.x, rect.Min.y),
                                    ImVec2(0.0f, tick_world))
                          .y;
      draw_list->AddLine(ImVec2(rect.Max.x, y),
                         ImVec2(rect.Max.x - tick_length, y), tick_color, 1.0f);
      if (major) {
        char label[32];
        std::snprintf(label, sizeof(label), "%.3g", tick_units);
        draw_list->AddText(ImVec2(rect.Min.x + 4.0f, y + 2.0f), text_color,
                           label);
      }
    }
  }
}

} // namespace

bool DrawCanvas(CanvasState &state, const CanvasWidgetOptions &options) {
  InitializeDefaultDocument(state);
  TransientCanvasState &transient_state = GetTransientCanvasState();
  ImGuiIO &io = ImGui::GetIO();

  const ImVec2 canvas_size = ImGui::GetContentRegionAvail();
  if (canvas_size.x <= 2.0f || canvas_size.y <= 2.0f) {
    return false;
  }

  ImGui::PushID("im2d_canvas");
  ImGui::InvisibleButton("##surface", canvas_size,
                         ImGuiButtonFlags_MouseButtonLeft |
                             ImGuiButtonFlags_MouseButtonRight |
                             ImGuiButtonFlags_MouseButtonMiddle);

  const ImRect total_rect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
  const ImRect top_ruler_rect(
      ImVec2(total_rect.Min.x + options.ruler_thickness, total_rect.Min.y),
      ImVec2(total_rect.Max.x, total_rect.Min.y + options.ruler_thickness));
  const ImRect left_ruler_rect(
      ImVec2(total_rect.Min.x, total_rect.Min.y + options.ruler_thickness),
      ImVec2(total_rect.Min.x + options.ruler_thickness, total_rect.Max.y));
  const ImRect corner_rect(total_rect.Min,
                           ImVec2(total_rect.Min.x + options.ruler_thickness,
                                  total_rect.Min.y + options.ruler_thickness));
  const ImRect canvas_rect(ImVec2(total_rect.Min.x + options.ruler_thickness,
                                  total_rect.Min.y + options.ruler_thickness),
                           total_rect.Max);

  ImDrawList *draw_list = ImGui::GetWindowDrawList();
  draw_list->AddRectFilled(
      total_rect.Min, total_rect.Max,
      ImGui::ColorConvertFloat4ToU32(state.theme.ruler_background));
  draw_list->AddRectFilled(
      canvas_rect.Min, canvas_rect.Max,
      ImGui::ColorConvertFloat4ToU32(state.theme.canvas_background));
  draw_list->AddRectFilled(
      corner_rect.Min, corner_rect.Max,
      ImGui::ColorConvertFloat4ToU32(state.theme.ruler_background));

  const bool canvas_hovered = canvas_rect.Contains(io.MousePos);
  const bool top_ruler_hovered = top_ruler_rect.Contains(io.MousePos);
  const bool left_ruler_hovered = left_ruler_rect.Contains(io.MousePos);
  const ImportedArtworkHit imported_artwork_hit =
      FindHoveredImportedArtwork(state, canvas_rect.Min, canvas_rect,
                                 io.MousePos, options.resize_handle_size);
  const WorkingAreaHit area_hit =
      FindHoveredWorkingArea(state, canvas_rect.Min, canvas_rect, io.MousePos,
                             options.resize_handle_size);
  const bool marquee_mode_active =
      state.imported_artwork_edit_mode != ImportedArtworkEditMode::None &&
      state.selected_imported_artwork_id != 0;

  int hovered_guide_id = 0;
  if (canvas_hovered && !transient_state.creating_guide) {
    hovered_guide_id =
        FindHoveredGuide(state, canvas_rect.Min, canvas_rect, io.MousePos);
  }

  if (canvas_hovered && io.MouseWheel != 0.0f) {
    const ImVec2 focus_world =
        ScreenToWorld(state, canvas_rect.Min, io.MousePos);
    state.view.zoom =
        ClampZoom(state.view.zoom * std::pow(1.15f, io.MouseWheel));
    state.view.pan = ImVec2(
        io.MousePos.x - canvas_rect.Min.x - focus_world.x * state.view.zoom,
        io.MousePos.y - canvas_rect.Min.y - focus_world.y * state.view.zoom);
  }

  if (canvas_hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
    state.view.pan.x += io.MouseDelta.x;
    state.view.pan.y += io.MouseDelta.y;
  }

  if (!transient_state.creating_guide &&
      (top_ruler_hovered || left_ruler_hovered) &&
      ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    transient_state.creating_guide = true;
    transient_state.pending_orientation = left_ruler_hovered
                                              ? GuideOrientation::Vertical
                                              : GuideOrientation::Horizontal;
  }

  if (transient_state.creating_guide) {
    const ImVec2 world = ScreenToWorld(state, canvas_rect.Min, io.MousePos);
    const float raw_position =
        transient_state.pending_orientation == GuideOrientation::Vertical
            ? world.x
            : world.y;
    transient_state.pending_position =
        SnapAxisCoordinate(state, transient_state.pending_orientation,
                           raw_position)
            .value;

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      state.guides.push_back(Guide{state.next_guide_id++,
                                   transient_state.pending_orientation,
                                   transient_state.pending_position, false});
      transient_state.creating_guide = false;
    }
  }

  if (!transient_state.creating_guide && canvas_hovered &&
      ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    if (marquee_mode_active) {
      transient_state.marquee_selecting = true;
      transient_state.marquee_start_world =
          ScreenToWorld(state, canvas_rect.Min, io.MousePos);
      transient_state.marquee_end_world = transient_state.marquee_start_world;
      transient_state.dragging_guide_id = 0;
      transient_state.dragging_imported_artwork_id = 0;
      transient_state.resizing_imported_artwork_id = 0;
      transient_state.dragging_working_area_id = 0;
      transient_state.resizing_working_area_id = 0;
    } else if (hovered_guide_id != 0) {
      if (Guide *guide = FindGuide(state, hovered_guide_id);
          guide != nullptr && !guide->locked) {
        transient_state.dragging_guide_id = hovered_guide_id;
      }
    } else if (imported_artwork_hit.id != 0) {
      state.selected_imported_artwork_id = imported_artwork_hit.id;
      state.selected_imported_debug = {ImportedDebugSelectionKind::Artwork,
                                       imported_artwork_hit.id, 0};
      ClearSelectedImportedElements(state);
      transient_state.selected_working_area_id = 0;
      if (ImportedArtwork *artwork =
              FindImportedArtwork(state, imported_artwork_hit.id);
          artwork != nullptr) {
        const ImVec2 world = ScreenToWorld(state, canvas_rect.Min, io.MousePos);
        if (imported_artwork_hit.zone == ImportedArtworkHitZone::ResizeHandle &&
            HasImportedArtworkFlag(artwork->flags,
                                   ImportedArtworkFlagResizable)) {
          transient_state.resizing_imported_artwork_id =
              imported_artwork_hit.id;
          transient_state.imported_artwork_resize_initial_scale =
              artwork->scale;
        } else if (imported_artwork_hit.zone == ImportedArtworkHitZone::Body &&
                   HasImportedArtworkFlag(artwork->flags,
                                          ImportedArtworkFlagMovable)) {
          transient_state.dragging_imported_artwork_id =
              imported_artwork_hit.id;
          transient_state.imported_artwork_drag_offset =
              ImVec2(world.x - artwork->origin.x, world.y - artwork->origin.y);
        }
      }
    } else if (area_hit.id != 0) {
      transient_state.selected_working_area_id = area_hit.id;
      state.selected_imported_artwork_id = 0;
      ClearImportedDebugSelection(state);
      ClearSelectedImportedElements(state);
      if (WorkingArea *area = FindWorkingArea(state, area_hit.id);
          area != nullptr) {
        const ImVec2 world = ScreenToWorld(state, canvas_rect.Min, io.MousePos);
        if (area_hit.zone == WorkingAreaHitZone::ResizeHandle &&
            HasWorkingAreaFlag(area->flags, WorkingAreaFlagResizable)) {
          transient_state.resizing_working_area_id = area_hit.id;
        } else if (area_hit.zone == WorkingAreaHitZone::Body &&
                   HasWorkingAreaFlag(area->flags, WorkingAreaFlagMovable)) {
          transient_state.dragging_working_area_id = area_hit.id;
          transient_state.working_area_drag_offset =
              ImVec2(world.x - area->origin.x, world.y - area->origin.y);
        }
      }
    } else {
      state.selected_imported_artwork_id = 0;
      transient_state.selected_working_area_id = 0;
      ClearImportedDebugSelection(state);
      ClearSelectedImportedElements(state);
    }
  }

  if (transient_state.marquee_selecting) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      transient_state.marquee_end_world =
          ScreenToWorld(state, canvas_rect.Min, io.MousePos);
    } else {
      SelectImportedElementsInWorldRect(
          state, state.selected_imported_artwork_id,
          transient_state.marquee_start_world,
          transient_state.marquee_end_world, state.imported_artwork_edit_mode);
      transient_state.marquee_selecting = false;
    }
  }

  if (transient_state.dragging_guide_id != 0) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (Guide *guide = FindGuide(state, transient_state.dragging_guide_id);
          guide != nullptr) {
        const ImVec2 world = ScreenToWorld(state, canvas_rect.Min, io.MousePos);
        const float raw_position =
            guide->orientation == GuideOrientation::Vertical ? world.x
                                                             : world.y;
        guide->position = SnapAxisCoordinate(state, guide->orientation,
                                             raw_position, guide->id)
                              .value;
      }
    } else {
      transient_state.dragging_guide_id = 0;
    }
  }

  if (transient_state.dragging_imported_artwork_id != 0) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (ImportedArtwork *artwork = FindImportedArtwork(
              state, transient_state.dragging_imported_artwork_id);
          artwork != nullptr) {
        const ImVec2 world = ScreenToWorld(state, canvas_rect.Min, io.MousePos);
        ImVec2 new_origin(
            world.x - transient_state.imported_artwork_drag_offset.x,
            world.y - transient_state.imported_artwork_drag_offset.y);
        new_origin = SnapPoint(state, new_origin);
        artwork->origin = new_origin;
      }
    } else {
      transient_state.dragging_imported_artwork_id = 0;
    }
  }

  if (transient_state.resizing_imported_artwork_id != 0) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (ImportedArtwork *artwork = FindImportedArtwork(
              state, transient_state.resizing_imported_artwork_id);
          artwork != nullptr) {
        ImVec2 bottom_right = SnapPoint(
            state, ScreenToWorld(state, canvas_rect.Min, io.MousePos));
        bottom_right.x = std::max(
            bottom_right.x, artwork->origin.x + options.min_working_area_size);
        bottom_right.y = std::max(
            bottom_right.y, artwork->origin.y + options.min_working_area_size);
        const ImVec2 local_size = ImportedArtworkLocalSize(*artwork);
        const ImVec2 target_size(bottom_right.x - artwork->origin.x,
                                 bottom_right.y - artwork->origin.y);
        const ImVec2 target_scale(
            std::max(target_size.x / local_size.x, 0.01f),
            std::max(target_size.y / local_size.y, 0.01f));
        if (!IsImportedArtworkScaleRatioLocked(*artwork)) {
          UpdateImportedArtworkScaleFromTarget(*artwork, target_scale);
        } else {
          const float base_x = std::max(
              transient_state.imported_artwork_resize_initial_scale.x, 0.01f);
          const float base_y = std::max(
              transient_state.imported_artwork_resize_initial_scale.y, 0.01f);
          const float factor_x = target_scale.x / base_x;
          const float factor_y = target_scale.y / base_y;
          const float chosen_factor =
              std::abs(factor_x - 1.0f) >= std::abs(factor_y - 1.0f) ? factor_x
                                                                     : factor_y;
          artwork->scale.x = std::max(base_x * chosen_factor, 0.01f);
          artwork->scale.y = std::max(base_y * chosen_factor, 0.01f);
        }
      }
    } else {
      transient_state.resizing_imported_artwork_id = 0;
      transient_state.imported_artwork_resize_initial_scale =
          ImVec2(1.0f, 1.0f);
    }
  }

  if (transient_state.dragging_working_area_id != 0) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (WorkingArea *area =
              FindWorkingArea(state, transient_state.dragging_working_area_id);
          area != nullptr) {
        const ImVec2 world = ScreenToWorld(state, canvas_rect.Min, io.MousePos);
        ImVec2 new_origin(world.x - transient_state.working_area_drag_offset.x,
                          world.y - transient_state.working_area_drag_offset.y);
        new_origin = SnapPoint(state, new_origin);
        area->origin = new_origin;
        SyncExportAreaFromWorkingArea(state, area->id);
      }
    } else {
      transient_state.dragging_working_area_id = 0;
    }
  }

  if (transient_state.resizing_working_area_id != 0) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (WorkingArea *area =
              FindWorkingArea(state, transient_state.resizing_working_area_id);
          area != nullptr) {
        ImVec2 bottom_right = SnapPoint(
            state, ScreenToWorld(state, canvas_rect.Min, io.MousePos));
        bottom_right.x = std::max(
            bottom_right.x, area->origin.x + options.min_working_area_size);
        bottom_right.y = std::max(
            bottom_right.y, area->origin.y + options.min_working_area_size);
        area->size = ImVec2(bottom_right.x - area->origin.x,
                            bottom_right.y - area->origin.y);
        SyncExportAreaFromWorkingArea(state, area->id);
      }
    } else {
      transient_state.resizing_working_area_id = 0;
    }
  }

  if (imported_artwork_hit.id != 0 &&
      ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    transient_state.context_imported_artwork_id = imported_artwork_hit.id;
    state.selected_imported_artwork_id = imported_artwork_hit.id;
    state.selected_imported_debug = {ImportedDebugSelectionKind::Artwork,
                                     imported_artwork_hit.id, 0};
    ImGui::OpenPopup("imported_artwork_context_menu");
  } else if (hovered_guide_id != 0 &&
             ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    transient_state.context_guide_id = hovered_guide_id;
    ImGui::OpenPopup("guide_context_menu");
  } else if ((top_ruler_hovered || left_ruler_hovered) &&
             ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    ImGui::OpenPopup("ruler_context_menu");
  } else if (canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    ImGui::OpenPopup("canvas_context_menu");
  }

  DrawGrid(draw_list, state, canvas_rect);
  DrawWorkingAreas(draw_list, state, canvas_rect,
                   transient_state.selected_working_area_id, options);
  DrawImportedArtwork(draw_list, state, canvas_rect,
                      state.selected_imported_artwork_id, options);
  DrawGuides(draw_list, state, canvas_rect, hovered_guide_id, transient_state);
  DrawImportedMarquee(draw_list, state, canvas_rect, transient_state);
  DrawRulerAxis(draw_list, state, top_ruler_rect, true);
  DrawRulerAxis(draw_list, state, left_ruler_rect, false);
  draw_list->AddLine(ImVec2(canvas_rect.Min.x, total_rect.Min.y),
                     ImVec2(canvas_rect.Min.x, total_rect.Max.y),
                     ImGui::ColorConvertFloat4ToU32(state.theme.ruler_ticks));
  draw_list->AddLine(ImVec2(total_rect.Min.x, canvas_rect.Min.y),
                     ImVec2(total_rect.Max.x, canvas_rect.Min.y),
                     ImGui::ColorConvertFloat4ToU32(state.theme.ruler_ticks));
  draw_list->AddText(ImVec2(corner_rect.Min.x + 6.0f, corner_rect.Min.y + 7.0f),
                     ImGui::ColorConvertFloat4ToU32(state.theme.ruler_text),
                     MeasurementUnitLabel(state.ruler_unit));

  if (ImGui::BeginPopup("ruler_context_menu")) {
    ImGui::TextUnformatted("Ruler Units");
    ImGui::Separator();
    for (MeasurementUnit unit :
         {MeasurementUnit::Millimeters, MeasurementUnit::Inches,
          MeasurementUnit::Pixels}) {
      const bool selected = state.ruler_unit == unit;
      if (ImGui::MenuItem(MeasurementUnitLabel(unit), nullptr, selected)) {
        state.ruler_unit = unit;
      }
    }
    ImGui::EndPopup();
  }

  if (ImGui::BeginPopup("imported_artwork_context_menu")) {
    if (ImportedArtwork *artwork = FindImportedArtwork(
            state, transient_state.context_imported_artwork_id);
        artwork != nullptr) {
      if (ImGui::MenuItem("Prepare For Cutting")) {
        PrepareImportedArtworkForCutting(state, artwork->id);
        ImGui::CloseCurrentPopup();
      }
      if (!state.selected_imported_elements.empty() &&
          ImGui::MenuItem("Extract Selected Elements")) {
        ExtractSelectedImportedElements(state, artwork->id);
        ImGui::CloseCurrentPopup();
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Flip Horizontal")) {
        FlipImportedArtworkHorizontal(state, artwork->id);
        ImGui::CloseCurrentPopup();
      }
      if (ImGui::MenuItem("Flip Vertical")) {
        FlipImportedArtworkVertical(state, artwork->id);
        ImGui::CloseCurrentPopup();
      }
      if (ImGui::MenuItem("Rotate 90 CW")) {
        RotateImportedArtworkClockwise(state, artwork->id);
        ImGui::CloseCurrentPopup();
      }
      if (ImGui::MenuItem("Rotate 90 CCW")) {
        RotateImportedArtworkCounterClockwise(state, artwork->id);
        ImGui::CloseCurrentPopup();
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Delete")) {
        DeleteImportedArtwork(state, artwork->id);
        transient_state.context_imported_artwork_id = 0;
        transient_state.dragging_imported_artwork_id = 0;
        transient_state.resizing_imported_artwork_id = 0;
        transient_state.imported_artwork_resize_initial_scale =
            ImVec2(1.0f, 1.0f);
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }

  if (ImGui::BeginPopup("guide_context_menu")) {
    if (Guide *guide = FindGuide(state, transient_state.context_guide_id);
        guide != nullptr) {
      if (state.selected_imported_artwork_id != 0 &&
          ImGui::MenuItem("Split Selected Artwork At Guide")) {
        SeparateImportedArtworkByGuide(
            state, state.selected_imported_artwork_id, guide->id);
        ImGui::CloseCurrentPopup();
      }
      if (state.selected_imported_artwork_id != 0) {
        ImGui::Separator();
      }
      if (ImGui::MenuItem(guide->locked ? "Unlock guide" : "Lock guide")) {
        guide->locked = !guide->locked;
      }
      if (ImGui::MenuItem("Delete guide")) {
        RemoveGuide(state, guide->id);
        transient_state.context_guide_id = 0;
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }

  if (ImGui::BeginPopup("canvas_context_menu")) {
    ImGui::TextUnformatted("Canvas actions TBD");
    ImGui::Separator();
    ImGui::MenuItem("Placeholder", nullptr, false, false);
    ImGui::EndPopup();
  }

  ImGui::PopID();
  return canvas_hovered;
}

} // namespace im2d