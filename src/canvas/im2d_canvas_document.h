#pragma once

#include "im2d_canvas_types.h"

namespace im2d {

void InitializeDefaultDocument(CanvasState &state);
int AddWorkingArea(CanvasState &state,
                   const WorkingAreaCreateInfo &create_info);
Guide *FindGuide(CanvasState &state, int guide_id);
const Guide *FindGuide(const CanvasState &state, int guide_id);
WorkingArea *FindWorkingArea(CanvasState &state, int working_area_id);
const WorkingArea *FindWorkingArea(const CanvasState &state,
                                   int working_area_id);
void SyncExportAreaFromWorkingArea(CanvasState &state, int working_area_id);

} // namespace im2d