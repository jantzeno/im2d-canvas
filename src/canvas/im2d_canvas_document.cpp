#include "im2d_canvas_document.h"

#include "../common/im2d_log.h"

#include "im2d_canvas_units.h"

#include <algorithm>
#include <cmath>

namespace im2d {

namespace {

constexpr float kMinimumImportedArtworkScale = 0.01f;

template <typename Function>
void ForEachImportedArtworkPoint(ImportedArtwork &artwork,
                                 Function &&function) {
  for (ImportedPath &path : artwork.paths) {
    for (ImportedPathSegment &segment : path.segments) {
      function(segment.start);
      if (segment.kind == ImportedPathSegmentKind::CubicBezier) {
        function(segment.control1);
        function(segment.control2);
      }
      function(segment.end);
    }
  }
}

struct ImportedArtworkBounds {
  ImVec2 min = ImVec2(0.0f, 0.0f);
  ImVec2 max = ImVec2(0.0f, 0.0f);
  bool valid = false;
};

void IncludePoint(ImportedArtworkBounds &bounds, const ImVec2 &point) {
  if (!bounds.valid) {
    bounds.min = point;
    bounds.max = point;
    bounds.valid = true;
    return;
  }

  bounds.min.x = std::min(bounds.min.x, point.x);
  bounds.min.y = std::min(bounds.min.y, point.y);
  bounds.max.x = std::max(bounds.max.x, point.x);
  bounds.max.y = std::max(bounds.max.y, point.y);
}

ImportedArtworkBounds ComputeImportedPathBounds(const ImportedPath &path) {
  ImportedArtworkBounds bounds;
  for (const ImportedPathSegment &segment : path.segments) {
    IncludePoint(bounds, segment.start);
    if (segment.kind == ImportedPathSegmentKind::CubicBezier) {
      IncludePoint(bounds, segment.control1);
      IncludePoint(bounds, segment.control2);
    }
    IncludePoint(bounds, segment.end);
  }
  return bounds;
}

ImportedArtworkBounds
ComputeImportedArtworkBounds(const ImportedArtwork &artwork) {
  ImportedArtworkBounds bounds;
  for (const ImportedPath &path : artwork.paths) {
    for (const ImportedPathSegment &segment : path.segments) {
      IncludePoint(bounds, segment.start);
      if (segment.kind == ImportedPathSegmentKind::CubicBezier) {
        IncludePoint(bounds, segment.control1);
        IncludePoint(bounds, segment.control2);
      }
      IncludePoint(bounds, segment.end);
    }
  }

  return bounds;
}

ImVec2 ImportedArtworkLocalSize(const ImportedArtwork &artwork) {
  return ImVec2(std::max(artwork.bounds_max.x - artwork.bounds_min.x, 1.0f),
                std::max(artwork.bounds_max.y - artwork.bounds_min.y, 1.0f));
}

ImVec2 ImportedArtworkScaledSize(const ImportedArtwork &artwork) {
  const ImVec2 local_size = ImportedArtworkLocalSize(artwork);
  return ImVec2(std::max(local_size.x * artwork.scale.x, 1.0f),
                std::max(local_size.y * artwork.scale.y, 1.0f));
}

ImportedArtworkBounds ComputeImportedGroupBounds(const ImportedArtwork &artwork,
                                                 const ImportedGroup &group) {
  ImportedArtworkBounds bounds;

  for (const int path_id : group.path_ids) {
    const ImportedPath *path = FindImportedPath(artwork, path_id);
    if (path == nullptr) {
      continue;
    }

    const ImportedArtworkBounds path_bounds = ComputeImportedPathBounds(*path);
    if (!path_bounds.valid) {
      continue;
    }

    IncludePoint(bounds, path_bounds.min);
    IncludePoint(bounds, path_bounds.max);
  }

  for (const int child_group_id : group.child_group_ids) {
    const ImportedGroup *child_group =
        FindImportedGroup(artwork, child_group_id);
    if (child_group == nullptr) {
      continue;
    }

    const ImportedArtworkBounds child_bounds =
        ComputeImportedGroupBounds(artwork, *child_group);
    if (!child_bounds.valid) {
      continue;
    }

    IncludePoint(bounds, child_bounds.min);
    IncludePoint(bounds, child_bounds.max);
  }

  return bounds;
}

template <typename Function>
bool TransformImportedArtwork(CanvasState &state, int imported_artwork_id,
                              Function &&transform, const char *action_name) {
  ImportedArtwork *artwork = FindImportedArtwork(state, imported_artwork_id);
  if (artwork == nullptr) {
    return false;
  }

  const ImVec2 size = ImportedArtworkLocalSize(*artwork);
  const ImVec2 original_scaled_size = ImportedArtworkScaledSize(*artwork);
  const ImVec2 world_center(artwork->origin.x + original_scaled_size.x * 0.5f,
                            artwork->origin.y + original_scaled_size.y * 0.5f);

  ForEachImportedArtworkPoint(*artwork, [&](ImVec2 &point) {
    const ImVec2 local(point.x - artwork->bounds_min.x,
                       point.y - artwork->bounds_min.y);
    point = transform(local, size);
  });

  RecomputeImportedArtworkBounds(*artwork);
  RecomputeImportedHierarchyBounds(*artwork);
  const ImVec2 new_scaled_size = ImportedArtworkScaledSize(*artwork);
  artwork->origin = ImVec2(world_center.x - new_scaled_size.x * 0.5f,
                           world_center.y - new_scaled_size.y * 0.5f);

  log::GetLogger()->info("{} imported artwork id={} name='{}'", action_name,
                         artwork->id, artwork->name);
  return true;
}

ExportArea *FindExportAreaBySourceWorkingAreaId(CanvasState &state,
                                                int working_area_id) {
  auto it =
      std::find_if(state.export_areas.begin(), state.export_areas.end(),
                   [working_area_id](const ExportArea &area) {
                     return area.source_working_area_id == working_area_id;
                   });
  return it == state.export_areas.end() ? nullptr : &(*it);
}

} // namespace

ImVec2 ImportedArtworkPointToWorld(const ImportedArtwork &artwork,
                                   const ImVec2 &point) {
  return ImVec2(
      artwork.origin.x + (point.x - artwork.bounds_min.x) * artwork.scale.x,
      artwork.origin.y + (point.y - artwork.bounds_min.y) * artwork.scale.y);
}

void ImportedLocalBoundsToWorldBounds(const ImportedArtwork &artwork,
                                      const ImVec2 &local_min,
                                      const ImVec2 &local_max,
                                      ImVec2 *world_min, ImVec2 *world_max) {
  const ImVec2 world_min_value =
      ImportedArtworkPointToWorld(artwork, local_min);
  const ImVec2 world_max_value =
      ImportedArtworkPointToWorld(artwork, local_max);
  if (world_min != nullptr) {
    *world_min = ImVec2(std::min(world_min_value.x, world_max_value.x),
                        std::min(world_min_value.y, world_max_value.y));
  }
  if (world_max != nullptr) {
    *world_max = ImVec2(std::max(world_min_value.x, world_max_value.x),
                        std::max(world_min_value.y, world_max_value.y));
  }
}

void ClearImportedDebugSelection(CanvasState &state) {
  state.selected_imported_debug = {};
}

bool IsImportedArtworkScaleRatioLocked(const ImportedArtwork &artwork) {
  return HasImportedArtworkFlag(artwork.flags,
                                ImportedArtworkFlagLockScaleRatio);
}

void SetImportedArtworkScaleRatioLocked(ImportedArtwork &artwork, bool locked) {
  if (locked) {
    artwork.flags |= static_cast<uint32_t>(ImportedArtworkFlagLockScaleRatio);
    return;
  }

  artwork.flags &= ~static_cast<uint32_t>(ImportedArtworkFlagLockScaleRatio);
}

void UpdateImportedArtworkScaleAxis(ImportedArtwork &artwork, int axis,
                                    float new_value) {
  const float clamped_value = std::max(new_value, kMinimumImportedArtworkScale);
  const bool lock_ratio = IsImportedArtworkScaleRatioLocked(artwork);

  if (!lock_ratio) {
    if (axis == 0) {
      artwork.scale.x = clamped_value;
    } else {
      artwork.scale.y = clamped_value;
    }
    return;
  }

  const float old_x = std::max(artwork.scale.x, kMinimumImportedArtworkScale);
  const float old_y = std::max(artwork.scale.y, kMinimumImportedArtworkScale);
  if (axis == 0) {
    const float factor = clamped_value / old_x;
    artwork.scale.x = clamped_value;
    artwork.scale.y = std::max(old_y * factor, kMinimumImportedArtworkScale);
    return;
  }

  const float factor = clamped_value / old_y;
  artwork.scale.y = clamped_value;
  artwork.scale.x = std::max(old_x * factor, kMinimumImportedArtworkScale);
}

void UpdateImportedArtworkScaleFromTarget(ImportedArtwork &artwork,
                                          const ImVec2 &target_scale) {
  const ImVec2 clamped_scale(
      std::max(target_scale.x, kMinimumImportedArtworkScale),
      std::max(target_scale.y, kMinimumImportedArtworkScale));
  if (!IsImportedArtworkScaleRatioLocked(artwork)) {
    artwork.scale = clamped_scale;
    return;
  }

  const float old_x = std::max(artwork.scale.x, kMinimumImportedArtworkScale);
  const float old_y = std::max(artwork.scale.y, kMinimumImportedArtworkScale);
  const float factor_x = clamped_scale.x / old_x;
  const float factor_y = clamped_scale.y / old_y;
  const float chosen_factor =
      std::abs(factor_x - 1.0f) >= std::abs(factor_y - 1.0f) ? factor_x
                                                             : factor_y;
  artwork.scale.x =
      std::max(old_x * chosen_factor, kMinimumImportedArtworkScale);
  artwork.scale.y =
      std::max(old_y * chosen_factor, kMinimumImportedArtworkScale);
}

Guide *FindGuide(CanvasState &state, int guide_id) {
  auto it = std::find_if(
      state.guides.begin(), state.guides.end(),
      [guide_id](const Guide &guide) { return guide.id == guide_id; });
  return it == state.guides.end() ? nullptr : &(*it);
}

const Guide *FindGuide(const CanvasState &state, int guide_id) {
  auto it = std::find_if(
      state.guides.begin(), state.guides.end(),
      [guide_id](const Guide &guide) { return guide.id == guide_id; });
  return it == state.guides.end() ? nullptr : &(*it);
}

ImportedArtwork *FindImportedArtwork(CanvasState &state,
                                     int imported_artwork_id) {
  auto it =
      std::find_if(state.imported_artwork.begin(), state.imported_artwork.end(),
                   [imported_artwork_id](const ImportedArtwork &artwork) {
                     return artwork.id == imported_artwork_id;
                   });
  return it == state.imported_artwork.end() ? nullptr : &(*it);
}

const ImportedArtwork *FindImportedArtwork(const CanvasState &state,
                                           int imported_artwork_id) {
  auto it =
      std::find_if(state.imported_artwork.begin(), state.imported_artwork.end(),
                   [imported_artwork_id](const ImportedArtwork &artwork) {
                     return artwork.id == imported_artwork_id;
                   });
  return it == state.imported_artwork.end() ? nullptr : &(*it);
}

ImportedGroup *FindImportedGroup(ImportedArtwork &artwork, int group_id) {
  auto it = std::find_if(
      artwork.groups.begin(), artwork.groups.end(),
      [group_id](const ImportedGroup &group) { return group.id == group_id; });
  return it == artwork.groups.end() ? nullptr : &(*it);
}

const ImportedGroup *FindImportedGroup(const ImportedArtwork &artwork,
                                       int group_id) {
  auto it = std::find_if(
      artwork.groups.begin(), artwork.groups.end(),
      [group_id](const ImportedGroup &group) { return group.id == group_id; });
  return it == artwork.groups.end() ? nullptr : &(*it);
}

ImportedPath *FindImportedPath(ImportedArtwork &artwork, int path_id) {
  auto it = std::find_if(
      artwork.paths.begin(), artwork.paths.end(),
      [path_id](const ImportedPath &path) { return path.id == path_id; });
  return it == artwork.paths.end() ? nullptr : &(*it);
}

const ImportedPath *FindImportedPath(const ImportedArtwork &artwork,
                                     int path_id) {
  auto it = std::find_if(
      artwork.paths.begin(), artwork.paths.end(),
      [path_id](const ImportedPath &path) { return path.id == path_id; });
  return it == artwork.paths.end() ? nullptr : &(*it);
}

WorkingArea *FindWorkingArea(CanvasState &state, int working_area_id) {
  auto it = std::find_if(state.working_areas.begin(), state.working_areas.end(),
                         [working_area_id](const WorkingArea &area) {
                           return area.id == working_area_id;
                         });
  return it == state.working_areas.end() ? nullptr : &(*it);
}

const WorkingArea *FindWorkingArea(const CanvasState &state,
                                   int working_area_id) {
  auto it = std::find_if(state.working_areas.begin(), state.working_areas.end(),
                         [working_area_id](const WorkingArea &area) {
                           return area.id == working_area_id;
                         });
  return it == state.working_areas.end() ? nullptr : &(*it);
}

void SyncExportAreaFromWorkingArea(CanvasState &state, int working_area_id) {
  const WorkingArea *working_area = FindWorkingArea(state, working_area_id);
  ExportArea *export_area =
      FindExportAreaBySourceWorkingAreaId(state, working_area_id);
  if (working_area == nullptr || export_area == nullptr) {
    return;
  }

  export_area->origin = working_area->origin;
  export_area->size = working_area->size;
  export_area->visible = working_area->visible;
}

int AddWorkingArea(CanvasState &state,
                   const WorkingAreaCreateInfo &create_info) {
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

int AppendImportedArtwork(CanvasState &state, ImportedArtwork artwork) {
  artwork.id = state.next_imported_artwork_id++;
  if (artwork.name.empty()) {
    artwork.name = "Artwork " + std::to_string(artwork.id);
  }

  if (!state.working_areas.empty()) {
    const float stagger =
        24.0f * static_cast<float>(state.imported_artwork.size());
    artwork.origin.x += state.working_areas.front().origin.x + stagger;
    artwork.origin.y += state.working_areas.front().origin.y + stagger;
  }

  RecomputeImportedHierarchyBounds(artwork);

  state.imported_artwork.push_back(std::move(artwork));
  return state.imported_artwork.back().id;
}

void ClearImportedArtwork(CanvasState &state) {
  state.imported_artwork.clear();
  state.selected_imported_artwork_id = 0;
  ClearImportedDebugSelection(state);
}

void RecomputeImportedArtworkBounds(ImportedArtwork &artwork) {
  const ImportedArtworkBounds bounds = ComputeImportedArtworkBounds(artwork);
  if (!bounds.valid) {
    artwork.bounds_min = ImVec2(0.0f, 0.0f);
    artwork.bounds_max = ImVec2(1.0f, 1.0f);
    return;
  }

  const ImVec2 offset = bounds.min;
  ForEachImportedArtworkPoint(artwork, [&offset](ImVec2 &point) {
    point.x -= offset.x;
    point.y -= offset.y;
  });

  artwork.bounds_min = ImVec2(0.0f, 0.0f);
  artwork.bounds_max = ImVec2(std::max(bounds.max.x - bounds.min.x, 1.0f),
                              std::max(bounds.max.y - bounds.min.y, 1.0f));
}

void RecomputeImportedHierarchyBounds(ImportedArtwork &artwork) {
  for (ImportedPath &path : artwork.paths) {
    const ImportedArtworkBounds path_bounds = ComputeImportedPathBounds(path);
    if (path_bounds.valid) {
      path.bounds_min = path_bounds.min;
      path.bounds_max = path_bounds.max;
    } else {
      path.bounds_min = ImVec2(0.0f, 0.0f);
      path.bounds_max = ImVec2(0.0f, 0.0f);
    }
  }

  for (ImportedGroup &group : artwork.groups) {
    const ImportedArtworkBounds group_bounds =
        ComputeImportedGroupBounds(artwork, group);
    if (group_bounds.valid) {
      group.bounds_min = group_bounds.min;
      group.bounds_max = group_bounds.max;
    } else {
      group.bounds_min = ImVec2(0.0f, 0.0f);
      group.bounds_max = ImVec2(0.0f, 0.0f);
    }
  }
}

bool FlipImportedArtworkHorizontal(CanvasState &state,
                                   int imported_artwork_id) {
  return TransformImportedArtwork(
      state, imported_artwork_id,
      [](const ImVec2 &local, const ImVec2 &size) {
        return ImVec2(size.x - local.x, local.y);
      },
      "Flipped horizontally");
}

bool FlipImportedArtworkVertical(CanvasState &state, int imported_artwork_id) {
  return TransformImportedArtwork(
      state, imported_artwork_id,
      [](const ImVec2 &local, const ImVec2 &size) {
        return ImVec2(local.x, size.y - local.y);
      },
      "Flipped vertically");
}

bool RotateImportedArtworkClockwise(CanvasState &state,
                                    int imported_artwork_id) {
  return TransformImportedArtwork(
      state, imported_artwork_id,
      [](const ImVec2 &local, const ImVec2 &size) {
        const ImVec2 center(size.x * 0.5f, size.y * 0.5f);
        const ImVec2 delta(local.x - center.x, local.y - center.y);
        return ImVec2(center.x + delta.y, center.y - delta.x);
      },
      "Rotated 90 CW");
}

bool RotateImportedArtworkCounterClockwise(CanvasState &state,
                                           int imported_artwork_id) {
  return TransformImportedArtwork(
      state, imported_artwork_id,
      [](const ImVec2 &local, const ImVec2 &size) {
        const ImVec2 center(size.x * 0.5f, size.y * 0.5f);
        const ImVec2 delta(local.x - center.x, local.y - center.y);
        return ImVec2(center.x - delta.y, center.y + delta.x);
      },
      "Rotated 90 CCW");
}

bool DeleteImportedArtwork(CanvasState &state, int imported_artwork_id) {
  auto it =
      std::find_if(state.imported_artwork.begin(), state.imported_artwork.end(),
                   [imported_artwork_id](const ImportedArtwork &artwork) {
                     return artwork.id == imported_artwork_id;
                   });
  if (it == state.imported_artwork.end()) {
    return false;
  }

  log::GetLogger()->info("Deleted imported artwork id={} name='{}'", it->id,
                         it->name);
  state.imported_artwork.erase(it);
  if (state.selected_imported_artwork_id == imported_artwork_id) {
    state.selected_imported_artwork_id = 0;
  }
  if (state.selected_imported_debug.artwork_id == imported_artwork_id) {
    ClearImportedDebugSelection(state);
  }
  return true;
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