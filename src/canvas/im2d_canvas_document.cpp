#include "im2d_canvas_document.h"

#include "im2d_canvas_units.h"

#include <algorithm>

namespace im2d {

namespace {

ExportArea *FindExportAreaBySourceWorkingAreaId(CanvasState &state,
                                                int working_area_id) {
  auto it = std::find_if(state.export_areas.begin(), state.export_areas.end(),
                         [working_area_id](const ExportArea &area) {
                           return area.source_working_area_id == working_area_id;
                         });
  return it == state.export_areas.end() ? nullptr : &(*it);
}

} // namespace

Guide *FindGuide(CanvasState &state, int guide_id) {
  auto it = std::find_if(state.guides.begin(), state.guides.end(),
                         [guide_id](const Guide &guide) {
                           return guide.id == guide_id;
                         });
  return it == state.guides.end() ? nullptr : &(*it);
}

const Guide *FindGuide(const CanvasState &state, int guide_id) {
  auto it = std::find_if(state.guides.begin(), state.guides.end(),
                         [guide_id](const Guide &guide) {
                           return guide.id == guide_id;
                         });
  return it == state.guides.end() ? nullptr : &(*it);
}

WorkingArea *FindWorkingArea(CanvasState &state, int working_area_id) {
  auto it = std::find_if(state.working_areas.begin(), state.working_areas.end(),
                         [working_area_id](const WorkingArea &area) {
                           return area.id == working_area_id;
                         });
  return it == state.working_areas.end() ? nullptr : &(*it);
}

const WorkingArea *FindWorkingArea(const CanvasState &state, int working_area_id) {
  auto it = std::find_if(state.working_areas.begin(), state.working_areas.end(),
                         [working_area_id](const WorkingArea &area) {
                           return area.id == working_area_id;
                         });
  return it == state.working_areas.end() ? nullptr : &(*it);
}

void SyncExportAreaFromWorkingArea(CanvasState &state, int working_area_id) {
  const WorkingArea *working_area = FindWorkingArea(state, working_area_id);
  ExportArea *export_area = FindExportAreaBySourceWorkingAreaId(state, working_area_id);
  if (working_area == nullptr || export_area == nullptr) {
    return;
  }

  export_area->origin = working_area->origin;
  export_area->size = working_area->size;
  export_area->visible = working_area->visible;
}

int AddWorkingArea(CanvasState &state, const WorkingAreaCreateInfo &create_info) {
  WorkingArea area;
  area.id = state.next_working_area_id++;
  area.name = create_info.name.empty()
                  ? "Working Area " + std::to_string(area.id)
                  : create_info.name;
  area.size = ImVec2(std::max(create_info.size_pixels.x, 1.0f),
                     std::max(create_info.size_pixels.y, 1.0f));
  area.flags = create_info.flags;
  const float stagger = 32.0f * static_cast<float>(state.working_areas.size());
  area.origin = ImVec2(stagger, stagger);
  state.working_areas.push_back(area);

  ExportArea export_area;
  export_area.id = state.next_export_area_id++;
  export_area.source_working_area_id = area.id;
  export_area.origin = area.origin;
  export_area.size = area.size;
  state.export_areas.push_back(export_area);
  return area.id;
}

void InitializeDefaultDocument(CanvasState &state) {
  if (state.layers.empty()) {
    state.layers.push_back(Layer{state.next_layer_id++, "Root", true, false});
  }

  if (state.working_areas.empty()) {
    WorkingAreaCreateInfo create_info;
    create_info.name = "Working Area 1";
    create_info.size_pixels = ImVec2(
        UnitsToPixels(210.0f, MeasurementUnit::Millimeters, state.calibration),
        UnitsToPixels(297.0f, MeasurementUnit::Millimeters, state.calibration));
    create_info.flags = kDefaultWorkingAreaFlags;
    AddWorkingArea(state, create_info);
  }
}

} // namespace im2d