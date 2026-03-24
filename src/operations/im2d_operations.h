#pragma once

#include "../canvas/im2d_canvas_types.h"

namespace im2d::operations {

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
ExtractSelectedImportedElements(CanvasState &state, int imported_artwork_id);
ImportedArtworkOperationResult
SeparateImportedArtworkByGuide(CanvasState &state, int imported_artwork_id,
                               int guide_id);
bool DeleteImportedArtwork(CanvasState &state, int imported_artwork_id);

} // namespace im2d::operations