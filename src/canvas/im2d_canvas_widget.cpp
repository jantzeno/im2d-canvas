#include "im2d_canvas_widget.h"

#include "im2d_canvas_document.h"
#include "im2d_canvas_imported_artwork_ops.h"
#include "im2d_canvas_internal.h"
#include "im2d_canvas_notification.h"
#include "im2d_canvas_snap.h"
#include "im2d_canvas_undo.h"
#include "im2d_canvas_units.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

#include <imgui_internal.h>

namespace im2d {

namespace {

constexpr float kImportedPreviewStrokeWidth = 1.0f;
constexpr float kImportedPreviewCurveFlatnessPixels = 0.35f;
constexpr int kImportedPreviewCurveMaxSubdivisionDepth = 10;
constexpr float kMarqueeDragStartDistancePixels = 4.0f;
constexpr float kRightDragStartDistancePixels = 4.0f;
constexpr float kMarqueeOutlineThickness = 1.5f;
constexpr float kMarqueeFillAlpha = 0.18f;

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

struct ImportedArtworkDragSnapshot {
  int id = 0;
  ImVec2 origin = ImVec2(0.0f, 0.0f);
};

struct ImportedArtworkResizeSnapshot {
  int id = 0;
  ImVec2 origin = ImVec2(0.0f, 0.0f);
  ImVec2 scale = ImVec2(1.0f, 1.0f);
};

struct ImportedPathHit {
  int artwork_id = 0;
  int path_id = 0;
};

bool ImportedPreviewCurveFlatEnough(const ImVec2 &start, const ImVec2 &control1,
                                    const ImVec2 &control2, const ImVec2 &end);
void AppendImportedSegmentPath(ImDrawList *draw_list, const CanvasState &state,
                               const ImRect &canvas_rect,
                               const ImportedArtwork &artwork,
                               const std::vector<ImportedPathSegment> &segments,
                               bool closed);
std::vector<std::vector<ImVec2>>
BuildGlyphFillPolygons(const ImportedTextGlyph &glyph);
void AppendImportedTextContourPath(ImDrawList *draw_list,
                                   const CanvasState &state,
                                   const ImRect &canvas_rect,
                                   const ImportedArtwork &artwork,
                                   const ImportedTextContour &contour);

struct TransientCanvasState {
  enum class MarqueeInteractionState {
    Idle,
    ArmedCanvasClear,
    ArmedObjectClear,
    ArmedPendingObjectTarget,
    SelectingCanvas,
    SelectingObject,
  };

  bool creating_guide = false;
  MarqueeInteractionState marquee_state = MarqueeInteractionState::Idle;
  GuideOrientation pending_orientation = GuideOrientation::Vertical;
  float pending_position = 0.0f;
  int dragging_guide_id = 0;
  int context_guide_id = 0;
  int context_imported_artwork_id = 0;
  std::vector<ImportedArtworkDragSnapshot> dragging_imported_artwork;
  std::vector<ImportedArtworkResizeSnapshot> resizing_imported_artwork_group;
  int dragging_imported_artwork_anchor_id = 0;
  int resizing_imported_artwork_id = 0;
  ImRect imported_artwork_resize_initial_world_rect;
  ImVec2 imported_artwork_resize_initial_scale = ImVec2(1.0f, 1.0f);
  int dragging_working_area_id = 0;
  int resizing_working_area_id = 0;
  bool right_mouse_pressed_in_canvas = false;
  bool right_mouse_dragged = false;
  ImVec2 imported_artwork_drag_offset = ImVec2(0.0f, 0.0f);
  ImVec2 working_area_drag_offset = ImVec2(0.0f, 0.0f);
  int marquee_artwork_id = 0;
  ImVec2 marquee_press_screen = ImVec2(0.0f, 0.0f);
  ImVec2 marquee_start_world = ImVec2(0.0f, 0.0f);
  ImVec2 marquee_end_world = ImVec2(0.0f, 0.0f);
  int last_selected_imported_artwork_id = 0;
  int last_selected_imported_artwork_count = 0;
  ImportedArtworkSelectionScope last_selection_scope =
      ImportedArtworkSelectionScope::Canvas;
};

TransientCanvasState &GetTransientCanvasState() {
  static TransientCanvasState state;
  return state;
}

void ResetMarqueeInteractionState(TransientCanvasState *state) {
  state->marquee_state = TransientCanvasState::MarqueeInteractionState::Idle;
  state->marquee_artwork_id = 0;
  state->marquee_press_screen = ImVec2(0.0f, 0.0f);
  state->marquee_start_world = ImVec2(0.0f, 0.0f);
  state->marquee_end_world = ImVec2(0.0f, 0.0f);
}

bool IsMarqueeInteractionActive(const TransientCanvasState &state) {
  return state.marquee_state !=
         TransientCanvasState::MarqueeInteractionState::Idle;
}

bool IsMarqueeInteractionArmed(const TransientCanvasState &state) {
  return state.marquee_state ==
             TransientCanvasState::MarqueeInteractionState::ArmedCanvasClear ||
         state.marquee_state == TransientCanvasState::MarqueeInteractionState::
                                    ArmedPendingObjectTarget ||
         state.marquee_state ==
             TransientCanvasState::MarqueeInteractionState::ArmedObjectClear;
}

bool IsMarqueeInteractionSelecting(const TransientCanvasState &state) {
  return state.marquee_state ==
             TransientCanvasState::MarqueeInteractionState::SelectingCanvas ||
         state.marquee_state ==
             TransientCanvasState::MarqueeInteractionState::SelectingObject;
}

void ClearActiveCanvasManipulation(TransientCanvasState *state) {
  state->dragging_guide_id = 0;
  state->dragging_imported_artwork.clear();
  state->resizing_imported_artwork_group.clear();
  state->dragging_imported_artwork_anchor_id = 0;
  state->resizing_imported_artwork_id = 0;
  state->dragging_working_area_id = 0;
  state->resizing_working_area_id = 0;
}

bool IsCanvasArtworkScope(const CanvasState &state) {
  return state.selection_scope == ImportedArtworkSelectionScope::Canvas;
}

bool IsImportedArtworkSelectionModifierDown() {
  const ImGuiIO &io = ImGui::GetIO();
  return io.KeyCtrl || io.KeyShift;
}

bool TryGetImportedArtworkWorldRect(const ImportedArtwork &artwork,
                                    ImRect *world_rect) {
  if (world_rect == nullptr) {
    return false;
  }

  ImVec2 world_min;
  ImVec2 world_max;
  ImportedLocalBoundsToWorldBounds(artwork, artwork.bounds_min,
                                   artwork.bounds_max, &world_min, &world_max);
  *world_rect = ImRect(world_min, world_max);
  return true;
}

bool TryGetSelectedImportedArtworkWorldRect(const CanvasState &state,
                                            ImRect *world_rect) {
  if (world_rect == nullptr ||
      CountSelectedImportedArtworkObjects(state) == 0) {
    return false;
  }

  bool found = false;
  ImVec2 selection_min(0.0f, 0.0f);
  ImVec2 selection_max(0.0f, 0.0f);
  for (const ImportedArtwork &artwork : state.imported_artwork) {
    if (!artwork.visible ||
        !IsImportedArtworkObjectSelected(state, artwork.id)) {
      continue;
    }

    ImRect artwork_world_rect;
    if (!TryGetImportedArtworkWorldRect(artwork, &artwork_world_rect)) {
      continue;
    }

    if (!found) {
      selection_min = artwork_world_rect.Min;
      selection_max = artwork_world_rect.Max;
      found = true;
      continue;
    }

    selection_min.x = std::min(selection_min.x, artwork_world_rect.Min.x);
    selection_min.y = std::min(selection_min.y, artwork_world_rect.Min.y);
    selection_max.x = std::max(selection_max.x, artwork_world_rect.Max.x);
    selection_max.y = std::max(selection_max.y, artwork_world_rect.Max.y);
  }

  if (!found) {
    return false;
  }

  *world_rect = ImRect(selection_min, selection_max);
  return true;
}

bool TryGetSelectedImportedArtworkScreenRect(const CanvasState &state,
                                             const ImVec2 &canvas_min,
                                             ImRect *screen_rect) {
  ImRect world_rect;
  if (!TryGetSelectedImportedArtworkWorldRect(state, &world_rect)) {
    return false;
  }

  const ImVec2 min(
      canvas_min.x + state.view.pan.x + world_rect.Min.x * state.view.zoom,
      canvas_min.y + state.view.pan.y + world_rect.Min.y * state.view.zoom);
  const ImVec2 max(
      canvas_min.x + state.view.pan.x + world_rect.Max.x * state.view.zoom,
      canvas_min.y + state.view.pan.y + world_rect.Max.y * state.view.zoom);
  *screen_rect = ImRect(ImVec2(std::min(min.x, max.x), std::min(min.y, max.y)),
                        ImVec2(std::max(min.x, max.x), std::max(min.y, max.y)));
  return true;
}

bool PointInsideSelectionShape(const ImRect &selection_rect,
                               ImportedArtworkEditMode mode,
                               const ImVec2 &point) {
  if (mode == ImportedArtworkEditMode::SelectOval) {
    const ImVec2 center((selection_rect.Min.x + selection_rect.Max.x) * 0.5f,
                        (selection_rect.Min.y + selection_rect.Max.y) * 0.5f);
    const float radius_x =
        std::max((selection_rect.Max.x - selection_rect.Min.x) * 0.5f, 0.001f);
    const float radius_y =
        std::max((selection_rect.Max.y - selection_rect.Min.y) * 0.5f, 0.001f);
    const float normalized_x = (point.x - center.x) / radius_x;
    const float normalized_y = (point.y - center.y) / radius_y;
    return normalized_x * normalized_x + normalized_y * normalized_y <= 1.0f;
  }

  return selection_rect.Contains(point);
}

ImRect ImportedArtworkResizeHandleRect(const ImRect &screen_rect,
                                       const float resize_handle_size) {
  return ImRect(ImVec2(screen_rect.Max.x - resize_handle_size,
                       screen_rect.Max.y - resize_handle_size),
                screen_rect.Max);
}

float DistanceSquaredToSegment(const ImVec2 &point, const ImVec2 &segment_start,
                               const ImVec2 &segment_end) {
  const ImVec2 delta(segment_end.x - segment_start.x,
                     segment_end.y - segment_start.y);
  const float length_squared = delta.x * delta.x + delta.y * delta.y;
  if (length_squared <= 0.0001f) {
    const float dx = point.x - segment_start.x;
    const float dy = point.y - segment_start.y;
    return dx * dx + dy * dy;
  }

  const float projection = ((point.x - segment_start.x) * delta.x +
                            (point.y - segment_start.y) * delta.y) /
                           length_squared;
  const float clamped_projection = std::clamp(projection, 0.0f, 1.0f);
  const ImVec2 nearest(segment_start.x + delta.x * clamped_projection,
                       segment_start.y + delta.y * clamped_projection);
  const float dx = point.x - nearest.x;
  const float dy = point.y - nearest.y;
  return dx * dx + dy * dy;
}

ImportedPathHit FindHoveredImportedPath(const CanvasState &state,
                                        const ImVec2 &canvas_min,
                                        const ImRect &canvas_rect,
                                        const ImVec2 &mouse_pos,
                                        const float hit_radius_pixels) {
  if (!canvas_rect.Contains(mouse_pos)) {
    return {};
  }

  ImportedPathHit hit;
  float best_distance_squared = hit_radius_pixels * hit_radius_pixels;
  std::vector<ImVec2> sample_points;

  for (auto artwork_it = state.imported_artwork.rbegin();
       artwork_it != state.imported_artwork.rend(); ++artwork_it) {
    const ImportedArtwork &artwork = *artwork_it;
    if (!artwork.visible) {
      continue;
    }

    for (auto path_it = artwork.paths.rbegin(); path_it != artwork.paths.rend();
         ++path_it) {
      const ImportedPath &path = *path_it;
      if (path.segments.empty()) {
        continue;
      }

      ImVec2 world_min;
      ImVec2 world_max;
      ImportedLocalBoundsToWorldBounds(artwork, path.bounds_min,
                                       path.bounds_max, &world_min, &world_max);
      const ImVec2 screen_min(
          canvas_min.x + state.view.pan.x + world_min.x * state.view.zoom,
          canvas_min.y + state.view.pan.y + world_min.y * state.view.zoom);
      const ImVec2 screen_max(
          canvas_min.x + state.view.pan.x + world_max.x * state.view.zoom,
          canvas_min.y + state.view.pan.y + world_max.y * state.view.zoom);
      const ImRect path_rect(ImVec2(std::min(screen_min.x, screen_max.x),
                                    std::min(screen_min.y, screen_max.y)),
                             ImVec2(std::max(screen_min.x, screen_max.x),
                                    std::max(screen_min.y, screen_max.y)));
      const ImRect expanded_rect(ImVec2(path_rect.Min.x - hit_radius_pixels,
                                        path_rect.Min.y - hit_radius_pixels),
                                 ImVec2(path_rect.Max.x + hit_radius_pixels,
                                        path_rect.Max.y + hit_radius_pixels));
      if (!expanded_rect.Contains(mouse_pos)) {
        continue;
      }

      sample_points.clear();
      detail::AppendPathSamplePointsWorld(artwork, path, &sample_points);
      if (sample_points.size() < 2) {
        continue;
      }

      for (size_t index = 1; index < sample_points.size(); ++index) {
        const ImVec2 screen_start(
            canvas_min.x + state.view.pan.x +
                sample_points[index - 1].x * state.view.zoom,
            canvas_min.y + state.view.pan.y +
                sample_points[index - 1].y * state.view.zoom);
        const ImVec2 screen_end(canvas_min.x + state.view.pan.x +
                                    sample_points[index].x * state.view.zoom,
                                canvas_min.y + state.view.pan.y +
                                    sample_points[index].y * state.view.zoom);
        const float distance_squared =
            DistanceSquaredToSegment(mouse_pos, screen_start, screen_end);
        if (distance_squared > best_distance_squared) {
          continue;
        }

        best_distance_squared = distance_squared;
        hit = {artwork.id, path.id};
      }
    }

    if (hit.path_id != 0) {
      return hit;
    }
  }

  return hit;
}

void SelectImportedArtworkObjectsInWorldRect(CanvasState &state,
                                             const ImVec2 &world_start,
                                             const ImVec2 &world_end,
                                             ImportedArtworkEditMode mode) {
  const ImRect selection_rect(ImVec2(std::min(world_start.x, world_end.x),
                                     std::min(world_start.y, world_end.y)),
                              ImVec2(std::max(world_start.x, world_end.x),
                                     std::max(world_start.y, world_end.y)));

  ClearSelectedImportedArtworkObjects(state);
  ClearSelectedImportedElements(state);
  state.selected_working_area_id = 0;

  for (const ImportedArtwork &artwork : state.imported_artwork) {
    if (!artwork.visible) {
      continue;
    }

    ImRect artwork_world_rect;
    if (!TryGetImportedArtworkWorldRect(artwork, &artwork_world_rect)) {
      continue;
    }

    const ImVec2 corners[] = {
        artwork_world_rect.Min,
        ImVec2(artwork_world_rect.Max.x, artwork_world_rect.Min.y),
        artwork_world_rect.Max,
        ImVec2(artwork_world_rect.Min.x, artwork_world_rect.Max.y),
    };
    bool enclosed = true;
    for (const ImVec2 &corner : corners) {
      if (!PointInsideSelectionShape(selection_rect, mode, corner)) {
        enclosed = false;
        break;
      }
    }

    if (!enclosed) {
      continue;
    }

    AddSelectedImportedArtworkObject(state, artwork.id);
  }

  if (state.selected_imported_artwork_ids.empty()) {
    ClearImportedDebugSelection(state);
    return;
  }

  state.selected_imported_artwork_id =
      state.selected_imported_artwork_ids.front();
  state.selected_imported_debug = {ImportedDebugSelectionKind::Artwork,
                                   state.selected_imported_artwork_id, 0};
}

void BeginImportedArtworkDrag(CanvasState &state,
                              TransientCanvasState *transient_state,
                              const int anchor_artwork_id,
                              const ImVec2 &mouse_world) {
  transient_state->dragging_imported_artwork.clear();
  transient_state->dragging_imported_artwork_anchor_id = anchor_artwork_id;

  ImportedArtwork *anchor_artwork =
      FindImportedArtwork(state, anchor_artwork_id);
  if (anchor_artwork == nullptr) {
    return;
  }

  PushUndoSnapshot(state, "Move imported artwork");

  transient_state->imported_artwork_drag_offset =
      ImVec2(mouse_world.x - anchor_artwork->origin.x,
             mouse_world.y - anchor_artwork->origin.y);

  const bool drag_selection_set =
      IsCanvasArtworkScope(state) &&
      CountSelectedImportedArtworkObjects(state) > 1 &&
      IsImportedArtworkObjectSelected(state, anchor_artwork_id);
  if (drag_selection_set) {
    transient_state->dragging_imported_artwork.reserve(
        state.selected_imported_artwork_ids.size());
    for (const int artwork_id : state.selected_imported_artwork_ids) {
      if (ImportedArtwork *artwork = FindImportedArtwork(state, artwork_id);
          artwork != nullptr) {
        transient_state->dragging_imported_artwork.push_back(
            {artwork_id, artwork->origin});
      }
    }
    return;
  }

  transient_state->dragging_imported_artwork.push_back(
      {anchor_artwork_id, anchor_artwork->origin});
}

void BeginSelectedImportedArtworkResize(CanvasState &state,
                                        TransientCanvasState *transient_state) {
  transient_state->resizing_imported_artwork_group.clear();
  if (!TryGetSelectedImportedArtworkWorldRect(
          state,
          &transient_state->imported_artwork_resize_initial_world_rect)) {
    return;
  }

  PushUndoSnapshot(state, "Resize imported artwork");

  const std::vector<int> selected_artwork_ids =
      GetSelectedImportedArtworkObjects(state);
  transient_state->resizing_imported_artwork_group.reserve(
      selected_artwork_ids.size());
  for (const int artwork_id : selected_artwork_ids) {
    if (ImportedArtwork *artwork = FindImportedArtwork(state, artwork_id);
        artwork != nullptr) {
      transient_state->resizing_imported_artwork_group.push_back(
          {artwork_id, artwork->origin, artwork->scale});
    }
  }
}

void SelectImportedArtworkForCanvas(CanvasState &state,
                                    TransientCanvasState *transient_state,
                                    int artwork_id) {
  SetSingleSelectedImportedArtworkObject(state, artwork_id);
  state.selected_imported_debug = {ImportedDebugSelectionKind::Artwork,
                                   artwork_id, 0};
  ClearSelectedImportedElements(state);
  state.selected_working_area_id = 0;
  ResetMarqueeInteractionState(transient_state);
}

int ResolveActiveImportedArtworkId(const CanvasState &state,
                                   const int preferred_artwork_id = 0) {
  if (preferred_artwork_id != 0) {
    return preferred_artwork_id;
  }
  if (state.selected_imported_artwork_id != 0) {
    return state.selected_imported_artwork_id;
  }
  if (state.selected_imported_debug.artwork_id != 0) {
    return state.selected_imported_debug.artwork_id;
  }
  return state.selected_imported_artwork_ids.empty()
             ? 0
             : state.selected_imported_artwork_ids.front();
}

int ResolveRecoverableObjectScopeArtworkId(
    const CanvasState &state, const TransientCanvasState &transient_state) {
  const int candidates[] = {
      state.selected_imported_artwork_id,
      state.selected_imported_debug.artwork_id,
      transient_state.marquee_artwork_id,
      state.selected_imported_artwork_ids.empty()
          ? 0
          : state.selected_imported_artwork_ids.front(),
  };

  for (const int artwork_id : candidates) {
    if (artwork_id == 0) {
      continue;
    }
    if (FindImportedArtwork(state, artwork_id) != nullptr) {
      return artwork_id;
    }
  }

  return 0;
}

void ClearAllImportedSelection(CanvasState &state) {
  ClearSelectedImportedArtworkObjects(state);
  ClearImportedDebugSelection(state);
  ClearSelectedImportedElements(state);
}

void SelectAllVisibleImportedArtwork(CanvasState &state) {
  ClearAllImportedSelection(state);
  for (const ImportedArtwork &artwork : state.imported_artwork) {
    if (!artwork.visible) {
      continue;
    }
    AddSelectedImportedArtworkObject(state, artwork.id);
  }

  if (!state.selected_imported_artwork_ids.empty()) {
    state.selected_imported_artwork_id =
        state.selected_imported_artwork_ids.front();
    state.selected_imported_debug = {ImportedDebugSelectionKind::Artwork,
                                     state.selected_imported_artwork_id, 0};
  }
}

void ClearImportedSelectionForCurrentScope(CanvasState &state,
                                           const int preferred_artwork_id = 0) {
  state.selected_working_area_id = 0;

  if (IsCanvasArtworkScope(state)) {
    ClearAllImportedSelection(state);
    return;
  }

  const int active_artwork_id =
      ResolveActiveImportedArtworkId(state, preferred_artwork_id);

  ClearSelectedImportedElements(state);
  if (active_artwork_id != 0) {
    SetSingleSelectedImportedArtworkObject(state, active_artwork_id);
    state.selected_imported_debug = {ImportedDebugSelectionKind::Artwork,
                                     active_artwork_id, 0};
    return;
  }

  ClearSelectedImportedArtworkObjects(state);
  ClearImportedDebugSelection(state);
}

void EnsureObjectScopeArtworkContext(CanvasState &state,
                                     TransientCanvasState *transient_state) {
  if (IsCanvasArtworkScope(state)) {
    return;
  }

  if (state.selected_imported_artwork_id == 0 &&
      state.selected_imported_debug.artwork_id == 0 &&
      state.selected_imported_elements.empty() &&
      transient_state->marquee_artwork_id == 0) {
    return;
  }

  const int artwork_id =
      ResolveRecoverableObjectScopeArtworkId(state, *transient_state);
  if (artwork_id == 0) {
    return;
  }

  SetSingleSelectedImportedArtworkObject(state, artwork_id);
  state.selected_working_area_id = 0;
  if (state.selected_imported_elements.empty() ||
      state.selected_imported_debug.artwork_id != artwork_id) {
    state.selected_imported_debug = {ImportedDebugSelectionKind::Artwork,
                                     artwork_id, 0};
  }
  transient_state->last_selected_imported_artwork_id = artwork_id;
  transient_state->last_selected_imported_artwork_count = 1;
}

void SelectImportedPathForObjectScope(CanvasState &state,
                                      TransientCanvasState *transient_state,
                                      const int artwork_id, const int path_id,
                                      const bool additive_selection) {
  const bool same_artwork = state.selected_imported_artwork_id == artwork_id;
  SetSingleSelectedImportedArtworkObject(state, artwork_id);
  state.selected_working_area_id = 0;
  ResetMarqueeInteractionState(transient_state);

  if (!additive_selection || !same_artwork) {
    state.selected_imported_elements = {{ImportedElementKind::Path, path_id}};
    state.selected_imported_debug = {ImportedDebugSelectionKind::Path,
                                     artwork_id, path_id};
    return;
  }

  auto existing =
      std::find_if(state.selected_imported_elements.begin(),
                   state.selected_imported_elements.end(),
                   [path_id](const ImportedElementSelection &selection) {
                     return selection.kind == ImportedElementKind::Path &&
                            selection.item_id == path_id;
                   });
  if (existing != state.selected_imported_elements.end()) {
    state.selected_imported_elements.erase(existing);
    if (state.selected_imported_elements.empty()) {
      state.selected_imported_debug = {ImportedDebugSelectionKind::Artwork,
                                       artwork_id, 0};
    } else {
      state.selected_imported_debug = {
          ImportedDebugSelectionKind::Path, artwork_id,
          state.selected_imported_elements.front().item_id};
    }
    return;
  }

  state.selected_imported_elements.push_back(
      {ImportedElementKind::Path, path_id});
  state.selected_imported_debug = {ImportedDebugSelectionKind::Path, artwork_id,
                                   path_id};
}

ImportedArtworkOperationResult ApplyImportedArtworkTransformToSelection(
    CanvasState &state, const int fallback_artwork_id,
    const char *operation_name,
    const std::function<bool(CanvasState &, int)> &operation) {
  ScopedUndoTransaction undo_transaction(state, operation_name);
  return ApplyImportedArtworkOperationToSelection(
      state, fallback_artwork_id, operation_name,
      [&operation_name, &operation](CanvasState &callback_state,
                                    const int target_artwork_id) {
        ImportedArtworkOperationResult result;
        result.artwork_id = target_artwork_id;
        result.success = operation(callback_state, target_artwork_id);
        result.message = callback_state.last_imported_artwork_operation.message;
        if (result.message.empty()) {
          result.message = std::string(operation_name) +
                           (result.success ? " completed." : " failed.");
        }
        return result;
      });
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

void BeginMarqueeInteraction(CanvasState &state,
                             TransientCanvasState *transient_state,
                             const ImVec2 &canvas_min,
                             const ImVec2 &mouse_screen,
                             const int preferred_artwork_id = 0) {
  ResetMarqueeInteractionState(transient_state);
  ClearActiveCanvasManipulation(transient_state);
  const int object_artwork_id =
      IsCanvasArtworkScope(state)
          ? 0
          : ResolveActiveImportedArtworkId(state, preferred_artwork_id);
  transient_state->marquee_state =
      IsCanvasArtworkScope(state)
          ? TransientCanvasState::MarqueeInteractionState::ArmedCanvasClear
          : (object_artwork_id != 0
                 ? TransientCanvasState::MarqueeInteractionState::
                       ArmedObjectClear
                 : TransientCanvasState::MarqueeInteractionState::
                       ArmedPendingObjectTarget);
  transient_state->marquee_artwork_id = object_artwork_id;
  transient_state->marquee_press_screen = mouse_screen;
  transient_state->marquee_start_world =
      ScreenToWorld(state, canvas_min, mouse_screen);
  transient_state->marquee_end_world = transient_state->marquee_start_world;
}

int FindFirstIntersectingImportedArtworkId(const CanvasState &state,
                                           const ImVec2 &world_start,
                                           const ImVec2 &world_end) {
  const ImRect selection_rect(ImVec2(std::min(world_start.x, world_end.x),
                                     std::min(world_start.y, world_end.y)),
                              ImVec2(std::max(world_start.x, world_end.x),
                                     std::max(world_start.y, world_end.y)));

  for (const ImportedArtwork &artwork : state.imported_artwork) {
    if (!artwork.visible) {
      continue;
    }

    ImRect artwork_world_rect;
    if (!TryGetImportedArtworkWorldRect(artwork, &artwork_world_rect)) {
      continue;
    }

    if (selection_rect.Overlaps(artwork_world_rect)) {
      return artwork.id;
    }
  }

  return 0;
}

int ResolvePendingObjectMarqueeTarget(const CanvasState &state,
                                      const ImportedPathHit &path_hit,
                                      const ImportedArtworkHit &artwork_hit,
                                      const ImVec2 &world_start,
                                      const ImVec2 &world_end) {
  if (path_hit.artwork_id != 0) {
    return path_hit.artwork_id;
  }
  if (artwork_hit.id != 0) {
    return artwork_hit.id;
  }
  return FindFirstIntersectingImportedArtworkId(state, world_start, world_end);
}

void PromoteArmedMarqueeToSelecting(TransientCanvasState *state) {
  switch (state->marquee_state) {
  case TransientCanvasState::MarqueeInteractionState::ArmedCanvasClear:
    state->marquee_state =
        TransientCanvasState::MarqueeInteractionState::SelectingCanvas;
    break;
  case TransientCanvasState::MarqueeInteractionState::ArmedObjectClear:
    state->marquee_state =
        TransientCanvasState::MarqueeInteractionState::SelectingObject;
    break;
  case TransientCanvasState::MarqueeInteractionState::ArmedPendingObjectTarget:
  case TransientCanvasState::MarqueeInteractionState::Idle:
  case TransientCanvasState::MarqueeInteractionState::SelectingCanvas:
  case TransientCanvasState::MarqueeInteractionState::SelectingObject:
    break;
  }
}

void ApplyMarqueeClickRelease(CanvasState &state,
                              TransientCanvasState *transient_state) {
  switch (transient_state->marquee_state) {
  case TransientCanvasState::MarqueeInteractionState::ArmedCanvasClear:
    ClearImportedSelectionForCurrentScope(state);
    break;
  case TransientCanvasState::MarqueeInteractionState::ArmedObjectClear:
    ClearImportedSelectionForCurrentScope(state,
                                          transient_state->marquee_artwork_id);
    break;
  case TransientCanvasState::MarqueeInteractionState::ArmedPendingObjectTarget:
  case TransientCanvasState::MarqueeInteractionState::Idle:
  case TransientCanvasState::MarqueeInteractionState::SelectingCanvas:
  case TransientCanvasState::MarqueeInteractionState::SelectingObject:
    break;
  }
}

void CommitMarqueeSelection(CanvasState &state,
                            const TransientCanvasState &transient_state) {
  switch (transient_state.marquee_state) {
  case TransientCanvasState::MarqueeInteractionState::SelectingCanvas:
    SelectImportedArtworkObjectsInWorldRect(
        state, transient_state.marquee_start_world,
        transient_state.marquee_end_world, state.imported_artwork_edit_mode);
    break;
  case TransientCanvasState::MarqueeInteractionState::SelectingObject:
    if (transient_state.marquee_artwork_id != 0) {
      SelectImportedPathsInWorldRect(state, transient_state.marquee_artwork_id,
                                     transient_state.marquee_start_world,
                                     transient_state.marquee_end_world,
                                     state.imported_artwork_edit_mode);
    }
    break;
  case TransientCanvasState::MarqueeInteractionState::ArmedPendingObjectTarget:
  case TransientCanvasState::MarqueeInteractionState::Idle:
  case TransientCanvasState::MarqueeInteractionState::ArmedCanvasClear:
  case TransientCanvasState::MarqueeInteractionState::ArmedObjectClear:
    break;
  }
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

struct PreviewOverlayColors {
  ImU32 stroke = 0;
  ImU32 fill = 0;
};

struct PreviewBucketRegion {
  int bucket_index = -1;
  int bucket_column = 0;
  int bucket_row = 0;
  PreviewOverlayColors colors;
};

ImU32 ThemeColorToU32(const ImVec4 &color) {
  return ImGui::ColorConvertFloat4ToU32(color);
}

PreviewOverlayColors MakePreviewOverlayColors(const ImVec4 &stroke,
                                              const ImVec4 &fill) {
  return {ThemeColorToU32(stroke), ThemeColorToU32(fill)};
}

bool ImportedIssueOverlayVisible(const ImportedIssueOverlaySettings &settings,
                                 const uint32_t issue_flags) {
  return (settings.show_ambiguous_cleanup &&
          HasImportedElementIssueFlag(
              issue_flags, ImportedElementIssueFlagAmbiguousCleanup)) ||
         (settings.show_orphan_hole &&
          HasImportedElementIssueFlag(issue_flags,
                                      ImportedElementIssueFlagOrphanHole)) ||
         (settings.show_placeholder_text &&
          HasImportedElementIssueFlag(
              issue_flags, ImportedElementIssueFlagPlaceholderText)) ||
         (settings.show_open_geometry &&
          HasImportedElementIssueFlag(issue_flags,
                                      ImportedElementIssueFlagOpenGeometry));
}

ImU32 ImportedIssueOverlayColor(const CanvasTheme &theme,
                                const ImportedIssueOverlaySettings &settings,
                                const uint32_t issue_flags) {
  if (settings.show_ambiguous_cleanup &&
      HasImportedElementIssueFlag(issue_flags,
                                  ImportedElementIssueFlagAmbiguousCleanup)) {
    return ThemeColorToU32(theme.imported_issue_ambiguous_cleanup);
  }
  if (settings.show_orphan_hole &&
      HasImportedElementIssueFlag(issue_flags,
                                  ImportedElementIssueFlagOrphanHole)) {
    return ThemeColorToU32(theme.imported_issue_orphan_hole);
  }
  if (settings.show_placeholder_text &&
      HasImportedElementIssueFlag(issue_flags,
                                  ImportedElementIssueFlagPlaceholderText)) {
    return ThemeColorToU32(theme.imported_issue_placeholder_text);
  }
  return ThemeColorToU32(theme.imported_issue_open_geometry);
}

PreviewOverlayColors GetSeparationPreviewColors(
    const CanvasTheme &theme,
    const ImportedSeparationPreviewClassification classification) {
  switch (classification) {
  case ImportedSeparationPreviewClassification::Assigned:
    return MakePreviewOverlayColors(theme.preview_assigned_stroke,
                                    theme.preview_assigned_fill);
  case ImportedSeparationPreviewClassification::Crossing:
    return MakePreviewOverlayColors(theme.preview_crossing_stroke,
                                    theme.preview_crossing_fill);
  case ImportedSeparationPreviewClassification::Orphan:
    return MakePreviewOverlayColors(theme.preview_orphan_stroke,
                                    theme.preview_orphan_fill);
  }
  return MakePreviewOverlayColors(theme.preview_default_stroke,
                                  theme.preview_default_fill);
}

std::vector<PreviewBucketRegion> BuildPreviewBucketRegions(
    const CanvasTheme &theme,
    const std::vector<ImportedSeparationPreviewPart> &parts) {
  std::vector<PreviewBucketRegion> bucket_regions;
  for (const ImportedSeparationPreviewPart &part : parts) {
    if (part.classification !=
            ImportedSeparationPreviewClassification::Assigned ||
        part.bucket_index < 0) {
      continue;
    }

    const bool already_added =
        std::any_of(bucket_regions.begin(), bucket_regions.end(),
                    [&part](const PreviewBucketRegion &region) {
                      return region.bucket_index == part.bucket_index;
                    });
    if (already_added) {
      continue;
    }

    PreviewOverlayColors colors =
        GetSeparationPreviewColors(theme, part.classification);
    const size_t palette_index = static_cast<size_t>(part.bucket_index) %
                                 theme.preview_bucket_strokes.size();
    colors.stroke =
        ThemeColorToU32(theme.preview_bucket_strokes[palette_index]);
    colors.fill = ThemeColorToU32(theme.preview_bucket_fills[palette_index]);
    bucket_regions.push_back(
        {part.bucket_index, part.bucket_column, part.bucket_row, colors});
  }
  return bucket_regions;
}

const char *
PreviewLabelForPart(ImportedSeparationPreviewClassification classification,
                    int bucket_index, std::string *scratch_label) {
  if (classification == ImportedSeparationPreviewClassification::Assigned) {
    *scratch_label = "B" + std::to_string(bucket_index + 1);
    return scratch_label->c_str();
  }
  if (classification == ImportedSeparationPreviewClassification::Crossing) {
    return "Crossing";
  }
  return "Orphan";
}

enum class ActiveCanvasNotification {
  None,
  GuideSplitPreview,
  AutoCutPreview,
  CallerNotification,
};

struct ActiveCanvasNotificationContent {
  ActiveCanvasNotification kind = ActiveCanvasNotification::None;
  const char *title = nullptr;
  std::string summary;
  CanvasNotificationDismissMode dismiss_mode =
      CanvasNotificationDismissMode::UserClosable;
  CanvasNotificationId notification_id = 0;
};

ActiveCanvasNotification
ResolveActiveCanvasNotification(const CanvasState &state) {
  if (state.imported_artwork_auto_cut_preview.active) {
    return ActiveCanvasNotification::AutoCutPreview;
  }
  if (state.imported_artwork_separation_preview.active) {
    return ActiveCanvasNotification::GuideSplitPreview;
  }
  if (state.canvas_notification.active) {
    return ActiveCanvasNotification::CallerNotification;
  }
  return ActiveCanvasNotification::None;
}

ActiveCanvasNotificationContent
BuildActiveCanvasNotificationContent(const CanvasState &state) {
  ActiveCanvasNotificationContent content;
  content.kind = ResolveActiveCanvasNotification(state);
  switch (content.kind) {
  case ActiveCanvasNotification::GuideSplitPreview:
    content.title = "Guide Split Preview Active";
    content.summary =
        std::to_string(
            state.imported_artwork_separation_preview.future_object_count) +
        " objects, " +
        std::to_string(
            state.imported_artwork_separation_preview.skipped_count) +
        " skipped";
    break;
  case ActiveCanvasNotification::AutoCutPreview:
    content.title = "Auto Cut Preview Active";
    content.summary =
        std::to_string(
            state.imported_artwork_auto_cut_preview.future_band_count) +
        " bands, " +
        std::to_string(state.imported_artwork_auto_cut_preview.skipped_count) +
        " skipped";
    break;
  case ActiveCanvasNotification::CallerNotification:
    content.title = state.canvas_notification.title.c_str();
    content.summary = state.canvas_notification.summary;
    content.dismiss_mode = state.canvas_notification.dismiss_mode;
    content.notification_id = state.canvas_notification.id;
    break;
  case ActiveCanvasNotification::None:
    break;
  }
  return content;
}

void DrawCallerNotification(ImDrawList *draw_list, const CanvasState &state,
                            const ImRect &canvas_rect) {
  const CanvasNotificationState &notification = state.canvas_notification;
  if (!notification.active) {
    return;
  }

  DrawCanvasNotificationBanner(
      draw_list,
      {.min = ImVec2(canvas_rect.Min.x + 12.0f, canvas_rect.Min.y + 12.0f)},
      CanvasNotificationBannerStyleFromTheme(state.theme),
      notification.title.c_str(), notification.summary,
      notification.dismiss_mode);
}

void DrawBandPreviewOverlay(
    ImDrawList *draw_list, const CanvasState &state, const ImRect &canvas_rect,
    int artwork_id, const std::vector<float> &vertical_positions,
    const std::vector<float> &horizontal_positions,
    const std::vector<ImportedSeparationPreviewPart> &parts, const char *title,
    const std::string &preview_summary, const bool show_notification,
    const CanvasNotificationDismissMode dismiss_mode) {
  draw_list->PushClipRect(canvas_rect.Min, canvas_rect.Max, true);
  if (show_notification) {
    DrawCanvasNotificationBanner(
        draw_list,
        {.min = ImVec2(canvas_rect.Min.x + 12.0f, canvas_rect.Min.y + 12.0f)},
        CanvasNotificationBannerStyleFromTheme(state.theme), title,
        preview_summary, dismiss_mode);
  }

  ImRect preview_artwork_rect = canvas_rect;
  if (const ImportedArtwork *artwork = FindImportedArtwork(state, artwork_id)) {
    preview_artwork_rect =
        ImportedArtworkScreenRect(state, canvas_rect.Min, *artwork);
    preview_artwork_rect.ClipWith(canvas_rect);
  }

  const std::vector<PreviewBucketRegion> bucket_regions =
      BuildPreviewBucketRegions(state.theme, parts);
  const ImVec2 visible_world_min =
      ScreenToWorld(state, canvas_rect.Min, canvas_rect.Min);
  const ImVec2 visible_world_max =
      ScreenToWorld(state, canvas_rect.Min, canvas_rect.Max);

  auto bucket_min_for = [](const std::vector<float> &positions, int index,
                           float fallback_min) {
    if (index <= 0 || positions.empty()) {
      return fallback_min;
    }
    return positions[static_cast<size_t>(index - 1)];
  };
  auto bucket_max_for = [](const std::vector<float> &positions, int index,
                           float fallback_max) {
    if (positions.empty() || index >= static_cast<int>(positions.size())) {
      return fallback_max;
    }
    return positions[static_cast<size_t>(index)];
  };

  for (const PreviewBucketRegion &region : bucket_regions) {
    const float world_left = bucket_min_for(
        vertical_positions, region.bucket_column, visible_world_min.x);
    const float world_right = bucket_max_for(
        vertical_positions, region.bucket_column, visible_world_max.x);
    const float world_top = bucket_min_for(
        horizontal_positions, region.bucket_row, visible_world_min.y);
    const float world_bottom = bucket_max_for(
        horizontal_positions, region.bucket_row, visible_world_max.y);
    const ImRect band_screen_rect =
        WorldRectToScreenRect(state, canvas_rect.Min,
                              ImRect(ImVec2(world_left, world_top),
                                     ImVec2(world_right, world_bottom)));
    ImRect clipped_band_rect = band_screen_rect;
    clipped_band_rect.ClipWith(preview_artwork_rect);
    if (!clipped_band_rect.IsInverted()) {
      draw_list->AddRectFilled(clipped_band_rect.Min, clipped_band_rect.Max,
                               region.colors.fill);
      draw_list->AddRect(clipped_band_rect.Min, clipped_band_rect.Max,
                         region.colors.stroke, 0.0f, 0, 2.0f);

      const std::string bucket_label =
          "Split " + std::to_string(region.bucket_index + 1);
      const ImVec2 label_pos(clipped_band_rect.Min.x + 10.0f,
                             clipped_band_rect.Min.y + 10.0f);
      draw_list->AddText(label_pos, region.colors.stroke, bucket_label.c_str());
    }
  }

  std::string label_scratch;
  for (const ImportedSeparationPreviewPart &part : parts) {
    PreviewOverlayColors colors =
        GetSeparationPreviewColors(state.theme, part.classification);
    if (part.classification ==
        ImportedSeparationPreviewClassification::Assigned) {
      continue;
    }

    const ImRect screen_rect = WorldRectToScreenRect(
        state, canvas_rect.Min,
        ImRect(part.world_bounds_min, part.world_bounds_max));
    draw_list->AddRect(screen_rect.Min, screen_rect.Max, colors.stroke, 4.0f, 0,
                       3.0f);

    const char *label = PreviewLabelForPart(part.classification,
                                            part.bucket_index, &label_scratch);
    const ImVec2 label_min(screen_rect.Min.x + 4.0f, screen_rect.Min.y + 4.0f);
    const ImVec2 label_max(label_min.x + 72.0f, label_min.y + 20.0f);
    draw_list->AddRectFilled(
        label_min, label_max,
        ThemeColorToU32(state.theme.preview_label_background), 4.0f);
    draw_list->AddRect(label_min, label_max, colors.stroke, 4.0f, 0, 1.5f);
    draw_list->AddText(ImVec2(label_min.x + 6.0f, label_min.y + 3.0f),
                       colors.stroke, label);
  }
  draw_list->PopClipRect();
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

int DecimalPlacesForStep(float step_units) {
  if (step_units <= 0.0f) {
    return 0;
  }

  float normalized = std::fabs(step_units);
  int decimal_places = 0;
  while (decimal_places < 6 &&
         std::fabs(normalized - std::round(normalized)) > 0.0001f) {
    normalized *= 10.0f;
    ++decimal_places;
  }

  return decimal_places;
}

std::string FormatRulerTickLabel(float tick_units, float step_units) {
  const int decimal_places = DecimalPlacesForStep(step_units);
  char label[32];
  std::snprintf(label, sizeof(label), "%.*f", decimal_places, tick_units);

  std::string formatted(label);
  if (formatted.find('.') != std::string::npos) {
    while (!formatted.empty() && formatted.back() == '0') {
      formatted.pop_back();
    }
    if (!formatted.empty() && formatted.back() == '.') {
      formatted.pop_back();
    }
  }

  if (formatted == "-0") {
    return "0";
  }

  return formatted;
}

float RulerDirection(const CanvasState &state, bool horizontal) {
  const float direction = horizontal
                              ? state.ruler_reference.horizontal_direction
                              : state.ruler_reference.vertical_direction;
  return std::abs(direction) < 1e-6f ? 1.0f : direction;
}

float RulerOriginWorld(const CanvasState &state, bool horizontal) {
  return horizontal ? state.ruler_reference.origin_world.x
                    : state.ruler_reference.origin_world.y;
}

float TransformRulerWorld(const CanvasState &state, float world,
                          bool horizontal) {
  if (!state.ruler_reference.enabled) {
    return world;
  }
  return (world - RulerOriginWorld(state, horizontal)) *
         RulerDirection(state, horizontal);
}

float InverseTransformRulerWorld(const CanvasState &state, float world,
                                 bool horizontal) {
  if (!state.ruler_reference.enabled) {
    return world;
  }
  return RulerOriginWorld(state, horizontal) +
         (world / RulerDirection(state, horizontal));
}

float ComputeLeftRulerThickness(const CanvasState &state,
                                const ImRect &total_rect,
                                float top_ruler_thickness,
                                float minimum_thickness) {
  const float pixels_per_unit =
      UnitsToPixels(1.0f, state.ruler_unit, state.calibration) *
      state.view.zoom;
  if (pixels_per_unit <= 0.0f) {
    return minimum_thickness;
  }

  const float major_step_units = NiceStep(80.0f / pixels_per_unit);
  const float minor_step_units = major_step_units / 5.0f;
  if (major_step_units <= 0.0f || minor_step_units <= 0.0f) {
    return minimum_thickness;
  }

  const ImVec2 canvas_min(total_rect.Min.x + minimum_thickness,
                          total_rect.Min.y + top_ruler_thickness);
  const float visible_min_world =
      ScreenToWorld(state, canvas_min, ImVec2(canvas_min.x, canvas_min.y)).y;
  const float visible_max_world =
      ScreenToWorld(state, canvas_min, ImVec2(canvas_min.x, total_rect.Max.y))
          .y;

  const float min_units =
      PixelsToUnits(TransformRulerWorld(state, visible_min_world, false),
                    state.ruler_unit, state.calibration);
  const float max_units =
      PixelsToUnits(TransformRulerWorld(state, visible_max_world, false),
                    state.ruler_unit, state.calibration);
  const float start_units =
      std::floor(std::min(min_units, max_units) / minor_step_units) *
      minor_step_units;
  const float end_units =
      std::ceil(std::max(min_units, max_units) / minor_step_units) *
      minor_step_units;

  float max_label_width = 0.0f;
  for (float tick_units = start_units; tick_units <= end_units;
       tick_units += minor_step_units) {
    const float remainder = std::fmod(std::fabs(tick_units), major_step_units);
    const bool major = remainder < 0.0001f ||
                       std::fabs(remainder - major_step_units) < 0.0001f;
    if (!major) {
      continue;
    }

    const std::string label =
        FormatRulerTickLabel(tick_units, major_step_units);
    max_label_width =
        std::max(max_label_width, ImGui::CalcTextSize(label.c_str()).x);
  }

  constexpr float kRulerLabelPadding = 4.0f;
  return std::max(minimum_thickness,
                  max_label_width + kRulerLabelPadding * 2.0f);
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

void DrawSeparationPreviewOverlay(
    ImDrawList *draw_list, const CanvasState &state, const ImRect &canvas_rect,
    const bool show_notification,
    const CanvasNotificationDismissMode dismiss_mode) {
  const ImportedArtworkSeparationPreview &preview =
      state.imported_artwork_separation_preview;
  if (!preview.active) {
    return;
  }

  const std::string preview_summary =
      std::to_string(preview.future_object_count) + " objects, " +
      std::to_string(preview.skipped_count) + " skipped";

  std::vector<float> vertical_positions;
  std::vector<float> horizontal_positions;
  for (const int guide_id : preview.guide_ids) {
    const Guide *guide = FindGuide(state, guide_id);
    if (guide == nullptr) {
      continue;
    }
    if (guide->orientation == GuideOrientation::Vertical) {
      vertical_positions.push_back(guide->position);
    } else {
      horizontal_positions.push_back(guide->position);
    }
  }
  std::sort(vertical_positions.begin(), vertical_positions.end());
  std::sort(horizontal_positions.begin(), horizontal_positions.end());

  DrawBandPreviewOverlay(draw_list, state, canvas_rect, preview.artwork_id,
                         vertical_positions, horizontal_positions,
                         preview.parts, "Guide Split Preview Active",
                         preview_summary, show_notification, dismiss_mode);
}

void DrawAutoCutPreviewOverlay(
    ImDrawList *draw_list, const CanvasState &state, const ImRect &canvas_rect,
    const bool show_notification,
    const CanvasNotificationDismissMode dismiss_mode) {
  const ImportedArtworkAutoCutPreview &preview =
      state.imported_artwork_auto_cut_preview;
  if (!preview.active) {
    return;
  }

  const std::string preview_summary =
      std::to_string(preview.future_band_count) + " bands, " +
      std::to_string(preview.skipped_count) + " skipped";
  DrawBandPreviewOverlay(draw_list, state, canvas_rect, preview.artwork_id,
                         preview.vertical_positions,
                         preview.horizontal_positions, preview.parts,
                         "Auto Cut Preview Active", preview_summary,
                         show_notification, dismiss_mode);
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

bool PreviewPointsNear(const ImVec2 &a, const ImVec2 &b) {
  const float dx = a.x - b.x;
  const float dy = a.y - b.y;
  return dx * dx + dy * dy <= 1.0f;
}

void AppendPreviewPathPoint(std::vector<ImVec2> *points, const ImVec2 &point) {
  if (!points->empty() && PreviewPointsNear(points->back(), point)) {
    return;
  }
  points->push_back(point);
}

float DistancePointToLineSquared(const ImVec2 &point, const ImVec2 &line_start,
                                 const ImVec2 &line_end) {
  const float dx = line_end.x - line_start.x;
  const float dy = line_end.y - line_start.y;
  const float line_length_squared = dx * dx + dy * dy;
  if (line_length_squared <= 0.000001f) {
    const float px = point.x - line_start.x;
    const float py = point.y - line_start.y;
    return px * px + py * py;
  }

  const float t = std::clamp(
      ((point.x - line_start.x) * dx + (point.y - line_start.y) * dy) /
          line_length_squared,
      0.0f, 1.0f);
  const float projection_x = line_start.x + t * dx;
  const float projection_y = line_start.y + t * dy;
  const float px = point.x - projection_x;
  const float py = point.y - projection_y;
  return px * px + py * py;
}

bool ImportedPreviewCurveFlatEnough(const ImVec2 &start, const ImVec2 &control1,
                                    const ImVec2 &control2, const ImVec2 &end) {
  const float tolerance_squared =
      kImportedPreviewCurveFlatnessPixels * kImportedPreviewCurveFlatnessPixels;
  return DistancePointToLineSquared(control1, start, end) <=
             tolerance_squared &&
         DistancePointToLineSquared(control2, start, end) <= tolerance_squared;
}

void AppendAdaptiveImportedPreviewCurvePoints(std::vector<ImVec2> *points,
                                              const ImVec2 &start,
                                              const ImVec2 &control1,
                                              const ImVec2 &control2,
                                              const ImVec2 &end, int depth) {
  if (depth >= kImportedPreviewCurveMaxSubdivisionDepth ||
      ImportedPreviewCurveFlatEnough(start, control1, control2, end)) {
    AppendPreviewPathPoint(points, end);
    return;
  }

  const ImVec2 start_control1_mid((start.x + control1.x) * 0.5f,
                                  (start.y + control1.y) * 0.5f);
  const ImVec2 control1_control2_mid((control1.x + control2.x) * 0.5f,
                                     (control1.y + control2.y) * 0.5f);
  const ImVec2 control2_end_mid((control2.x + end.x) * 0.5f,
                                (control2.y + end.y) * 0.5f);
  const ImVec2 left_control2(
      (start_control1_mid.x + control1_control2_mid.x) * 0.5f,
      (start_control1_mid.y + control1_control2_mid.y) * 0.5f);
  const ImVec2 right_control1(
      (control1_control2_mid.x + control2_end_mid.x) * 0.5f,
      (control1_control2_mid.y + control2_end_mid.y) * 0.5f);
  const ImVec2 split_point((left_control2.x + right_control1.x) * 0.5f,
                           (left_control2.y + right_control1.y) * 0.5f);

  AppendAdaptiveImportedPreviewCurvePoints(
      points, start, start_control1_mid, left_control2, split_point, depth + 1);
  AppendAdaptiveImportedPreviewCurvePoints(points, split_point, right_control1,
                                           control2_end_mid, end, depth + 1);
}

void AppendImportedSegmentPath(ImDrawList *draw_list, const CanvasState &state,
                               const ImRect &canvas_rect,
                               const ImportedArtwork &artwork,
                               const std::vector<ImportedPathSegment> &segments,
                               bool closed) {
  if (segments.empty()) {
    return;
  }

  std::vector<ImVec2> screen_points;
  screen_points.reserve(segments.size() * 4);
  const ImVec2 first_world =
      ImportedArtworkPointToWorld(artwork, segments.front().start);
  AppendPreviewPathPoint(&screen_points,
                         WorldToScreen(state, canvas_rect.Min, first_world));

  for (const ImportedPathSegment &segment : segments) {
    if (segment.kind == ImportedPathSegmentKind::Line) {
      const ImVec2 end_world =
          ImportedArtworkPointToWorld(artwork, segment.end);
      AppendPreviewPathPoint(&screen_points,
                             WorldToScreen(state, canvas_rect.Min, end_world));
      continue;
    }

    const ImVec2 start_screen =
        WorldToScreen(state, canvas_rect.Min,
                      ImportedArtworkPointToWorld(artwork, segment.start));
    const ImVec2 control1_screen =
        WorldToScreen(state, canvas_rect.Min,
                      ImportedArtworkPointToWorld(artwork, segment.control1));
    const ImVec2 control2_screen =
        WorldToScreen(state, canvas_rect.Min,
                      ImportedArtworkPointToWorld(artwork, segment.control2));
    const ImVec2 end_screen =
        WorldToScreen(state, canvas_rect.Min,
                      ImportedArtworkPointToWorld(artwork, segment.end));
    AppendAdaptiveImportedPreviewCurvePoints(&screen_points, start_screen,
                                             control1_screen, control2_screen,
                                             end_screen, 0);
  }

  if (closed && screen_points.size() > 1 &&
      PreviewPointsNear(screen_points.front(), screen_points.back())) {
    screen_points.pop_back();
  }

  draw_list->PathClear();
  for (const ImVec2 &point : screen_points) {
    draw_list->PathLineTo(point);
  }
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
  AppendImportedSegmentPath(draw_list, state, canvas_rect, artwork,
                            contour.segments, contour.closed);
}

void DrawImportedDxfText(ImDrawList *draw_list, const CanvasState &state,
                         const ImRect &canvas_rect,
                         const ImportedArtwork &artwork,
                         const ImportedDxfText &text) {
  const ImU32 packed_color = ImGui::ColorConvertFloat4ToU32(text.stroke_color);
  const float outline_thickness = kImportedPreviewStrokeWidth;

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
  const ImU32 operation_issue_color =
      ThemeColorToU32(state.theme.operation_issue);
  const bool show_group_selection_bounds =
      IsCanvasArtworkScope(state) &&
      CountSelectedImportedArtworkObjects(state) > 1;

  for (const ImportedArtwork &artwork : state.imported_artwork) {
    if (!artwork.visible) {
      continue;
    }

    const bool artwork_selected =
        IsImportedArtworkObjectSelected(state, artwork.id);
    const bool show_issue_overlays =
        artwork_selected || artwork.id == selected_imported_artwork_id ||
        state.selected_imported_debug.artwork_id == artwork.id;

    for (const ImportedDxfText &text : artwork.dxf_text) {
      if (text.placeholder_only && artwork.source_format == "DXF" &&
          !state.show_imported_dxf_text) {
        continue;
      }
      DrawImportedDxfText(draw_list, state, canvas_rect, artwork, text);
      const bool has_visible_issue_overlay =
          show_issue_overlays &&
          ImportedIssueOverlayVisible(state.imported_issue_overlays,
                                      text.issue_flags);
      if (has_visible_issue_overlay) {
        const ImRect text_rect = ImportedElementScreenRect(
            state, canvas_rect.Min, artwork, text.bounds_min, text.bounds_max);
        draw_list->AddRect(
            text_rect.Min, text_rect.Max,
            ImportedIssueOverlayColor(
                state.theme, state.imported_issue_overlays, text.issue_flags),
            3.0f, 0, 2.0f);
      }
      if (state.highlight_last_imported_operation_issue_elements &&
          show_issue_overlays && !has_visible_issue_overlay &&
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

      const auto append_path = [&]() {
        AppendImportedSegmentPath(draw_list, state, canvas_rect, artwork,
                                  path.segments, path.closed);
      };

      const bool is_filled_text =
          HasImportedPathFlag(path.flags, ImportedPathFlagFilledText);
      const bool is_hole_contour =
          HasImportedPathFlag(path.flags, ImportedPathFlagHoleContour);
      const float thickness = kImportedPreviewStrokeWidth;
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

      const bool has_visible_issue_overlay =
          show_issue_overlays &&
          ImportedIssueOverlayVisible(state.imported_issue_overlays,
                                      path.issue_flags);
      if (has_visible_issue_overlay) {
        const ImRect path_rect = ImportedElementScreenRect(
            state, canvas_rect.Min, artwork, path.bounds_min, path.bounds_max);
        draw_list->AddRect(
            path_rect.Min, path_rect.Max,
            ImportedIssueOverlayColor(
                state.theme, state.imported_issue_overlays, path.issue_flags),
            3.0f, 0, 2.0f);
      }
      if (state.highlight_last_imported_operation_issue_elements &&
          show_issue_overlays && !has_visible_issue_overlay &&
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
    if (!show_group_selection_bounds && (artwork_selected || has_debug_focus)) {
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

  if (show_group_selection_bounds) {
    ImRect selection_screen_rect;
    if (TryGetSelectedImportedArtworkScreenRect(state, canvas_rect.Min,
                                                &selection_screen_rect)) {
      draw_list->AddRect(selection_screen_rect.Min, selection_screen_rect.Max,
                         selected_color, 4.0f, 0, 2.0f);
      const ImRect handle_rect = ImportedArtworkResizeHandleRect(
          selection_screen_rect, options.resize_handle_size);
      draw_list->AddRectFilled(handle_rect.Min, handle_rect.Max, selected_color,
                               2.0f);
    }
  }

  draw_list->PopClipRect();
}

void DrawImportedMarquee(ImDrawList *draw_list, const CanvasState &state,
                         const ImRect &canvas_rect,
                         const TransientCanvasState &transient_state) {
  if (!IsMarqueeInteractionSelecting(transient_state)) {
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

  const float w = marquee_screen_rect.Max.x - marquee_screen_rect.Min.x;
  const float h = marquee_screen_rect.Max.y - marquee_screen_rect.Min.y;
  if (w < 1.0f && h < 1.0f) {
    return;
  }

  draw_list->PushClipRect(canvas_rect.Min, canvas_rect.Max, true);

  const ImU32 outline_color =
      ImGui::ColorConvertFloat4ToU32(state.theme.guide_hovered);
  const ImU32 fill_color = ImGui::ColorConvertFloat4ToU32(
      ImVec4(state.theme.guide_hovered.x, state.theme.guide_hovered.y,
             state.theme.guide_hovered.z, kMarqueeFillAlpha));

  if (state.imported_artwork_edit_mode == ImportedArtworkEditMode::SelectOval) {
    const ImVec2 center(
        (marquee_screen_rect.Min.x + marquee_screen_rect.Max.x) * 0.5f,
        (marquee_screen_rect.Min.y + marquee_screen_rect.Max.y) * 0.5f);
    const ImVec2 radius(
        (marquee_screen_rect.Max.x - marquee_screen_rect.Min.x) * 0.5f,
        (marquee_screen_rect.Max.y - marquee_screen_rect.Min.y) * 0.5f);
    draw_list->AddEllipseFilled(center, radius, fill_color);
    draw_list->AddEllipse(center, radius, outline_color, 0.0f, 0,
                          kMarqueeOutlineThickness);
    draw_list->PopClipRect();
    return;
  }

  draw_list->AddRectFilled(marquee_screen_rect.Min, marquee_screen_rect.Max,
                           fill_color, 3.0f);
  draw_list->AddRect(marquee_screen_rect.Min, marquee_screen_rect.Max,
                     outline_color, 3.0f, 0, kMarqueeOutlineThickness);
  draw_list->PopClipRect();
}

void DrawWorkingAreas(ImDrawList *draw_list, const CanvasState &state,
                      const ImRect &canvas_rect, int selected_working_area_id,
                      const CanvasWidgetOptions &options) {
  draw_list->PushClipRect(canvas_rect.Min, canvas_rect.Max, true);

  const ImU32 fill_color =
      ImGui::ColorConvertFloat4ToU32(state.theme.working_area_fill);
  const ImU32 exclusion_fill =
      ImGui::ColorConvertFloat4ToU32(state.theme.exclusion_area_fill);
  const ImU32 exclusion_outline =
      ImGui::ColorConvertFloat4ToU32(state.theme.exclusion_area_outline);
  const ImU32 exclusion_selected =
      ImGui::ColorConvertFloat4ToU32(state.theme.exclusion_area_selected);

  for (const WorkingArea &area : state.working_areas) {
    if (!area.visible) {
      continue;
    }

    const ImRect screen_rect =
        WorkingAreaScreenRect(state, canvas_rect.Min, area);
    const bool selected = area.id == selected_working_area_id;
    const ImU32 active_border = ImGui::ColorConvertFloat4ToU32(
        selected ? area.selected_border_color : area.border_color);
    const float thickness =
        selected ? area.selected_outline_thickness : area.outline_thickness;

    constexpr float corner_radius = 0.0f;
    draw_list->AddRectFilled(screen_rect.Min, screen_rect.Max, fill_color,
                             corner_radius);
    draw_list->AddRect(screen_rect.Min, screen_rect.Max, active_border,
                       corner_radius, 0, thickness);
    draw_list->AddText(
        ImVec2(screen_rect.Min.x + 8.0f, screen_rect.Min.y + 8.0f),
        active_border, area.name.c_str());

    constexpr float handle_radius = 2.0f;
    if (selected && HasWorkingAreaFlag(area.flags, WorkingAreaFlagResizable)) {
      const ImRect handle_rect(
          ImVec2(screen_rect.Max.x - options.resize_handle_size,
                 screen_rect.Max.y - options.resize_handle_size),
          screen_rect.Max);
      draw_list->AddRectFilled(handle_rect.Min, handle_rect.Max, active_border,
                               handle_radius);
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
    if (!area.hide_fill) {
      draw_list->AddRectFilled(screen_rect.Min, screen_rect.Max,
                               ImGui::ColorConvertFloat4ToU32(area.fill_color),
                               0.0f);
    }
    draw_list->AddRect(screen_rect.Min, screen_rect.Max,
                       ImGui::ColorConvertFloat4ToU32(area.outline_color), 0.0f,
                       0, 1.0f);
  }

  for (const ExclusionArea &area : state.exclusion_areas) {
    if (!area.visible) {
      continue;
    }

    const ImRect screen_rect(
        WorldToScreen(state, canvas_rect.Min, area.origin),
        WorldToScreen(
            state, canvas_rect.Min,
            ImVec2(area.origin.x + area.size.x, area.origin.y + area.size.y)));
    if (!area.hide_fill) {
      draw_list->AddRectFilled(screen_rect.Min, screen_rect.Max, exclusion_fill,
                               0.0f);
    }
    draw_list->AddRect(screen_rect.Min, screen_rect.Max,
                       area.selected ? exclusion_selected : exclusion_outline,
                       0.0f, 0, area.selected ? 2.0f : 1.0f);
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

  draw_list->PushClipRect(rect.Min, rect.Max, true);

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
      PixelsToUnits(TransformRulerWorld(state, visible_min_world, horizontal),
                    state.ruler_unit, state.calibration);
  const float max_units =
      PixelsToUnits(TransformRulerWorld(state, visible_max_world, horizontal),
                    state.ruler_unit, state.calibration);
  const float start_units =
      std::floor(std::min(min_units, max_units) / minor_step_units) *
      minor_step_units;
  const float end_units =
      std::ceil(std::max(min_units, max_units) / minor_step_units) *
      minor_step_units;

  for (float tick_units = start_units; tick_units <= end_units;
       tick_units += minor_step_units) {
    const float display_tick_world =
        UnitsToPixels(tick_units, state.ruler_unit, state.calibration);
    const float tick_world =
        InverseTransformRulerWorld(state, display_tick_world, horizontal);
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
        const std::string label =
            FormatRulerTickLabel(tick_units, major_step_units);
        draw_list->AddText(ImVec2(x + 4.0f, rect.Min.y + 4.0f), text_color,
                           label.c_str());
      }
    } else {
      const float y = WorldToScreen(state, ImVec2(rect.Max.x, rect.Min.y),
                                    ImVec2(0.0f, tick_world))
                          .y;
      draw_list->AddLine(ImVec2(rect.Max.x, y),
                         ImVec2(rect.Max.x - tick_length, y), tick_color, 1.0f);
      if (major) {
        const std::string label =
            FormatRulerTickLabel(tick_units, major_step_units);
        const float label_width = ImGui::CalcTextSize(label.c_str()).x;
        const float label_x =
            std::max(rect.Min.x + 4.0f, rect.Max.x - label_width - 4.0f);
        draw_list->AddText(ImVec2(label_x, y + 2.0f), text_color,
                           label.c_str());
      }
    }
  }

  draw_list->PopClipRect();
}

} // namespace

bool DrawCanvas(CanvasState &state, const CanvasWidgetOptions &options) {
  InitializeDefaultDocument(state, options.ensure_default_working_area);
  TransientCanvasState &transient_state = GetTransientCanvasState();
  ImGuiIO &io = ImGui::GetIO();
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
      transient_state.last_selection_scope != state.selection_scope) {
    ApplyImportedArtworkSelectionScope(state, state.selection_scope);
    ClearActiveCanvasManipulation(&transient_state);
    if (!IsMarqueeInteractionActive(transient_state)) {
      ResetMarqueeInteractionState(&transient_state);
    }
    transient_state.last_selection_scope = state.selection_scope;
  }
  const int selected_imported_artwork_count =
      CountSelectedImportedArtworkObjects(state);

  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
      (transient_state.last_selected_imported_artwork_id !=
           state.selected_imported_artwork_id ||
       transient_state.last_selected_imported_artwork_count !=
           selected_imported_artwork_count)) {
    if (!IsMarqueeInteractionActive(transient_state)) {
      ResetMarqueeInteractionState(&transient_state);
    }
    transient_state.last_selected_imported_artwork_id =
        state.selected_imported_artwork_id;
    transient_state.last_selected_imported_artwork_count =
        selected_imported_artwork_count;
  }

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
  const float top_ruler_thickness = options.ruler_thickness;
  const float left_ruler_thickness = ComputeLeftRulerThickness(
      state, total_rect, top_ruler_thickness, options.ruler_thickness);
  const ImRect top_ruler_rect(
      ImVec2(total_rect.Min.x + left_ruler_thickness, total_rect.Min.y),
      ImVec2(total_rect.Max.x, total_rect.Min.y + top_ruler_thickness));
  const ImRect left_ruler_rect(
      ImVec2(total_rect.Min.x, total_rect.Min.y + top_ruler_thickness),
      ImVec2(total_rect.Min.x + left_ruler_thickness, total_rect.Max.y));
  const ImRect corner_rect(total_rect.Min,
                           ImVec2(total_rect.Min.x + left_ruler_thickness,
                                  total_rect.Min.y + top_ruler_thickness));
  const ImRect canvas_rect(ImVec2(total_rect.Min.x + left_ruler_thickness,
                                  total_rect.Min.y + top_ruler_thickness),
                           total_rect.Max);
  state.runtime.valid = true;
  state.runtime.total_min = total_rect.Min;
  state.runtime.total_max = total_rect.Max;
  state.runtime.canvas_min = canvas_rect.Min;
  state.runtime.canvas_max = canvas_rect.Max;

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

  const bool any_popup_open =
      ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);

  const bool canvas_hovered = canvas_rect.Contains(io.MousePos);
  const ActiveCanvasNotificationContent notification_content =
      BuildActiveCanvasNotificationContent(state);
  const ActiveCanvasNotification active_notification =
      notification_content.kind;
  const bool notification_visible =
      active_notification != ActiveCanvasNotification::None;
  const CanvasNotificationBannerLayout notification_layout = {
      .min = ImVec2(canvas_rect.Min.x + 12.0f, canvas_rect.Min.y + 12.0f)};
  const CanvasNotificationBannerStyle notification_style =
      CanvasNotificationBannerStyleFromTheme(state.theme);
  const CanvasNotificationDismissMode notification_dismiss_mode =
      notification_content.dismiss_mode;
  const CanvasNotificationBannerLayout resolved_notification_layout =
      notification_visible
          ? ResolveCanvasNotificationBannerLayout(
                notification_layout, notification_style,
                notification_content.title, notification_content.summary,
                notification_dismiss_mode)
          : notification_layout;
  const bool notification_hovered =
      notification_visible &&
      CanvasNotificationBannerContainsPoint(
          resolved_notification_layout, notification_style,
          notification_content.title, notification_content.summary,
          notification_dismiss_mode, io.MousePos);
  const bool notification_close_clicked =
      !any_popup_open && notification_visible &&
      CanvasNotificationBannerCloseButtonContainsPoint(
          resolved_notification_layout, notification_style,
          notification_content.title, notification_content.summary,
          notification_dismiss_mode, io.MousePos) &&
      ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  if (notification_close_clicked) {
    if (active_notification == ActiveCanvasNotification::AutoCutPreview) {
      ClearImportedArtworkAutoCutPreview(state);
    } else if (active_notification ==
               ActiveCanvasNotification::GuideSplitPreview) {
      ClearImportedArtworkSeparationPreview(state);
    } else if (active_notification ==
               ActiveCanvasNotification::CallerNotification) {
      DismissCanvasNotification(state, notification_content.notification_id);
    }
  }
  const bool canvas_input_hovered = canvas_hovered && !notification_hovered;
  if (canvas_hovered) {
    state.runtime.has_cursor_world = true;
    state.runtime.cursor_world =
        ScreenToWorld(state, canvas_rect.Min, io.MousePos);
  }
  const bool top_ruler_hovered = top_ruler_rect.Contains(io.MousePos);
  const bool left_ruler_hovered = left_ruler_rect.Contains(io.MousePos);
  const ImportedArtworkHit imported_artwork_hit =
      canvas_input_hovered
          ? FindHoveredImportedArtwork(state, canvas_rect.Min, canvas_rect,
                                       io.MousePos, options.resize_handle_size)
          : ImportedArtworkHit{};
  const ImportedPathHit imported_path_hit =
      !canvas_input_hovered || IsCanvasArtworkScope(state)
          ? ImportedPathHit{}
          : FindHoveredImportedPath(state, canvas_rect.Min, canvas_rect,
                                    io.MousePos, 6.0f);
  ImRect selected_imported_artwork_group_rect;
  ImportedArtworkHitZone selected_imported_artwork_group_zone =
      ImportedArtworkHitZone::None;
  if (IsCanvasArtworkScope(state) &&
      CountSelectedImportedArtworkObjects(state) > 1 &&
      TryGetSelectedImportedArtworkScreenRect(
          state, canvas_rect.Min, &selected_imported_artwork_group_rect)) {
    const ImRect handle_rect = ImportedArtworkResizeHandleRect(
        selected_imported_artwork_group_rect, options.resize_handle_size);
    if (handle_rect.Contains(io.MousePos)) {
      selected_imported_artwork_group_zone =
          ImportedArtworkHitZone::ResizeHandle;
    } else if (selected_imported_artwork_group_rect.Contains(io.MousePos)) {
      selected_imported_artwork_group_zone = ImportedArtworkHitZone::Body;
    }
  }
  const WorkingAreaHit area_hit =
      canvas_input_hovered
          ? FindHoveredWorkingArea(state, canvas_rect.Min, canvas_rect,
                                   io.MousePos, options.resize_handle_size)
          : WorkingAreaHit{};
  const bool marquee_mode_active =
      state.imported_artwork_edit_mode != ImportedArtworkEditMode::None;

  int hovered_guide_id = 0;
  if (canvas_input_hovered && !transient_state.creating_guide) {
    hovered_guide_id =
        FindHoveredGuide(state, canvas_rect.Min, canvas_rect, io.MousePos);
  }

  if (!any_popup_open && canvas_input_hovered && io.MouseWheel != 0.0f) {
    const ImVec2 focus_world =
        ScreenToWorld(state, canvas_rect.Min, io.MousePos);
    state.view.zoom =
        ClampZoom(state.view.zoom * std::pow(1.15f, io.MouseWheel));
    state.view.pan = ImVec2(
        io.MousePos.x - canvas_rect.Min.x - focus_world.x * state.view.zoom,
        io.MousePos.y - canvas_rect.Min.y - focus_world.y * state.view.zoom);
  }

  if (!any_popup_open && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    transient_state.right_mouse_pressed_in_canvas = canvas_input_hovered;
    transient_state.right_mouse_dragged = false;
  }

  if (!any_popup_open && transient_state.right_mouse_pressed_in_canvas &&
      ImGui::IsMouseDragging(ImGuiMouseButton_Right,
                             kRightDragStartDistancePixels)) {
    state.view.pan.x += io.MouseDelta.x;
    state.view.pan.y += io.MouseDelta.y;
    transient_state.right_mouse_dragged = true;
  }

  if (!any_popup_open && !transient_state.creating_guide &&
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

  if (!any_popup_open && !transient_state.creating_guide &&
      canvas_input_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    if (hovered_guide_id != 0) {
      if (Guide *guide = FindGuide(state, hovered_guide_id);
          guide != nullptr && !guide->locked) {
        ResetMarqueeInteractionState(&transient_state);
        transient_state.dragging_guide_id = hovered_guide_id;
      }
    } else if (selected_imported_artwork_group_zone ==
                   ImportedArtworkHitZone::ResizeHandle &&
               !marquee_mode_active) {
      BeginSelectedImportedArtworkResize(state, &transient_state);
    } else if (selected_imported_artwork_group_zone ==
                   ImportedArtworkHitZone::Body &&
               imported_artwork_hit.id == 0 && !marquee_mode_active) {
      if (ImportedArtwork *artwork =
              FindImportedArtwork(state, state.selected_imported_artwork_id);
          artwork != nullptr &&
          HasImportedArtworkFlag(artwork->flags, ImportedArtworkFlagMovable)) {
        const ImVec2 world = ScreenToWorld(state, canvas_rect.Min, io.MousePos);
        BeginImportedArtworkDrag(state, &transient_state,
                                 state.selected_imported_artwork_id, world);
      }
    } else if (!IsCanvasArtworkScope(state) && imported_path_hit.path_id != 0) {
      const bool additive_selection = IsImportedArtworkSelectionModifierDown();
      if (marquee_mode_active) {
        if (ResolveActiveImportedArtworkId(state) == 0) {
          SelectImportedPathForObjectScope(
              state, &transient_state, imported_path_hit.artwork_id,
              imported_path_hit.path_id, additive_selection);
        } else {
          BeginMarqueeInteraction(state, &transient_state, canvas_rect.Min,
                                  io.MousePos, imported_path_hit.artwork_id);
        }
      } else {
        SelectImportedPathForObjectScope(
            state, &transient_state, imported_path_hit.artwork_id,
            imported_path_hit.path_id, additive_selection);
      }
    } else if (imported_artwork_hit.id != 0) {
      const bool artwork_selection_modifier_down =
          IsImportedArtworkSelectionModifierDown();
      if (!IsCanvasArtworkScope(state)) {
        if (marquee_mode_active) {
          BeginMarqueeInteraction(state, &transient_state, canvas_rect.Min,
                                  io.MousePos, imported_artwork_hit.id);
        } else {
          SetSingleSelectedImportedArtworkObject(state,
                                                 imported_artwork_hit.id);
          ClearSelectedImportedElements(state);
          state.selected_imported_debug = {ImportedDebugSelectionKind::Artwork,
                                           imported_artwork_hit.id, 0};
          state.selected_working_area_id = 0;
          ResetMarqueeInteractionState(&transient_state);
        }
      } else if (marquee_mode_active) {
        BeginMarqueeInteraction(state, &transient_state, canvas_rect.Min,
                                io.MousePos);
      } else {
        if (artwork_selection_modifier_down) {
          state.selected_working_area_id = 0;
          ClearSelectedImportedElements(state);
          ResetMarqueeInteractionState(&transient_state);
          if (IsImportedArtworkObjectSelected(state, imported_artwork_hit.id)) {
            RemoveSelectedImportedArtworkObject(state, imported_artwork_hit.id);
            if (state.selected_imported_artwork_id == 0) {
              ClearImportedDebugSelection(state);
            } else {
              state.selected_imported_debug = {
                  ImportedDebugSelectionKind::Artwork,
                  state.selected_imported_artwork_id, 0};
            }
          } else {
            AddSelectedImportedArtworkObject(state, imported_artwork_hit.id);
            state.selected_imported_artwork_id = imported_artwork_hit.id;
            state.selected_imported_debug = {
                ImportedDebugSelectionKind::Artwork, imported_artwork_hit.id,
                0};
          }
        } else if (!IsImportedArtworkObjectSelected(state,
                                                    imported_artwork_hit.id) ||
                   CountSelectedImportedArtworkObjects(state) <= 1) {
          SelectImportedArtworkForCanvas(state, &transient_state,
                                         imported_artwork_hit.id);
        } else {
          state.selected_imported_artwork_id = imported_artwork_hit.id;
          state.selected_imported_debug = {ImportedDebugSelectionKind::Artwork,
                                           imported_artwork_hit.id, 0};
          state.selected_working_area_id = 0;
          ClearSelectedImportedElements(state);
          ResetMarqueeInteractionState(&transient_state);
        }
        if (ImportedArtwork *artwork =
                FindImportedArtwork(state, imported_artwork_hit.id);
            artwork != nullptr) {
          const ImVec2 world =
              ScreenToWorld(state, canvas_rect.Min, io.MousePos);
          if (imported_artwork_hit.zone ==
                  ImportedArtworkHitZone::ResizeHandle &&
              HasImportedArtworkFlag(artwork->flags,
                                     ImportedArtworkFlagResizable) &&
              CountSelectedImportedArtworkObjects(state) == 1) {
            PushUndoSnapshot(state, "Resize imported artwork");
            transient_state.resizing_imported_artwork_id =
                imported_artwork_hit.id;
            transient_state.imported_artwork_resize_initial_scale =
                artwork->scale;
          } else if (imported_artwork_hit.zone ==
                         ImportedArtworkHitZone::Body &&
                     HasImportedArtworkFlag(artwork->flags,
                                            ImportedArtworkFlagMovable)) {
            BeginImportedArtworkDrag(state, &transient_state,
                                     imported_artwork_hit.id, world);
          }
        }
      }
    } else if (area_hit.id != 0) {
      if (marquee_mode_active) {
        if (!IsCanvasArtworkScope(state)) {
          ClearAllImportedSelection(state);
        }
        BeginMarqueeInteraction(state, &transient_state, canvas_rect.Min,
                                io.MousePos);
      } else {
        ResetMarqueeInteractionState(&transient_state);
        state.selected_working_area_id = area_hit.id;
        ClearAllImportedSelection(state);
        if (WorkingArea *area = FindWorkingArea(state, area_hit.id);
            area != nullptr) {
          const ImVec2 world =
              ScreenToWorld(state, canvas_rect.Min, io.MousePos);
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
      }
    } else {
      if (marquee_mode_active) {
        if (!IsCanvasArtworkScope(state)) {
          ClearAllImportedSelection(state);
        }
        BeginMarqueeInteraction(state, &transient_state, canvas_rect.Min,
                                io.MousePos);
      } else {
        ResetMarqueeInteractionState(&transient_state);
        ClearAllImportedSelection(state);
      }
    }
  }

  if (IsMarqueeInteractionArmed(transient_state)) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      transient_state.marquee_end_world =
          ScreenToWorld(state, canvas_rect.Min, io.MousePos);
      const ImVec2 drag_delta(
          io.MousePos.x - transient_state.marquee_press_screen.x,
          io.MousePos.y - transient_state.marquee_press_screen.y);
      const float drag_distance_squared =
          drag_delta.x * drag_delta.x + drag_delta.y * drag_delta.y;
      if (drag_distance_squared >=
          kMarqueeDragStartDistancePixels * kMarqueeDragStartDistancePixels) {
        if (transient_state.marquee_state ==
            TransientCanvasState::MarqueeInteractionState::
                ArmedPendingObjectTarget) {
          const int target_artwork_id = ResolvePendingObjectMarqueeTarget(
              state, imported_path_hit, imported_artwork_hit,
              transient_state.marquee_start_world,
              transient_state.marquee_end_world);
          if (target_artwork_id != 0) {
            transient_state.marquee_artwork_id = target_artwork_id;
            SetSingleSelectedImportedArtworkObject(state, target_artwork_id);
            state.selected_imported_debug = {
                ImportedDebugSelectionKind::Artwork, target_artwork_id, 0};
            transient_state.marquee_state =
                TransientCanvasState::MarqueeInteractionState::SelectingObject;
          }
        } else {
          PromoteArmedMarqueeToSelecting(&transient_state);
        }
      }
    } else {
      ApplyMarqueeClickRelease(state, &transient_state);
      ResetMarqueeInteractionState(&transient_state);
    }
  }

  if (IsMarqueeInteractionSelecting(transient_state)) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      transient_state.marquee_end_world =
          ScreenToWorld(state, canvas_rect.Min, io.MousePos);
    } else {
      CommitMarqueeSelection(state, transient_state);
      ResetMarqueeInteractionState(&transient_state);
    }
  }

  if (transient_state.dragging_guide_id != 0) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (Guide *guide = FindGuide(state, transient_state.dragging_guide_id);
          guide != nullptr) {
        state.selected_guide_id = guide->id;
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

  if (!transient_state.dragging_imported_artwork.empty()) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      const ImVec2 world = ScreenToWorld(state, canvas_rect.Min, io.MousePos);
      auto anchor = std::find_if(
          transient_state.dragging_imported_artwork.begin(),
          transient_state.dragging_imported_artwork.end(),
          [&transient_state](const ImportedArtworkDragSnapshot &snapshot) {
            return snapshot.id ==
                   transient_state.dragging_imported_artwork_anchor_id;
          });
      if (anchor != transient_state.dragging_imported_artwork.end()) {
        ImVec2 anchor_origin(
            world.x - transient_state.imported_artwork_drag_offset.x,
            world.y - transient_state.imported_artwork_drag_offset.y);
        anchor_origin = SnapPoint(state, anchor_origin);
        const ImVec2 delta(anchor_origin.x - anchor->origin.x,
                           anchor_origin.y - anchor->origin.y);

        for (const ImportedArtworkDragSnapshot &snapshot :
             transient_state.dragging_imported_artwork) {
          if (ImportedArtwork *artwork =
                  FindImportedArtwork(state, snapshot.id);
              artwork != nullptr) {
            artwork->origin = ImVec2(snapshot.origin.x + delta.x,
                                     snapshot.origin.y + delta.y);
          }
        }
      }
    } else {
      transient_state.dragging_imported_artwork.clear();
      transient_state.dragging_imported_artwork_anchor_id = 0;
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

  if (!transient_state.resizing_imported_artwork_group.empty()) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      ImVec2 bottom_right =
          SnapPoint(state, ScreenToWorld(state, canvas_rect.Min, io.MousePos));
      const ImRect initial_world_rect =
          transient_state.imported_artwork_resize_initial_world_rect;
      bottom_right.x =
          std::max(bottom_right.x,
                   initial_world_rect.Min.x + options.min_working_area_size);
      bottom_right.y =
          std::max(bottom_right.y,
                   initial_world_rect.Min.y + options.min_working_area_size);

      const float initial_width =
          std::max(initial_world_rect.Max.x - initial_world_rect.Min.x, 1.0f);
      const float initial_height =
          std::max(initial_world_rect.Max.y - initial_world_rect.Min.y, 1.0f);
      const float target_width = bottom_right.x - initial_world_rect.Min.x;
      const float target_height = bottom_right.y - initial_world_rect.Min.y;
      const float factor_x = target_width / initial_width;
      const float factor_y = target_height / initial_height;
      const float chosen_factor = std::max(
          std::abs(factor_x - 1.0f) >= std::abs(factor_y - 1.0f) ? factor_x
                                                                 : factor_y,
          0.01f);

      for (const ImportedArtworkResizeSnapshot &snapshot :
           transient_state.resizing_imported_artwork_group) {
        if (ImportedArtwork *artwork = FindImportedArtwork(state, snapshot.id);
            artwork != nullptr) {
          artwork->origin =
              ImVec2(initial_world_rect.Min.x +
                         (snapshot.origin.x - initial_world_rect.Min.x) *
                             chosen_factor,
                     initial_world_rect.Min.y +
                         (snapshot.origin.y - initial_world_rect.Min.y) *
                             chosen_factor);
          UpdateImportedArtworkScaleFromTarget(
              *artwork,
              ImVec2(std::max(snapshot.scale.x * chosen_factor, 0.01f),
                     std::max(snapshot.scale.y * chosen_factor, 0.01f)));
        }
      }
    } else {
      transient_state.resizing_imported_artwork_group.clear();
      transient_state.imported_artwork_resize_initial_world_rect = ImRect();
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

  EnsureObjectScopeArtworkContext(state, &transient_state);

  if (!any_popup_open && hovered_guide_id != 0 &&
      ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
      !transient_state.right_mouse_dragged) {
    transient_state.context_guide_id = hovered_guide_id;
    transient_state.context_imported_artwork_id = 0;
    state.selected_guide_id = hovered_guide_id;
    ImGui::OpenPopup("guide_context_menu");
  } else if (!any_popup_open && IsCanvasArtworkScope(state) &&
             imported_artwork_hit.id != 0 &&
             ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
             !transient_state.right_mouse_dragged) {
    transient_state.context_imported_artwork_id = imported_artwork_hit.id;
    if (!IsCanvasArtworkScope(state) ||
        !IsImportedArtworkObjectSelected(state, imported_artwork_hit.id)) {
      SetSingleSelectedImportedArtworkObject(state, imported_artwork_hit.id);
    } else {
      state.selected_imported_artwork_id = imported_artwork_hit.id;
    }
    state.selected_imported_debug = {ImportedDebugSelectionKind::Artwork,
                                     imported_artwork_hit.id, 0};
    ImGui::OpenPopup("imported_artwork_context_menu");
  } else if (!any_popup_open && (top_ruler_hovered || left_ruler_hovered) &&
             ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
             !transient_state.right_mouse_dragged) {
    ImGui::OpenPopup("ruler_context_menu");
  } else if (!any_popup_open && canvas_input_hovered &&
             ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
             !transient_state.right_mouse_dragged) {
    ImGui::OpenPopup("canvas_context_menu");
  }

  if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
    transient_state.right_mouse_pressed_in_canvas = false;
    transient_state.right_mouse_dragged = false;
  }

  DrawGrid(draw_list, state, canvas_rect);
  DrawWorkingAreas(draw_list, state, canvas_rect,
                   state.selected_working_area_id, options);
  DrawImportedArtwork(draw_list, state, canvas_rect,
                      state.selected_imported_artwork_id, options);
  if (active_notification == ActiveCanvasNotification::CallerNotification) {
    DrawCallerNotification(draw_list, state, canvas_rect);
  }
  DrawSeparationPreviewOverlay(draw_list, state, canvas_rect,
                               active_notification ==
                                   ActiveCanvasNotification::GuideSplitPreview,
                               CanvasNotificationDismissMode::UserClosable);
  DrawAutoCutPreviewOverlay(draw_list, state, canvas_rect,
                            active_notification ==
                                ActiveCanvasNotification::AutoCutPreview,
                            CanvasNotificationDismissMode::UserClosable);
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

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                      options.context_menu_padding);

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
      const bool can_copy = CanCopySelectionToClipboard(state);
      const bool can_paste = HasClipboardContent(state);
      const bool can_group_artworks =
          HasGroupableImportedArtworkSelection(state);
      const bool can_group_selection =
          HasGroupableImportedElementSelection(state, *artwork);
      const bool can_group_root =
          HasGroupableImportedRootSelection(state, *artwork);
      const bool can_group =
          can_group_artworks || can_group_selection || can_group_root;
      const bool can_ungroup_artworks =
          HasUngroupableImportedArtworkSelection(state, *artwork);
      const bool can_ungroup_debug =
          HasUngroupableImportedDebugSelection(state, *artwork);
      const bool can_ungroup = can_ungroup_artworks || can_ungroup_debug;
      const bool has_preview =
          (state.imported_artwork_separation_preview.active &&
           state.imported_artwork_separation_preview.artwork_id ==
               artwork->id) ||
          (state.imported_artwork_auto_cut_preview.active &&
           state.imported_artwork_auto_cut_preview.artwork_id == artwork->id);
      const bool can_apply_auto_split =
          state.imported_artwork_auto_cut_preview.active &&
          state.imported_artwork_auto_cut_preview.artwork_id == artwork->id &&
          state.imported_artwork_auto_cut_preview.future_band_count > 1;
      const float minimum_gap =
          state.imported_artwork_auto_cut_preview.active &&
                  state.imported_artwork_auto_cut_preview.artwork_id ==
                      artwork->id
              ? state.imported_artwork_auto_cut_preview.minimum_gap
              : 5.0f;

      if (ImGui::MenuItem("Cut", "Ctrl+X", false, can_copy)) {
        CutSelectedToClipboard(state);
        ImGui::CloseCurrentPopup();
      }
      if (ImGui::MenuItem("Copy", "Ctrl+C", false, can_copy)) {
        CopySelectedToClipboard(state);
        ImGui::CloseCurrentPopup();
      }
      if (ImGui::MenuItem("Paste", "Ctrl+V", false, can_paste)) {
        PasteFromClipboard(state);
        ImGui::CloseCurrentPopup();
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Group", "Ctrl+G", false, can_group)) {
        if (can_group_artworks) {
          GroupSelectedImportedArtworkObjects(state);
        } else if (can_group_selection) {
          GroupSelectedImportedElements(state, artwork->id);
        } else {
          GroupImportedArtworkRootContents(state, artwork->id);
        }
        ImGui::CloseCurrentPopup();
      }
      if (ImGui::MenuItem("Ungroup", "Ctrl+Shift+G", false, can_ungroup)) {
        if (can_ungroup_artworks) {
          UngroupSelectedImportedArtworkObjects(state);
        } else {
          UngroupSelectedImportedGroup(state, artwork->id);
        }
        ImGui::CloseCurrentPopup();
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Flip Horizontal")) {
        ApplyImportedArtworkTransformToSelection(
            state, artwork->id, "Flip Horizontal",
            [](CanvasState &callback_state, const int target_artwork_id) {
              return FlipImportedArtworkHorizontal(callback_state,
                                                   target_artwork_id);
            });
        ImGui::CloseCurrentPopup();
      }
      if (ImGui::MenuItem("Flip Vertical")) {
        ApplyImportedArtworkTransformToSelection(
            state, artwork->id, "Flip Vertical",
            [](CanvasState &callback_state, const int target_artwork_id) {
              return FlipImportedArtworkVertical(callback_state,
                                                 target_artwork_id);
            });
        ImGui::CloseCurrentPopup();
      }
      if (ImGui::MenuItem("Rotate 90 CW")) {
        ApplyImportedArtworkTransformToSelection(
            state, artwork->id, "Rotate 90 CW",
            [](CanvasState &callback_state, const int target_artwork_id) {
              return RotateImportedArtworkClockwise(callback_state,
                                                    target_artwork_id);
            });
        ImGui::CloseCurrentPopup();
      }
      if (ImGui::MenuItem("Rotate 90 CCW")) {
        ApplyImportedArtworkTransformToSelection(
            state, artwork->id, "Rotate 90 CCW",
            [](CanvasState &callback_state, const int target_artwork_id) {
              return RotateImportedArtworkCounterClockwise(callback_state,
                                                           target_artwork_id);
            });
        ImGui::CloseCurrentPopup();
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Hide Selected")) {
        HideSelectedImportedArtwork(state);
        ImGui::CloseCurrentPopup();
      }
      if (ImGui::MenuItem("Isolate")) {
        IsolateSelectedImportedArtwork(state);
        ImGui::CloseCurrentPopup();
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Analyze Contours")) {
        ApplyImportedArtworkOperationToSelection(
            state, artwork->id, "Analyze Contours",
            [](CanvasState &callback_state, const int target_artwork_id) {
              return AnalyzeImportedArtworkContours(callback_state,
                                                    target_artwork_id);
            });
        ImGui::CloseCurrentPopup();
      }
      if (ImGui::BeginMenu("Repair")) {
        if (ImGui::MenuItem("Join Open Segments")) {
          ApplyImportedArtworkOperationToSelection(
              state, artwork->id, "Join Open Segments",
              [](CanvasState &callback_state, const int target_artwork_id) {
                return JoinImportedArtworkOpenSegments(callback_state,
                                                       target_artwork_id);
              });
          ImGui::CloseCurrentPopup();
        }
        if (ImGui::MenuItem("Repair Orphan Holes")) {
          ApplyImportedArtworkOperationToSelection(
              state, artwork->id, "Repair Orphan Holes",
              [](CanvasState &callback_state, const int target_artwork_id) {
                return RepairImportedArtworkOrphanHoles(callback_state,
                                                        target_artwork_id);
              });
          ImGui::CloseCurrentPopup();
        }
        ImGui::EndMenu();
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Clear Preview", nullptr, false, has_preview)) {
        ClearImportedArtworkSeparationPreview(state);
        ClearImportedArtworkAutoCutPreview(state);
        ImGui::CloseCurrentPopup();
      }
      if (ImGui::BeginMenu("Preview Auto-Split")) {
        if (ImGui::MenuItem("Horizontal")) {
          PreviewImportedArtworkAutoCut(state, artwork->id,
                                        AutoCutPreviewAxisMode::HorizontalOnly,
                                        minimum_gap);
          ImGui::CloseCurrentPopup();
        }
        if (ImGui::MenuItem("Vertical")) {
          PreviewImportedArtworkAutoCut(state, artwork->id,
                                        AutoCutPreviewAxisMode::VerticalOnly,
                                        minimum_gap);
          ImGui::CloseCurrentPopup();
        }
        if (ImGui::MenuItem("Both")) {
          PreviewImportedArtworkAutoCut(
              state, artwork->id, AutoCutPreviewAxisMode::Both, minimum_gap);
          ImGui::CloseCurrentPopup();
        }
        ImGui::EndMenu();
      }
      if (ImGui::MenuItem("Apply Auto-Split", nullptr, false,
                          can_apply_auto_split)) {
        ApplyImportedArtworkAutoCut(
            state, artwork->id,
            state.imported_artwork_auto_cut_preview.axis_mode, minimum_gap);
        ImGui::CloseCurrentPopup();
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Delete", "Delete")) {
        DeleteSelectedImportedContent(state);
        transient_state.context_imported_artwork_id = 0;
        transient_state.dragging_imported_artwork.clear();
        transient_state.resizing_imported_artwork_group.clear();
        transient_state.dragging_imported_artwork_anchor_id = 0;
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
      const int selected_artwork_count =
          CountSelectedImportedArtworkObjects(state);
      const int fallback_artwork_id = state.selected_imported_artwork_id;
      const bool can_run_guide_split = guide->id != 0 &&
                                       selected_artwork_count == 1 &&
                                       fallback_artwork_id != 0;
      const bool has_preview =
          state.imported_artwork_separation_preview.active ||
          state.imported_artwork_auto_cut_preview.active;

      if (ImGui::MenuItem(guide->locked ? "Unlock guide" : "Lock guide")) {
        guide->locked = !guide->locked;
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Clear Preview", nullptr, false, has_preview)) {
        ClearImportedArtworkSeparationPreview(state);
        ClearImportedArtworkAutoCutPreview(state);
        ImGui::CloseCurrentPopup();
      }
      if (ImGui::MenuItem("Preview Guide Split", nullptr, false,
                          can_run_guide_split)) {
        ApplyImportedArtworkOperationToSelection(
            state, fallback_artwork_id, "Preview Guide Split",
            [guide_id = guide->id](CanvasState &callback_state,
                                   const int target_artwork_id) {
              return PreviewSeparateImportedArtworkByGuide(
                  callback_state, target_artwork_id, guide_id);
            });
        ImGui::CloseCurrentPopup();
      }
      if (ImGui::MenuItem("Apply Guide Split", nullptr, false,
                          can_run_guide_split)) {
        ApplyImportedArtworkOperationToSelection(
            state, fallback_artwork_id, "Apply Guide Split",
            [guide_id = guide->id](CanvasState &callback_state,
                                   const int target_artwork_id) {
              return SeparateImportedArtworkByGuide(
                  callback_state, target_artwork_id, guide_id);
            });
        ImGui::CloseCurrentPopup();
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Delete guide")) {
        if (state.imported_artwork_separation_preview.active &&
            std::find(
                state.imported_artwork_separation_preview.guide_ids.begin(),
                state.imported_artwork_separation_preview.guide_ids.end(),
                guide->id) !=
                state.imported_artwork_separation_preview.guide_ids.end()) {
          ClearImportedArtworkSeparationPreview(state);
        }
        if (state.selected_guide_id == guide->id) {
          state.selected_guide_id = 0;
        }
        RemoveGuide(state, guide->id);
        transient_state.context_guide_id = 0;
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }

  if (ImGui::BeginPopup("canvas_context_menu")) {
    const bool has_selection = state.selected_working_area_id != 0 ||
                               state.selected_guide_id != 0 ||
                               state.selected_imported_artwork_id != 0 ||
                               !state.selected_imported_elements.empty();
    const bool can_paste = HasClipboardContent(state);
    const bool has_imported_artwork = !state.imported_artwork.empty();
    const bool any_visible_artwork = std::ranges::any_of(
        state.imported_artwork,
        [](const ImportedArtwork &artwork) { return artwork.visible; });
    if (ImGui::MenuItem("Paste", "Ctrl+V", false, can_paste)) {
      PasteFromClipboard(state);
      ImGui::CloseCurrentPopup();
    }
    ImGui::Separator();
    const char *visibility_label =
        any_visible_artwork ? "Hide All" : "Show All";
    if (ImGui::MenuItem(visibility_label, nullptr, false,
                        has_imported_artwork)) {
      if (any_visible_artwork) {
        HideAllImportedArtwork(state);
      } else {
        ShowAllImportedArtwork(state);
      }
      ImGui::CloseCurrentPopup();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Select All", "Ctrl+A", false,
                        !state.imported_artwork.empty())) {
      state.selected_working_area_id = 0;
      state.selected_guide_id = 0;
      ResetMarqueeInteractionState(&transient_state);
      SelectAllVisibleImportedArtwork(state);
      ImGui::CloseCurrentPopup();
    }
    if (ImGui::MenuItem("Clear Selection", nullptr, false, has_selection)) {
      ClearAllImportedSelection(state);
      state.selected_working_area_id = 0;
      state.selected_guide_id = 0;
      ResetMarqueeInteractionState(&transient_state);
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  ImGui::PopStyleVar();

  ImGui::PopID();
  return canvas_hovered;
}

} // namespace im2d
