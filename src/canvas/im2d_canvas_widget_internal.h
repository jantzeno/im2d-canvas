#pragma once

#include "im2d_canvas_types.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <functional>
#include <string>
#include <vector>

namespace im2d::detail {

// ── TransientCanvasState ────────────────────────────────────────────────────

struct ImportedArtworkDragSnapshot {
  int id = 0;
  ImVec2 origin = ImVec2(0.0f, 0.0f);
};

struct ImportedArtworkResizeSnapshot {
  int id = 0;
  ImVec2 origin = ImVec2(0.0f, 0.0f);
  ImVec2 scale = ImVec2(1.0f, 1.0f);
};

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

TransientCanvasState &GetTransientCanvasState();

// ── Hit detection types ─────────────────────────────────────────────────────

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

struct ImportedPathHit {
  int artwork_id = 0;
  int path_id = 0;
};

// ── Preview overlay types ───────────────────────────────────────────────────

struct PreviewOverlayColors {
  ImU32 stroke = 0;
  ImU32 fill = 0;
};

struct PreviewBucketRegion {
  int bucket_index = 0;
  int bucket_column = 0;
  int bucket_row = 0;
  PreviewOverlayColors colors;
};

// ── Coordinate transforms ───────────────────────────────────────────────────

float ClampZoom(float zoom);
ImVec2 WorldToScreen(const CanvasState &state, const ImVec2 &canvas_min,
                     const ImVec2 &world);
ImVec2 ScreenToWorld(const CanvasState &state, const ImVec2 &canvas_min,
                     const ImVec2 &screen);

// ── TransientCanvasState helpers ────────────────────────────────────────────

void ResetMarqueeInteractionState(TransientCanvasState *state);
bool IsMarqueeInteractionActive(const TransientCanvasState &state);
bool IsMarqueeInteractionArmed(const TransientCanvasState &state);
bool IsMarqueeInteractionSelecting(const TransientCanvasState &state);
void ClearActiveCanvasManipulation(TransientCanvasState *state);
bool IsCanvasArtworkScope(const CanvasState &state);
bool IsImportedArtworkSelectionModifierDown();

// ── Hit detection ───────────────────────────────────────────────────────────

int FindHoveredGuide(const CanvasState &state, const ImVec2 &canvas_min,
                     const ImRect &canvas_rect, const ImVec2 &mouse_pos);
WorkingAreaHit FindHoveredWorkingArea(const CanvasState &state,
                                      const ImVec2 &canvas_min,
                                      const ImRect &canvas_rect,
                                      const ImVec2 &mouse_pos,
                                      float resize_handle_size);
ImportedArtworkHit FindHoveredImportedArtwork(const CanvasState &state,
                                              const ImVec2 &canvas_min,
                                              const ImRect &canvas_rect,
                                              const ImVec2 &mouse_pos,
                                              float resize_handle_size);
ImportedPathHit FindHoveredImportedPath(const CanvasState &state,
                                        const ImVec2 &canvas_min,
                                        const ImRect &canvas_rect,
                                        const ImVec2 &mouse_pos,
                                        float hit_tolerance);

bool TryGetImportedArtworkWorldRect(const ImportedArtwork &artwork,
                                    ImRect *out_world);
bool TryGetSelectedImportedArtworkWorldRect(const CanvasState &state,
                                            ImRect *out_world);
bool TryGetSelectedImportedArtworkScreenRect(const CanvasState &state,
                                             const ImVec2 &canvas_min,
                                             ImRect *out_screen);
bool PointInsideSelectionShape(const ImRect &selection_rect,
                               ImportedArtworkEditMode mode,
                               const ImVec2 &test_point);
ImRect ImportedArtworkResizeHandleRect(const ImRect &screen_rect,
                                       float handle_size);
float DistanceSquaredToSegment(const ImVec2 &point, const ImVec2 &segment_start,
                               const ImVec2 &segment_end);
ImRect WorkingAreaScreenRect(const CanvasState &state, const ImVec2 &canvas_min,
                             const WorkingArea &area);
ImRect ImportedArtworkScreenRect(const CanvasState &state,
                                 const ImVec2 &canvas_min,
                                 const ImportedArtwork &artwork);
ImRect WorldRectToScreenRect(const CanvasState &state, const ImVec2 &canvas_min,
                             const ImRect &world_rect);
ImRect ImportedElementScreenRect(const CanvasState &state,
                                 const ImVec2 &canvas_min,
                                 const ImportedArtwork &artwork,
                                 const ImVec2 &bounds_min,
                                 const ImVec2 &bounds_max);
bool TryGetImportedDebugScreenRect(const CanvasState &state,
                                   const ImVec2 &canvas_min, ImRect *out_rect);
bool IsLastOperationIssueElement(const CanvasState &state, int artwork_id,
                                 ImportedElementKind kind, int item_id);

// ── Marquee & selection ─────────────────────────────────────────────────────

void BeginMarqueeInteraction(CanvasState &state,
                             TransientCanvasState *transient,
                             const ImVec2 &canvas_min, const ImVec2 &mouse_pos,
                             int pending_object_artwork_id = 0);
void PromoteArmedMarqueeToSelecting(TransientCanvasState *state);
void ApplyMarqueeClickRelease(CanvasState &state,
                              TransientCanvasState *transient);
void CommitMarqueeSelection(CanvasState &state,
                            const TransientCanvasState &transient);
int FindFirstIntersectingImportedArtworkId(const CanvasState &state,
                                           const ImVec2 &world_min,
                                           const ImVec2 &world_max);
int ResolvePendingObjectMarqueeTarget(const CanvasState &state,
                                      const ImportedPathHit &path_hit,
                                      const ImportedArtworkHit &artwork_hit,
                                      const ImVec2 &world_start,
                                      const ImVec2 &world_end);
void SelectImportedArtworkObjectsInWorldRect(CanvasState &state,
                                             const ImVec2 &start,
                                             const ImVec2 &end,
                                             ImportedArtworkEditMode mode);
void ClearAllImportedSelection(CanvasState &state);
void SelectAllVisibleImportedArtwork(CanvasState &state);
void ClearImportedSelectionForCurrentScope(CanvasState &state,
                                           TransientCanvasState *transient);
void EnsureObjectScopeArtworkContext(CanvasState &state,
                                     TransientCanvasState *transient);
void SelectImportedPathForObjectScope(CanvasState &state,
                                      TransientCanvasState *transient,
                                      int artwork_id, int path_id,
                                      bool additive);
void SelectImportedArtworkForCanvas(CanvasState &state,
                                    TransientCanvasState *transient,
                                    int artwork_id);
int ResolveActiveImportedArtworkId(const CanvasState &state,
                                   int context_artwork_id = 0);
int ResolveRecoverableObjectScopeArtworkId(
    const CanvasState &state, const TransientCanvasState &transient);

// ── Manipulation ────────────────────────────────────────────────────────────

void BeginImportedArtworkDrag(CanvasState &state,
                              TransientCanvasState *transient, int artwork_id,
                              const ImVec2 &world_pos);
void BeginSelectedImportedArtworkResize(CanvasState &state,
                                        TransientCanvasState *transient);
ImportedArtworkOperationResult ApplyImportedArtworkTransformToSelection(
    CanvasState &state, int artwork_id, const char *undo_label,
    const std::function<ImportedArtworkOperationResult(CanvasState &, int)>
        &transform);
void RemoveGuide(CanvasState &state, int guide_id);

// ── Rendering ───────────────────────────────────────────────────────────────

void DrawGrid(ImDrawList *draw_list, const CanvasState &state,
              const ImRect &canvas_rect);
void DrawWorkingAreas(ImDrawList *draw_list, const CanvasState &state,
                      const ImRect &canvas_rect, int selected_id,
                      const CanvasWidgetOptions &options);
void DrawImportedArtwork(ImDrawList *draw_list, const CanvasState &state,
                         const ImRect &canvas_rect, int selected_id,
                         const CanvasWidgetOptions &options);
void DrawImportedMarquee(ImDrawList *draw_list, const CanvasState &state,
                         const ImRect &canvas_rect,
                         const TransientCanvasState &transient);
void DrawGuides(ImDrawList *draw_list, const CanvasState &state,
                const ImRect &canvas_rect, int hovered_id,
                const TransientCanvasState &transient);
void DrawRulerAxis(ImDrawList *draw_list, const CanvasState &state,
                   const ImRect &ruler_rect, bool horizontal);
void DrawSeparationPreviewOverlay(ImDrawList *draw_list,
                                  const CanvasState &state,
                                  const ImRect &canvas_rect, bool show_banner,
                                  CanvasNotificationDismissMode dismiss_mode);
void DrawAutoCutPreviewOverlay(ImDrawList *draw_list, const CanvasState &state,
                               const ImRect &canvas_rect, bool show_banner,
                               CanvasNotificationDismissMode dismiss_mode);
void DrawCallerNotification(ImDrawList *draw_list, const CanvasState &state,
                            const ImRect &canvas_rect);

// ── Ruler helpers ───────────────────────────────────────────────────────────

float ComputeLeftRulerThickness(const CanvasState &state,
                                const ImRect &total_rect,
                                float top_ruler_thickness,
                                float base_thickness);

// ── Preview / notification helpers ──────────────────────────────────────────

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

ActiveCanvasNotificationContent
BuildActiveCanvasNotificationContent(const CanvasState &state);

// ── Extracted DrawCanvas phases ─────────────────────────────────────────────

void UpdateCanvasManipulation(CanvasState &state,
                              TransientCanvasState &transient,
                              const ImVec2 &canvas_min, const ImVec2 &mouse_pos,
                              const CanvasWidgetOptions &options);

void HandleCanvasRightClickRelease(
    CanvasState &state, TransientCanvasState &transient, bool any_popup_open,
    bool canvas_input_hovered, bool top_ruler_hovered, bool left_ruler_hovered,
    int hovered_guide_id, const ImportedArtworkHit &artwork_hit);

void DrawCanvasContextMenus(CanvasState &state, TransientCanvasState &transient,
                            const ImVec2 &context_menu_padding);

} // namespace im2d::detail
