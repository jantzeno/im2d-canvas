#include "im2d_canvas_document.h"

#include "im2d_canvas_internal.h"

#include <clipper2/clipper.h>

#include "im2d_canvas_units.h"

#include <algorithm>
#include <cmath>

namespace im2d {

namespace {

constexpr double kMinimumPreparedContourArea = 0.01;

bool SameImportedSourceReference(const ImportedSourceReference &a,
                                 const ImportedSourceReference &b) {
  return a.source_artwork_id == b.source_artwork_id && a.kind == b.kind &&
         a.item_id == b.item_id;
}

bool ImportedSourceReferenceLess(const ImportedSourceReference &a,
                                 const ImportedSourceReference &b) {
  if (a.source_artwork_id != b.source_artwork_id) {
    return a.source_artwork_id < b.source_artwork_id;
  }
  if (a.kind != b.kind) {
    return static_cast<int>(a.kind) < static_cast<int>(b.kind);
  }
  return a.item_id < b.item_id;
}

void NormalizeImportedSourceReferences(
    std::vector<ImportedSourceReference> *references) {
  std::sort(references->begin(), references->end(),
            ImportedSourceReferenceLess);
  references->erase(std::unique(references->begin(), references->end(),
                                SameImportedSourceReference),
                    references->end());
}

void EnsureImportedPathProvenance(ImportedArtwork &artwork,
                                  ImportedPath *path) {
  if (path->provenance.empty()) {
    path->provenance.push_back(
        {artwork.part.source_artwork_id, ImportedElementKind::Path, path->id});
  }
  NormalizeImportedSourceReferences(&path->provenance);
}

void EnsureImportedDxfTextProvenance(ImportedArtwork &artwork,
                                     ImportedDxfText *text) {
  if (text->provenance.empty()) {
    text->provenance.push_back({artwork.part.source_artwork_id,
                                ImportedElementKind::DxfText, text->id});
  }
  NormalizeImportedSourceReferences(&text->provenance);
}

void EnsureImportedArtworkElementProvenance(ImportedArtwork &artwork) {
  for (ImportedPath &path : artwork.paths) {
    EnsureImportedPathProvenance(artwork, &path);
  }
  for (ImportedDxfText &text : artwork.dxf_text) {
    EnsureImportedDxfTextProvenance(artwork, &text);
  }
}

void EnsureContributingSourceArtworkId(ImportedArtwork &artwork) {
  if (artwork.part.source_artwork_id == 0) {
    artwork.part.source_artwork_id = artwork.id;
  }
  if (std::find(artwork.part.contributing_source_artwork_ids.begin(),
                artwork.part.contributing_source_artwork_ids.end(),
                artwork.part.source_artwork_id) ==
      artwork.part.contributing_source_artwork_ids.end()) {
    artwork.part.contributing_source_artwork_ids.push_back(
        artwork.part.source_artwork_id);
  }
}

struct ClosedContourCandidate {
  bool is_hole = false;
  ImportedElementKind owner_kind = ImportedElementKind::Path;
  int owner_item_id = 0;
  int contour_index = 0;
  Clipper2Lib::PathD polygon;
  Clipper2Lib::RectD bounds;
  double area = 0.0;
};

void AppendClosedContourCandidate(const Clipper2Lib::PathD &polygon,
                                  bool is_hole, ImportedElementKind owner_kind,
                                  int owner_item_id, int contour_index,
                                  std::vector<ClosedContourCandidate> *out) {
  if (polygon.size() < 3) {
    return;
  }
  const double area = std::fabs(Clipper2Lib::Area(polygon));
  if (area < kMinimumPreparedContourArea) {
    return;
  }
  out->push_back({is_hole, owner_kind, owner_item_id, contour_index, polygon,
                  Clipper2Lib::GetBounds(polygon), area});
}

bool RectContainsRect(const Clipper2Lib::RectD &outer,
                      const Clipper2Lib::RectD &inner) {
  return inner.left >= outer.left && inner.top >= outer.top &&
         inner.right <= outer.right && inner.bottom <= outer.bottom;
}

bool OuterContainsHole(const ClosedContourCandidate &outer,
                       const ClosedContourCandidate &hole) {
  if (!RectContainsRect(outer.bounds, hole.bounds) || hole.polygon.empty()) {
    return false;
  }
  const Clipper2Lib::PointD test_point = hole.polygon.front();
  const auto containment =
      Clipper2Lib::PointInPolygon(test_point, outer.polygon);
  return containment == Clipper2Lib::PointInPolygonResult::IsInside ||
         containment == Clipper2Lib::PointInPolygonResult::IsOn;
}

ImportedContourReference
MakeContourReference(const ClosedContourCandidate &candidate) {
  return {candidate.owner_kind, candidate.owner_item_id,
          candidate.contour_index};
}

double SampleImportedPathSignedArea(const ImportedPath &path) {
  if (!path.closed || path.segments.empty()) {
    return 0.0;
  }

  Clipper2Lib::PathD polygon = detail::SampleImportedPathToClipperPath(path);
  if (polygon.size() < 3) {
    return 0.0;
  }
  return Clipper2Lib::Area(polygon);
}

bool ImportedPathActsAsHole(const ImportedPath &path) {
  if (HasImportedPathFlag(path.flags, ImportedPathFlagHoleContour)) {
    return true;
  }
  if (!path.closed) {
    return false;
  }
  return SampleImportedPathSignedArea(path) < 0.0;
}

void InternalRecomputeImportedArtworkPartMetadata(ImportedArtwork &artwork) {
  constexpr uint32_t kPersistentPathIssueFlags =
      static_cast<uint32_t>(ImportedElementIssueFlagAmbiguousCleanup);

  artwork.part.outer_contours.clear();
  artwork.part.hole_attachments.clear();
  artwork.part.orphan_holes.clear();
  artwork.part.outer_contour_count = 0;
  artwork.part.hole_contour_count = 0;
  artwork.part.island_count = 0;
  artwork.part.attached_hole_count = 0;
  artwork.part.orphan_hole_count = 0;
  artwork.part.ambiguous_contour_count = 0;
  artwork.part.closed_contour_count = 0;
  artwork.part.open_contour_count = 0;
  artwork.part.placeholder_count = 0;
  std::vector<ClosedContourCandidate> closed_contours;

  const auto mark_item_issue = [&](ImportedElementKind kind, int item_id,
                                   ImportedElementIssueFlags flag) {
    if (kind == ImportedElementKind::Path) {
      for (ImportedPath &path : artwork.paths) {
        if (path.id == item_id) {
          path.issue_flags |= static_cast<uint32_t>(flag);
          return;
        }
      }
      return;
    }

    for (ImportedDxfText &text : artwork.dxf_text) {
      if (text.id == item_id) {
        text.issue_flags |= static_cast<uint32_t>(flag);
        return;
      }
    }
  };

  for (ImportedPath &path : artwork.paths) {
    path.issue_flags &= kPersistentPathIssueFlags;
    if (HasImportedElementIssueFlag(path.issue_flags,
                                    ImportedElementIssueFlagAmbiguousCleanup)) {
      artwork.part.ambiguous_contour_count += 1;
    }
  }
  for (ImportedDxfText &text : artwork.dxf_text) {
    text.issue_flags = ImportedElementIssueFlagNone;
  }

  for (ImportedPath &path : artwork.paths) {
    if (path.segments.empty()) {
      continue;
    }
    if (path.closed) {
      const bool is_hole = ImportedPathActsAsHole(path);
      artwork.part.closed_contour_count += 1;
      if (is_hole) {
        artwork.part.hole_contour_count += 1;
      } else {
        artwork.part.outer_contour_count += 1;
      }
      AppendClosedContourCandidate(
          detail::SampleImportedPathToClipperPath(path), is_hole,
          ImportedElementKind::Path, path.id, 0, &closed_contours);
    } else {
      artwork.part.open_contour_count += 1;
      path.issue_flags |= ImportedElementIssueFlagOpenGeometry;
    }
  }

  for (ImportedDxfText &text : artwork.dxf_text) {
    int contour_index = 0;
    if (text.placeholder_only) {
      artwork.part.placeholder_count += 1;
      text.issue_flags |= ImportedElementIssueFlagPlaceholderText;
    }

    for (const ImportedTextGlyph &glyph : text.glyphs) {
      for (const ImportedTextContour &contour : glyph.contours) {
        if (contour.role == ImportedTextContourRole::Guide ||
            contour.segments.empty()) {
          continue;
        }
        if (contour.closed) {
          const bool is_hole = contour.role == ImportedTextContourRole::Hole;
          artwork.part.closed_contour_count += 1;
          if (is_hole) {
            artwork.part.hole_contour_count += 1;
          } else {
            artwork.part.outer_contour_count += 1;
          }
          AppendClosedContourCandidate(
              detail::SampleImportedTextContourToClipperPath(contour), is_hole,
              ImportedElementKind::DxfText, text.id, contour_index,
              &closed_contours);
        } else {
          artwork.part.open_contour_count += 1;
          text.issue_flags |= ImportedElementIssueFlagOpenGeometry;
        }
        contour_index += 1;
      }
    }

    for (const ImportedTextContour &contour : text.placeholder_contours) {
      if (contour.role == ImportedTextContourRole::Guide ||
          contour.segments.empty()) {
        continue;
      }
      if (contour.closed) {
        const bool is_hole = contour.role == ImportedTextContourRole::Hole;
        artwork.part.closed_contour_count += 1;
        if (is_hole) {
          artwork.part.hole_contour_count += 1;
        } else {
          artwork.part.outer_contour_count += 1;
        }
        AppendClosedContourCandidate(
            detail::SampleImportedTextContourToClipperPath(contour), is_hole,
            ImportedElementKind::DxfText, text.id, contour_index,
            &closed_contours);
      } else {
        artwork.part.open_contour_count += 1;
        text.issue_flags |= ImportedElementIssueFlagOpenGeometry;
      }
      contour_index += 1;
    }
  }

  std::vector<const ClosedContourCandidate *> outers;
  std::vector<const ClosedContourCandidate *> holes;
  for (const ClosedContourCandidate &candidate : closed_contours) {
    if (candidate.is_hole) {
      holes.push_back(&candidate);
    } else {
      outers.push_back(&candidate);
      artwork.part.outer_contours.push_back(MakeContourReference(candidate));
    }
  }
  artwork.part.island_count = static_cast<int>(outers.size());

  for (const ClosedContourCandidate *hole : holes) {
    const ClosedContourCandidate *best_outer = nullptr;
    for (const ClosedContourCandidate *outer : outers) {
      if (!OuterContainsHole(*outer, *hole)) {
        continue;
      }
      if (best_outer == nullptr || outer->area < best_outer->area) {
        best_outer = outer;
      }
    }
    if (best_outer != nullptr) {
      artwork.part.attached_hole_count += 1;
      artwork.part.hole_attachments.push_back(
          {MakeContourReference(*best_outer), MakeContourReference(*hole)});
    } else {
      artwork.part.orphan_hole_count += 1;
      artwork.part.orphan_holes.push_back(MakeContourReference(*hole));
      mark_item_issue(hole->owner_kind, hole->owner_item_id,
                      ImportedElementIssueFlagOrphanHole);
    }
  }

  artwork.part.cut_ready = artwork.part.outer_contour_count > 0 &&
                           artwork.part.open_contour_count == 0 &&
                           artwork.part.placeholder_count == 0 &&
                           artwork.part.orphan_hole_count == 0;
  artwork.part.nest_ready = artwork.part.cut_ready;
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

void RefreshImportedArtworkPartMetadata(ImportedArtwork &artwork) {
  InternalRecomputeImportedArtworkPartMetadata(artwork);
}

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

void ClearSelectedImportedElements(CanvasState &state) {
  state.selected_imported_elements.clear();
}

bool IsImportedElementSelected(const CanvasState &state, int artwork_id,
                               ImportedElementKind kind, int item_id) {
  if (state.selected_imported_artwork_id != artwork_id) {
    return false;
  }

  return std::any_of(
      state.selected_imported_elements.begin(),
      state.selected_imported_elements.end(),
      [kind, item_id](const ImportedElementSelection &selection) {
        return selection.kind == kind && selection.item_id == item_id;
      });
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

ImportedDxfText *FindImportedDxfText(ImportedArtwork &artwork, int text_id) {
  auto it = std::find_if(
      artwork.dxf_text.begin(), artwork.dxf_text.end(),
      [text_id](const ImportedDxfText &text) { return text.id == text_id; });
  return it == artwork.dxf_text.end() ? nullptr : &(*it);
}

const ImportedDxfText *FindImportedDxfText(const ImportedArtwork &artwork,
                                           int text_id) {
  auto it = std::find_if(
      artwork.dxf_text.begin(), artwork.dxf_text.end(),
      [text_id](const ImportedDxfText &text) { return text.id == text_id; });
  return it == artwork.dxf_text.end() ? nullptr : &(*it);
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
  if (artwork.part.part_id == 0) {
    artwork.part.part_id = state.next_imported_part_id++;
  }
  if (artwork.part.source_artwork_id == 0) {
    artwork.part.source_artwork_id = artwork.id;
  }
  EnsureContributingSourceArtworkId(artwork);
  EnsureImportedArtworkElementProvenance(artwork);

  if (!state.working_areas.empty()) {
    const float stagger =
        24.0f * static_cast<float>(state.imported_artwork.size());
    artwork.origin.x += state.working_areas.front().origin.x + stagger;
    artwork.origin.y += state.working_areas.front().origin.y + stagger;
  }

  RecomputeImportedHierarchyBounds(artwork);
  RefreshImportedArtworkPartMetadata(artwork);

  state.imported_artwork.push_back(std::move(artwork));
  return state.imported_artwork.back().id;
}

void ClearImportedArtwork(CanvasState &state) {
  state.imported_artwork.clear();
  state.selected_imported_artwork_id = 0;
  ClearImportedDebugSelection(state);
  ClearSelectedImportedElements(state);
}

void RecomputeImportedArtworkBounds(ImportedArtwork &artwork) {
  const detail::ImportedArtworkBounds bounds =
      detail::ComputeImportedArtworkBounds(artwork);
  if (!bounds.valid) {
    artwork.bounds_min = ImVec2(0.0f, 0.0f);
    artwork.bounds_max = ImVec2(1.0f, 1.0f);
    return;
  }

  const ImVec2 offset = bounds.min;
  detail::ForEachImportedArtworkPoint(artwork, [&offset](ImVec2 &point) {
    point.x -= offset.x;
    point.y -= offset.y;
  });

  artwork.bounds_min = ImVec2(0.0f, 0.0f);
  artwork.bounds_max = ImVec2(std::max(bounds.max.x - bounds.min.x, 1.0f),
                              std::max(bounds.max.y - bounds.min.y, 1.0f));
}

void RecomputeImportedHierarchyBounds(ImportedArtwork &artwork) {
  for (ImportedPath &path : artwork.paths) {
    const detail::ImportedArtworkBounds path_bounds =
        detail::ComputeImportedPathBounds(path);
    if (path_bounds.valid) {
      path.bounds_min = path_bounds.min;
      path.bounds_max = path_bounds.max;
    } else {
      path.bounds_min = ImVec2(0.0f, 0.0f);
      path.bounds_max = ImVec2(0.0f, 0.0f);
    }
  }

  for (ImportedDxfText &text : artwork.dxf_text) {
    for (ImportedTextGlyph &glyph : text.glyphs) {
      detail::ImportedArtworkBounds glyph_bounds;
      for (ImportedTextContour &contour : glyph.contours) {
        const detail::ImportedArtworkBounds contour_bounds =
            detail::ComputeImportedTextContourBounds(contour);
        if (contour_bounds.valid) {
          contour.bounds_min = contour_bounds.min;
          contour.bounds_max = contour_bounds.max;
          detail::IncludePoint(glyph_bounds, contour_bounds.min);
          detail::IncludePoint(glyph_bounds, contour_bounds.max);
        } else {
          contour.bounds_min = ImVec2(0.0f, 0.0f);
          contour.bounds_max = ImVec2(0.0f, 0.0f);
        }
      }

      if (glyph_bounds.valid) {
        glyph.bounds_min = glyph_bounds.min;
        glyph.bounds_max = glyph_bounds.max;
      } else {
        glyph.bounds_min = ImVec2(0.0f, 0.0f);
        glyph.bounds_max = ImVec2(0.0f, 0.0f);
      }
    }

    for (ImportedTextContour &contour : text.placeholder_contours) {
      const detail::ImportedArtworkBounds contour_bounds =
          detail::ComputeImportedTextContourBounds(contour);
      if (contour_bounds.valid) {
        contour.bounds_min = contour_bounds.min;
        contour.bounds_max = contour_bounds.max;
      } else {
        contour.bounds_min = ImVec2(0.0f, 0.0f);
        contour.bounds_max = ImVec2(0.0f, 0.0f);
      }
    }

    const detail::ImportedArtworkBounds text_bounds =
        detail::ComputeImportedDxfTextBounds(text);
    if (text_bounds.valid) {
      text.bounds_min = text_bounds.min;
      text.bounds_max = text_bounds.max;
    } else {
      text.bounds_min = ImVec2(0.0f, 0.0f);
      text.bounds_max = ImVec2(0.0f, 0.0f);
    }
  }

  for (ImportedGroup &group : artwork.groups) {
    const detail::ImportedArtworkBounds group_bounds =
        detail::ComputeImportedGroupBounds(artwork, group);
    if (group_bounds.valid) {
      group.bounds_min = group_bounds.min;
      group.bounds_max = group_bounds.max;
    } else {
      group.bounds_min = ImVec2(0.0f, 0.0f);
      group.bounds_max = ImVec2(0.0f, 0.0f);
    }
  }
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