#pragma once

#include "im2d_canvas_types.h"

namespace im2d {

bool IsImportedArtworkScaleRatioLocked(const ImportedArtwork &artwork);
void UpdateImportedArtworkScaleFromTarget(ImportedArtwork &artwork,
                                          const ImVec2 &target_scale);
bool FlipImportedArtworkHorizontal(CanvasState &state, int imported_artwork_id);
bool FlipImportedArtworkVertical(CanvasState &state, int imported_artwork_id);
bool RotateImportedArtworkClockwise(CanvasState &state,
                                    int imported_artwork_id);
bool RotateImportedArtworkCounterClockwise(CanvasState &state,
                                           int imported_artwork_id);
ImportedArtworkOperationResult
AutoCloseImportedArtworkToPolyline(CanvasState &state, int imported_artwork_id,
                                   float weld_tolerance = 0.5f);
ImportedArtworkOperationResult PrepareImportedArtworkForCutting(
    CanvasState &state, int imported_artwork_id, float weld_tolerance = 0.5f,
    ImportedArtworkPrepareMode mode = ImportedArtworkPrepareMode::FidelityFirst,
    bool auto_close_to_polyline = false);
ImportedArtworkOperationResult SelectImportedElementsInWorldRect(
    CanvasState &state, int imported_artwork_id, const ImVec2 &world_start,
    const ImVec2 &world_end, ImportedArtworkEditMode mode);
ImportedArtworkOperationResult
ExtractSelectedImportedElements(CanvasState &state, int imported_artwork_id);
ImportedArtworkOperationResult
SeparateImportedArtworkByGuide(CanvasState &state, int imported_artwork_id,
                               int guide_id);
bool DeleteImportedArtwork(CanvasState &state, int imported_artwork_id);

} // namespace im2d