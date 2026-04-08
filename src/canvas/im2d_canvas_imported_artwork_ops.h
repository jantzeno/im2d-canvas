#pragma once

#include "im2d_canvas_types.h"

#include <functional>

namespace im2d {

bool IsImportedArtworkScaleRatioLocked(const ImportedArtwork &artwork);
void UpdateImportedArtworkScaleFromTarget(ImportedArtwork &artwork,
                                          const ImVec2 &target_scale);
std::vector<int>
ResolveImportedArtworkOperationTargets(const CanvasState &state,
                                       int fallback_artwork_id = 0);
ImportedArtworkOperationResult ApplyImportedArtworkOperationToSelection(
    CanvasState &state, int fallback_artwork_id, const char *operation_name,
    const std::function<ImportedArtworkOperationResult(CanvasState &, int)>
        &operation);
bool HideAllImportedArtwork(CanvasState &state);
bool ShowAllImportedArtwork(CanvasState &state);
bool HideSelectedImportedArtwork(CanvasState &state);
bool IsolateSelectedImportedArtwork(CanvasState &state);
bool FlipImportedArtworkHorizontal(CanvasState &state, int imported_artwork_id);
bool FlipImportedArtworkVertical(CanvasState &state, int imported_artwork_id);
bool RotateImportedArtworkClockwise(CanvasState &state,
                                    int imported_artwork_id);
bool RotateImportedArtworkCounterClockwise(CanvasState &state,
                                           int imported_artwork_id);
ImportedArtworkOperationResult
JoinImportedArtworkOpenSegments(CanvasState &state, int imported_artwork_id,
                                float weld_tolerance = 0.5f);
ImportedArtworkOperationResult
AnalyzeImportedArtworkContours(CanvasState &state, int imported_artwork_id);
ImportedArtworkOperationResult
RepairImportedArtworkOrphanHoles(CanvasState &state, int imported_artwork_id);
bool HasExtractableImportedDebugSelection(const CanvasState &state,
                                          const ImportedArtwork &artwork);
bool HasGroupableImportedElementSelection(const CanvasState &state,
                                          const ImportedArtwork &artwork);
bool HasGroupableImportedRootSelection(const CanvasState &state,
                                       const ImportedArtwork &artwork);
bool HasGroupableImportedArtworkSelection(const CanvasState &state);
bool HasUngroupableImportedArtworkSelection(const CanvasState &state,
                                            const ImportedArtwork &artwork);
bool HasUngroupableImportedDebugSelection(const CanvasState &state,
                                          const ImportedArtwork &artwork);
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
ImportedArtworkOperationResult SelectImportedPathsInWorldRect(
    CanvasState &state, int imported_artwork_id, const ImVec2 &world_start,
    const ImVec2 &world_end, ImportedArtworkEditMode mode);
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
ExtractSelectedImportedElements(CanvasState &state, int imported_artwork_id);
bool CanCopySelectionToClipboard(const CanvasState &state);
bool HasClipboardContent(const CanvasState &state);
ImportedArtworkOperationResult CopySelectedToClipboard(CanvasState &state);
ImportedArtworkOperationResult CutSelectedToClipboard(CanvasState &state);
ImportedArtworkOperationResult PasteFromClipboard(CanvasState &state);
ImportedArtworkOperationResult
GroupSelectedImportedElements(CanvasState &state, int imported_artwork_id);
ImportedArtworkOperationResult
GroupImportedArtworkRootContents(CanvasState &state, int imported_artwork_id);
ImportedArtworkOperationResult
GroupSelectedImportedArtworkObjects(CanvasState &state);
ImportedArtworkOperationResult
UngroupSelectedImportedArtworkObjects(CanvasState &state);
ImportedArtworkOperationResult
UngroupSelectedImportedGroup(CanvasState &state, int imported_artwork_id);
ImportedArtworkOperationResult
SeparateImportedArtworkByGuide(CanvasState &state, int imported_artwork_id,
                               int guide_id,
                               bool create_groups_from_cuts = false);
ImportedArtworkOperationResult
DeleteSelectedImportedContent(CanvasState &state);
bool DeleteImportedArtwork(CanvasState &state, int imported_artwork_id);

} // namespace im2d