#pragma once

#include "im2d_canvas_types.h"

namespace im2d {

void InitializeDefaultDocument(CanvasState &state,
                               bool ensure_default_working_area = true);
int AddWorkingArea(CanvasState &state,
                   const WorkingAreaCreateInfo &create_info);
int AppendImportedArtwork(CanvasState &state, ImportedArtwork artwork,
                          bool auto_place = true);
void ClearImportedArtwork(CanvasState &state);
void ClearImportedDebugSelection(CanvasState &state);
void ClearSelectedImportedArtworkObjects(CanvasState &state);
void ClearSelectedImportedElements(CanvasState &state);
int CountSelectedImportedArtworkObjects(const CanvasState &state);
std::vector<int> GetSelectedImportedArtworkObjects(const CanvasState &state);
bool IsImportedArtworkObjectSelected(const CanvasState &state,
                                     int imported_artwork_id);
void SetSingleSelectedImportedArtworkObject(CanvasState &state,
                                            int imported_artwork_id);
bool AddSelectedImportedArtworkObject(CanvasState &state,
                                      int imported_artwork_id);
bool RemoveSelectedImportedArtworkObject(CanvasState &state,
                                         int imported_artwork_id);
void ApplyImportedArtworkSelectionScope(CanvasState &state,
                                        ImportedArtworkSelectionScope scope);
bool IsImportedElementSelected(const CanvasState &state, int artwork_id,
                               ImportedElementKind kind, int item_id);
void RefreshImportedArtworkPartMetadata(ImportedArtwork &artwork);
void RecomputeImportedArtworkBounds(ImportedArtwork &artwork);
void RecomputeImportedHierarchyBounds(ImportedArtwork &artwork);
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
ExportArea *FindExportArea(CanvasState &state, int export_area_id);
const ExportArea *FindExportArea(const CanvasState &state, int export_area_id);
void SyncExportAreaFromWorkingArea(CanvasState &state, int working_area_id);

} // namespace im2d
