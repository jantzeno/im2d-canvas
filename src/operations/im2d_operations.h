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
GroupSelectedImportedElements(CanvasState &state, int imported_artwork_id);
ImportedArtworkOperationResult
GroupImportedArtworkRootContents(CanvasState &state, int imported_artwork_id);
ImportedArtworkOperationResult
UngroupSelectedImportedGroup(CanvasState &state, int imported_artwork_id);
ImportedArtworkOperationResult
PreviewSeparateImportedArtworkByGuide(CanvasState &state,
                                      int imported_artwork_id, int guide_id);
void ClearImportedArtworkSeparationPreview(CanvasState &state);
ImportedArtworkOperationResult PreviewImportedArtworkAutoCut(
    CanvasState &state, int imported_artwork_id,
    AutoCutPreviewAxisMode axis_mode = AutoCutPreviewAxisMode::Both,
    float minimum_gap = 5.0f);
ImportedArtworkOperationResult ApplyImportedArtworkAutoCut(
    CanvasState &state, int imported_artwork_id,
    AutoCutPreviewAxisMode axis_mode = AutoCutPreviewAxisMode::Both,
    float minimum_gap = 5.0f, bool create_groups_from_cuts = false);
void ClearImportedArtworkAutoCutPreview(CanvasState &state);
ImportedArtworkOperationResult
SeparateImportedArtworkByGuide(CanvasState &state, int imported_artwork_id,
                               int guide_id,
                               bool create_groups_from_cuts = false);
bool DeleteImportedArtwork(CanvasState &state, int imported_artwork_id);

} // namespace im2d::operations