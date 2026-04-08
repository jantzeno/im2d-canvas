#pragma once

#include "im2d_canvas_types.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace im2d::detail {

// Shared helpers for artwork operation implementation files.
// These were previously anonymous-namespace functions in
// im2d_canvas_imported_artwork_ops.cpp.

bool ImportedArtworkTraceEnabled();
void TraceImportedArtworkStep(const std::string &message);

void SetLastImportedArtworkOperation(CanvasState &state,
                                     ImportedArtworkOperationResult result);
void SetLastImportedOperationIssueElements(
    CanvasState &state, int artwork_id,
    std::vector<ImportedElementSelection> issue_elements,
    bool highlight_on_canvas = true);

std::string ImportedArtworkOperationTargetLabel(const ImportedArtwork *artwork);

void AccumulateImportedArtworkOperationResult(
    ImportedArtworkOperationResult *aggregate,
    const ImportedArtworkOperationResult &result);

void SyncImportedArtworkSourceMetadata(ImportedArtwork *artwork);

void PopulateOperationReadiness(ImportedArtworkOperationResult *result,
                                const ImportedArtwork &artwork);

double ImportedPathSignedArea(const ImportedPath &path);

void CollectImportedIssueElements(
    const ImportedArtwork &artwork,
    std::vector<ImportedElementSelection> *issue_elements);

ImportedTextContour *FindImportedTextContourByIndex(ImportedDxfText &text,
                                                    int contour_index);

void NormalizeImportedSourceReferences(
    std::vector<ImportedSourceReference> *references);

void ResetImportedArtworkCounters(ImportedArtwork &artwork);

void ClearImportedArtworkPreviewStatesForArtwork(CanvasState &state,
                                                 int artwork_id);
void ClearImportedArtworkSeparationPreviewState(CanvasState &state);
void ClearImportedArtworkAutoCutPreviewState(CanvasState &state);

bool UngroupImportedGroupInPlace(ImportedArtwork *artwork, int group_id);

void RemoveImportedGroupReference(std::vector<int> *group_ids, int group_id);
void PruneEmptyGroups(ImportedArtwork &artwork);

int CountGroupableImportedRootItems(const ImportedArtwork &artwork);

void CollectAncestorGroupIds(const ImportedArtwork &artwork, int group_id,
                             std::vector<int> *ancestor_ids);

void FilterGroupReferences(ImportedArtwork &artwork,
                           const std::unordered_set<int> &retained_group_ids);

void TranslateImportedPathToWorld(const ImportedArtwork &source,
                                  const ImportedPath &path,
                                  ImportedPath *output);

void TranslateImportedDxfTextToWorld(const ImportedArtwork &source,
                                     const ImportedDxfText &text,
                                     ImportedDxfText *output);

ImportedArtwork BuildArtworkGroupFromSelection(const CanvasState &state,
                                               int imported_artwork_id);

ImportedArtwork BuildArtworkSubset(const ImportedArtwork &source,
                                   const std::unordered_set<int> &path_ids,
                                   const std::unordered_set<int> &text_ids,
                                   const std::string &name_suffix = "");

int ResolveGroupingParentGroupId(const ImportedArtwork &artwork, int group_id);

int CountGroupingTargets(const std::unordered_set<int> &group_ids,
                         const ImportedGroup &group);

void MoveImportedGroupingTargetsToGroup(
    ImportedArtwork *artwork, int source_group_id, int target_group_id,
    const std::unordered_set<int> &selected_path_ids,
    const std::unordered_set<int> &selected_text_ids,
    const std::unordered_set<int> &selected_group_ids);

} // namespace im2d::detail
