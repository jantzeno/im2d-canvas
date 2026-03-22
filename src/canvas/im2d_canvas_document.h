#pragma once

#include "im2d_canvas_types.h"

namespace im2d {

void InitializeDefaultDocument(CanvasState &state);
int AddWorkingArea(CanvasState &state,
                   const WorkingAreaCreateInfo &create_info);
int AppendImportedArtwork(CanvasState &state, ImportedArtwork artwork);
void ClearImportedArtwork(CanvasState &state);
void ClearImportedDebugSelection(CanvasState &state);
void ClearSelectedImportedElements(CanvasState &state);
bool IsImportedElementSelected(const CanvasState &state, int artwork_id,
                               ImportedElementKind kind, int item_id);
void RecomputeImportedArtworkBounds(ImportedArtwork &artwork);
void RecomputeImportedHierarchyBounds(ImportedArtwork &artwork);
bool IsImportedArtworkScaleRatioLocked(const ImportedArtwork &artwork);
void SetImportedArtworkScaleRatioLocked(ImportedArtwork &artwork, bool locked);
void UpdateImportedArtworkScaleAxis(ImportedArtwork &artwork, int axis,
                                    float new_value);
void UpdateImportedArtworkScaleFromTarget(ImportedArtwork &artwork,
                                          const ImVec2 &target_scale);
bool FlipImportedArtworkHorizontal(CanvasState &state, int imported_artwork_id);
bool FlipImportedArtworkVertical(CanvasState &state, int imported_artwork_id);
bool RotateImportedArtworkClockwise(CanvasState &state,
                                    int imported_artwork_id);
bool RotateImportedArtworkCounterClockwise(CanvasState &state,
                                           int imported_artwork_id);
ImportedArtworkOperationResult
UpdateImportedArtworkOutlineColor(CanvasState &state, int imported_artwork_id,
                                  const ImVec4 &stroke_color);
ImportedArtworkOperationResult
PrepareImportedArtworkForCutting(CanvasState &state, int imported_artwork_id,
                                 float weld_tolerance = 0.5f);
ImportedArtworkOperationResult SelectImportedElementsInWorldRect(
    CanvasState &state, int imported_artwork_id, const ImVec2 &world_start,
    const ImVec2 &world_end, ImportedArtworkEditMode mode);
ImportedArtworkOperationResult
ExtractSelectedImportedElements(CanvasState &state, int imported_artwork_id);
ImportedArtworkOperationResult
SeparateImportedArtworkByGuide(CanvasState &state, int imported_artwork_id,
                               int guide_id);
bool DeleteImportedArtwork(CanvasState &state, int imported_artwork_id);
Guide *FindGuide(CanvasState &state, int guide_id);
const Guide *FindGuide(const CanvasState &state, int guide_id);
ImportedArtwork *FindImportedArtwork(CanvasState &state,
                                     int imported_artwork_id);
const ImportedArtwork *FindImportedArtwork(const CanvasState &state,
                                           int imported_artwork_id);
ImportedGroup *FindImportedGroup(ImportedArtwork &artwork, int group_id);
const ImportedGroup *FindImportedGroup(const ImportedArtwork &artwork,
                                       int group_id);
ImportedPath *FindImportedPath(ImportedArtwork &artwork, int path_id);
const ImportedPath *FindImportedPath(const ImportedArtwork &artwork,
                                     int path_id);
ImportedDxfText *FindImportedDxfText(ImportedArtwork &artwork, int text_id);
const ImportedDxfText *FindImportedDxfText(const ImportedArtwork &artwork,
                                           int text_id);
ImVec2 ImportedArtworkPointToWorld(const ImportedArtwork &artwork,
                                   const ImVec2 &point);
void ImportedLocalBoundsToWorldBounds(const ImportedArtwork &artwork,
                                      const ImVec2 &local_min,
                                      const ImVec2 &local_max,
                                      ImVec2 *world_min, ImVec2 *world_max);
WorkingArea *FindWorkingArea(CanvasState &state, int working_area_id);
const WorkingArea *FindWorkingArea(const CanvasState &state,
                                   int working_area_id);
void SyncExportAreaFromWorkingArea(CanvasState &state, int working_area_id);

} // namespace im2d