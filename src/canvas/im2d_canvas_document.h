#pragma once

#include "im2d_canvas_types.h"

namespace im2d {

void InitializeDefaultDocument(CanvasState &state);
int AddWorkingArea(CanvasState &state,
                   const WorkingAreaCreateInfo &create_info);
int AppendImportedArtwork(CanvasState &state, ImportedArtwork artwork);
void ClearImportedArtwork(CanvasState &state);
void RecomputeImportedArtworkBounds(ImportedArtwork &artwork);
bool FlipImportedArtworkHorizontal(CanvasState &state, int imported_artwork_id);
bool FlipImportedArtworkVertical(CanvasState &state, int imported_artwork_id);
bool RotateImportedArtworkClockwise(CanvasState &state,
                                    int imported_artwork_id);
bool RotateImportedArtworkCounterClockwise(CanvasState &state,
                                           int imported_artwork_id);
bool DeleteImportedArtwork(CanvasState &state, int imported_artwork_id);
Guide *FindGuide(CanvasState &state, int guide_id);
const Guide *FindGuide(const CanvasState &state, int guide_id);
ImportedArtwork *FindImportedArtwork(CanvasState &state,
                                     int imported_artwork_id);
const ImportedArtwork *FindImportedArtwork(const CanvasState &state,
                                           int imported_artwork_id);
WorkingArea *FindWorkingArea(CanvasState &state, int working_area_id);
const WorkingArea *FindWorkingArea(const CanvasState &state,
                                   int working_area_id);
void SyncExportAreaFromWorkingArea(CanvasState &state, int working_area_id);

} // namespace im2d