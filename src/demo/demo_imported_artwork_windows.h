#pragma once

#include "../canvas/im2d_canvas.h"
#include "../import/im2d_import.h"

namespace demo {

void DrawImportedArtworkListWindow(im2d::CanvasState &state,
                                   const char *window_title);
void DrawImportedArtworkInspectorWindow(im2d::CanvasState &state,
                                        const char *window_title);
void DrawImportResultSummary(const im2d::importer::ImportResult &result);

} // namespace demo