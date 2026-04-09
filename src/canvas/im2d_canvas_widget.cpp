#include "im2d_canvas_widget.h"

#include "im2d_canvas_document.h"
#include "im2d_canvas_imported_artwork_ops.h"
#include "im2d_canvas_internal.h"
#include "im2d_canvas_notification.h"
#include "im2d_canvas_snap.h"
#include "im2d_canvas_undo.h"
#include "im2d_canvas_units.h"
#include "im2d_canvas_widget_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include <imgui_internal.h>

namespace im2d {

namespace {

constexpr float kMarqueeDragStartDistancePixels = 4.0f;
constexpr float kRightDragStartDistancePixels = 4.0f;

} // namespace

namespace detail {

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
                                   const int preferred_artwork_id) {
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
                             const int preferred_artwork_id) {
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

void UpdateCanvasManipulation(CanvasState &state,
                              TransientCanvasState &transient,
                              const ImVec2 &canvas_min, const ImVec2 &mouse_pos,
                              const CanvasWidgetOptions &options) {
  if (transient.dragging_guide_id != 0) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (Guide *guide = FindGuide(state, transient.dragging_guide_id);
          guide != nullptr) {
        state.selected_guide_id = guide->id;
        const ImVec2 world = ScreenToWorld(state, canvas_min, mouse_pos);
        const float raw_position =
            guide->orientation == GuideOrientation::Vertical ? world.x
                                                             : world.y;
        guide->position = SnapAxisCoordinate(state, guide->orientation,
                                             raw_position, guide->id)
                              .value;
      }
    } else {
      transient.dragging_guide_id = 0;
    }
  }

  if (!transient.dragging_imported_artwork.empty()) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      const ImVec2 world = ScreenToWorld(state, canvas_min, mouse_pos);
      auto anchor = std::find_if(
          transient.dragging_imported_artwork.begin(),
          transient.dragging_imported_artwork.end(),
          [&transient](const ImportedArtworkDragSnapshot &snapshot) {
            return snapshot.id == transient.dragging_imported_artwork_anchor_id;
          });
      if (anchor != transient.dragging_imported_artwork.end()) {
        ImVec2 anchor_origin(world.x - transient.imported_artwork_drag_offset.x,
                             world.y -
                                 transient.imported_artwork_drag_offset.y);
        anchor_origin = SnapPoint(state, anchor_origin);
        const ImVec2 delta(anchor_origin.x - anchor->origin.x,
                           anchor_origin.y - anchor->origin.y);

        for (const ImportedArtworkDragSnapshot &snapshot :
             transient.dragging_imported_artwork) {
          if (ImportedArtwork *artwork =
                  FindImportedArtwork(state, snapshot.id);
              artwork != nullptr) {
            artwork->origin = ImVec2(snapshot.origin.x + delta.x,
                                     snapshot.origin.y + delta.y);
          }
        }
      }
    } else {
      transient.dragging_imported_artwork.clear();
      transient.dragging_imported_artwork_anchor_id = 0;
    }
  }

  if (transient.resizing_imported_artwork_id != 0) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (ImportedArtwork *artwork = FindImportedArtwork(
              state, transient.resizing_imported_artwork_id);
          artwork != nullptr) {
        ImVec2 bottom_right =
            SnapPoint(state, ScreenToWorld(state, canvas_min, mouse_pos));
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
              transient.imported_artwork_resize_initial_scale.x, 0.01f);
          const float base_y = std::max(
              transient.imported_artwork_resize_initial_scale.y, 0.01f);
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
      transient.resizing_imported_artwork_id = 0;
      transient.imported_artwork_resize_initial_scale = ImVec2(1.0f, 1.0f);
    }
  }

  if (!transient.resizing_imported_artwork_group.empty()) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      ImVec2 bottom_right =
          SnapPoint(state, ScreenToWorld(state, canvas_min, mouse_pos));
      const ImRect initial_world_rect =
          transient.imported_artwork_resize_initial_world_rect;
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
           transient.resizing_imported_artwork_group) {
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
      transient.resizing_imported_artwork_group.clear();
      transient.imported_artwork_resize_initial_world_rect = ImRect();
    }
  }

  if (transient.dragging_working_area_id != 0) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (WorkingArea *area =
              FindWorkingArea(state, transient.dragging_working_area_id);
          area != nullptr) {
        const ImVec2 world = ScreenToWorld(state, canvas_min, mouse_pos);
        ImVec2 new_origin(world.x - transient.working_area_drag_offset.x,
                          world.y - transient.working_area_drag_offset.y);
        new_origin = SnapPoint(state, new_origin);
        area->origin = new_origin;
        SyncExportAreaFromWorkingArea(state, area->id);
      }
    } else {
      transient.dragging_working_area_id = 0;
    }
  }

  if (transient.resizing_working_area_id != 0) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (WorkingArea *area =
              FindWorkingArea(state, transient.resizing_working_area_id);
          area != nullptr) {
        ImVec2 bottom_right =
            SnapPoint(state, ScreenToWorld(state, canvas_min, mouse_pos));
        bottom_right.x = std::max(
            bottom_right.x, area->origin.x + options.min_working_area_size);
        bottom_right.y = std::max(
            bottom_right.y, area->origin.y + options.min_working_area_size);
        area->size = ImVec2(bottom_right.x - area->origin.x,
                            bottom_right.y - area->origin.y);
        SyncExportAreaFromWorkingArea(state, area->id);
      }
    } else {
      transient.resizing_working_area_id = 0;
    }
  }
}

void HandleCanvasRightClickRelease(
    CanvasState &state, TransientCanvasState &transient, bool any_popup_open,
    bool canvas_input_hovered, bool top_ruler_hovered, bool left_ruler_hovered,
    int hovered_guide_id, const ImportedArtworkHit &artwork_hit) {
  if (!any_popup_open && hovered_guide_id != 0 &&
      ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
      !transient.right_mouse_dragged) {
    transient.context_guide_id = hovered_guide_id;
    transient.context_imported_artwork_id = 0;
    state.selected_guide_id = hovered_guide_id;
    ImGui::OpenPopup("guide_context_menu");
  } else if (!any_popup_open && artwork_hit.id != 0 &&
             ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
             !transient.right_mouse_dragged) {
    transient.context_imported_artwork_id = artwork_hit.id;
    if (!IsCanvasArtworkScope(state) ||
        !IsImportedArtworkObjectSelected(state, artwork_hit.id)) {
      SetSingleSelectedImportedArtworkObject(state, artwork_hit.id);
    } else {
      state.selected_imported_artwork_id = artwork_hit.id;
    }
    state.selected_imported_debug = {ImportedDebugSelectionKind::Artwork,
                                     artwork_hit.id, 0};
    ImGui::OpenPopup("imported_artwork_context_menu");
  } else if (!any_popup_open && (top_ruler_hovered || left_ruler_hovered) &&
             ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
             !transient.right_mouse_dragged) {
    ImGui::OpenPopup("ruler_context_menu");
  } else if (!any_popup_open && canvas_input_hovered &&
             ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
             !transient.right_mouse_dragged) {
    ImGui::OpenPopup("canvas_context_menu");
  }

  if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
    transient.right_mouse_pressed_in_canvas = false;
    transient.right_mouse_dragged = false;
  }
}

void DrawCanvasContextMenus(CanvasState &state, TransientCanvasState &transient,
                            const ImVec2 &context_menu_padding) {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, context_menu_padding);

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
    if (ImportedArtwork *artwork =
            FindImportedArtwork(state, transient.context_imported_artwork_id);
        artwork != nullptr) {
      const bool has_selected_elements =
          !state.selected_imported_elements.empty() &&
          state.selected_imported_artwork_id != 0;
      const bool can_copy = CanCopySelectionToClipboard(state);
      const bool can_paste = HasClipboardContent(state);
      const bool can_extract =
          has_selected_elements ||
          HasExtractableImportedDebugSelection(state, *artwork);
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
          CanvasEditor::HasArtworkPreview(state, artwork->id);
      const bool any_hidden_artwork = std::ranges::any_of(
          state.imported_artwork,
          [](const ImportedArtwork &candidate) { return !candidate.visible; });

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
      if (ImGui::MenuItem("Canvas Scope", nullptr,
                          state.selection_scope ==
                              ImportedArtworkSelectionScope::Canvas)) {
        ApplyImportedArtworkSelectionScope(
            state, ImportedArtworkSelectionScope::Canvas);
        ImGui::CloseCurrentPopup();
      }
      if (ImGui::MenuItem("Object Scope", nullptr,
                          state.selection_scope ==
                              ImportedArtworkSelectionScope::Object)) {
        ApplyImportedArtworkSelectionScope(
            state, ImportedArtworkSelectionScope::Object);
        ImGui::CloseCurrentPopup();
      }
      if (ImGui::BeginMenu("Selection Tool")) {
        if (ImGui::MenuItem("Pointer", nullptr,
                            state.imported_artwork_edit_mode ==
                                ImportedArtworkEditMode::None)) {
          state.imported_artwork_edit_mode = ImportedArtworkEditMode::None;
          ImGui::CloseCurrentPopup();
        }
        if (ImGui::MenuItem("Rectangle", nullptr,
                            state.imported_artwork_edit_mode ==
                                ImportedArtworkEditMode::SelectRectangle)) {
          state.imported_artwork_edit_mode =
              ImportedArtworkEditMode::SelectRectangle;
          ImGui::CloseCurrentPopup();
        }
        if (ImGui::MenuItem("Oval", nullptr,
                            state.imported_artwork_edit_mode ==
                                ImportedArtworkEditMode::SelectOval)) {
          state.imported_artwork_edit_mode =
              ImportedArtworkEditMode::SelectOval;
          ImGui::CloseCurrentPopup();
        }
        ImGui::EndMenu();
      }
      if (ImGui::MenuItem("Extract Selection", nullptr, false, can_extract)) {
        ExtractSelectedImportedElements(state, artwork->id);
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
      if (ImGui::MenuItem("Hide Selected", "H")) {
        HideSelectedImportedArtwork(state);
        ImGui::CloseCurrentPopup();
      }
      if (ImGui::MenuItem("Isolate", "I")) {
        IsolateSelectedImportedArtwork(state);
        ImGui::CloseCurrentPopup();
      }
      if (ImGui::MenuItem("Show All", "Shift+H", false, any_hidden_artwork)) {
        ShowAllImportedArtwork(state);
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
      if (ImGui::MenuItem("Repair Orphan Holes")) {
        ApplyImportedArtworkOperationToSelection(
            state, artwork->id, "Repair Orphan Holes",
            [](CanvasState &callback_state, const int target_artwork_id) {
              return RepairImportedArtworkOrphanHoles(callback_state,
                                                      target_artwork_id);
            });
        ImGui::CloseCurrentPopup();
      }
      if (ImGui::MenuItem("Weld...")) {
        transient.canvas_editor.OpenWeld(artwork->id);
        ImGui::CloseCurrentPopup();
      }
      if (ImGui::MenuItem("Split...")) {
        transient.canvas_editor.OpenSplit(artwork->id);
        ImGui::CloseCurrentPopup();
      }
      if (has_preview) {
        ImGui::Separator();
        if (ImGui::MenuItem("Clear Preview")) {
          CanvasEditor::ClearPreviewStateForArtwork(state, artwork->id);
          ImGui::CloseCurrentPopup();
        }
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Delete", "Delete")) {
        DeleteSelectedImportedContent(state);
        transient.context_imported_artwork_id = 0;
        transient.dragging_imported_artwork.clear();
        transient.resizing_imported_artwork_group.clear();
        transient.dragging_imported_artwork_anchor_id = 0;
        transient.resizing_imported_artwork_id = 0;
        transient.imported_artwork_resize_initial_scale = ImVec2(1.0f, 1.0f);
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }

  transient.canvas_editor.Draw(state);

  if (ImGui::BeginPopup("guide_context_menu")) {
    if (Guide *guide = FindGuide(state, transient.context_guide_id);
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
        transient.context_guide_id = 0;
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
    if (ImGui::MenuItem("Canvas Scope", nullptr,
                        state.selection_scope ==
                            ImportedArtworkSelectionScope::Canvas)) {
      ApplyImportedArtworkSelectionScope(state,
                                         ImportedArtworkSelectionScope::Canvas);
      ImGui::CloseCurrentPopup();
    }
    if (ImGui::MenuItem("Object Scope", nullptr,
                        state.selection_scope ==
                            ImportedArtworkSelectionScope::Object)) {
      ApplyImportedArtworkSelectionScope(state,
                                         ImportedArtworkSelectionScope::Object);
      ImGui::CloseCurrentPopup();
    }
    if (ImGui::BeginMenu("Selection Tool")) {
      if (ImGui::MenuItem("Pointer", nullptr,
                          state.imported_artwork_edit_mode ==
                              ImportedArtworkEditMode::None)) {
        state.imported_artwork_edit_mode = ImportedArtworkEditMode::None;
        ImGui::CloseCurrentPopup();
      }
      if (ImGui::MenuItem("Rectangle", nullptr,
                          state.imported_artwork_edit_mode ==
                              ImportedArtworkEditMode::SelectRectangle)) {
        state.imported_artwork_edit_mode =
            ImportedArtworkEditMode::SelectRectangle;
        ImGui::CloseCurrentPopup();
      }
      if (ImGui::MenuItem("Oval", nullptr,
                          state.imported_artwork_edit_mode ==
                              ImportedArtworkEditMode::SelectOval)) {
        state.imported_artwork_edit_mode = ImportedArtworkEditMode::SelectOval;
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndMenu();
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
      ResetMarqueeInteractionState(&transient);
      SelectAllVisibleImportedArtwork(state);
      ImGui::CloseCurrentPopup();
    }
    if (ImGui::MenuItem("Clear Selection", nullptr, false, has_selection)) {
      ClearAllImportedSelection(state);
      state.selected_working_area_id = 0;
      state.selected_guide_id = 0;
      ResetMarqueeInteractionState(&transient);
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  ImGui::PopStyleVar();
}

} // namespace detail

bool DrawCanvas(CanvasState &state, const CanvasWidgetOptions &options) {
  using namespace detail;
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

  const bool canvas_window_hovered =
      ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
  const bool another_window_hovered =
      ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) &&
      !canvas_window_hovered;
  const bool canvas_hovered =
      canvas_rect.Contains(io.MousePos) && !another_window_hovered;
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
  const bool top_ruler_hovered =
      top_ruler_rect.Contains(io.MousePos) && !another_window_hovered;
  const bool left_ruler_hovered =
      left_ruler_rect.Contains(io.MousePos) && !another_window_hovered;
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

  UpdateCanvasManipulation(state, transient_state, canvas_rect.Min, io.MousePos,
                           options);

  EnsureObjectScopeArtworkContext(state, &transient_state);

  HandleCanvasRightClickRelease(state, transient_state, any_popup_open,
                                canvas_input_hovered, top_ruler_hovered,
                                left_ruler_hovered, hovered_guide_id,
                                imported_artwork_hit);

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

  DrawCanvasContextMenus(state, transient_state, options.context_menu_padding);

  ImGui::PopID();
  return canvas_hovered;
}

} // namespace im2d
