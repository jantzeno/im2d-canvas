#pragma once

#include "../canvas/im2d_canvas.h"
#include "../import/im2d_import.h"

namespace demo {

void DrawImportedArtworkTransientUi(im2d::CanvasState &state);
void DrawImportedArtworkListContents(im2d::CanvasState &state);
void DrawImportedArtworkListWindow(im2d::CanvasState &state,
                                   const char *window_title);
void DrawImportedArtworkSvgExportContents(im2d::CanvasState &state);
void DrawImportedArtworkCanvasOperationsContents(im2d::CanvasState &state);
void DrawImportedArtworkCutOperationsContents(im2d::CanvasState &state);
void DrawImportedArtworkObjectInspectorSectionContents(
    im2d::CanvasState &state);
void DrawImportedArtworkInspectorContents(im2d::CanvasState &state);
void DrawImportedArtworkInspectorWindow(im2d::CanvasState &state,
                                        const char *window_title);
void DrawImportedArtworkWorkflowContents(im2d::CanvasState &state);
void DrawImportedArtworkWorkflowWindow(im2d::CanvasState &state,
                                       const char *window_title);
void DrawImportResultSummary(const im2d::importer::ImportResult &result);

} // namespace demo