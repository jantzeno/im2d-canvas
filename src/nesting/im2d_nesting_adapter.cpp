#include "im2d_nesting_adapter.h"

#include "../canvas/im2d_canvas_document.h"
#include "../canvas/im2d_canvas_internal.h"
#include "im2d_nesting_geometry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>

namespace im2d::nesting {

namespace {

constexpr std::size_t kSimplifyVertexThreshold = 24;
constexpr double kSimplifyDiagonalFraction = 0.0003;
constexpr double kSimplifyMaxEpsilon = 0.02;

bool SameContourReference(const ImportedContourReference &left,
                          const ImportedContourReference &right) {
  return left.kind == right.kind && left.item_id == right.item_id &&
         left.contour_index == right.contour_index;
}

Clipper2Lib::PathD SimplifyClipperPath(const Clipper2Lib::PathD &path) {
  if (path.size() <= kSimplifyVertexThreshold) {
    return path;
  }
  double min_x = std::numeric_limits<double>::max();
  double max_x = std::numeric_limits<double>::lowest();
  double min_y = std::numeric_limits<double>::max();
  double max_y = std::numeric_limits<double>::lowest();
  for (const auto &pt : path) {
    min_x = std::min(min_x, pt.x);
    max_x = std::max(max_x, pt.x);
    min_y = std::min(min_y, pt.y);
    max_y = std::max(max_y, pt.y);
  }
  const double diagonal = std::hypot(max_x - min_x, max_y - min_y);
  const double epsilon =
      std::min(diagonal * kSimplifyDiagonalFraction, kSimplifyMaxEpsilon);
  if (epsilon <= 0.0) {
    return path;
  }
  auto simplified = Clipper2Lib::SimplifyPath(path, epsilon, true);
  return simplified.size() >= 3 ? simplified : path;
}

Contour ConvertPathDToWorldContour(const ImportedArtwork &artwork,
                                   const Clipper2Lib::PathD &path) {
  const auto simplified = SimplifyClipperPath(path);
  Contour contour;
  contour.reserve(simplified.size());
  for (const Clipper2Lib::PointD &point : simplified) {
    const ImVec2 world = ImportedArtworkPointToWorld(
        artwork,
        ImVec2(static_cast<float>(point.x), static_cast<float>(point.y)));
    contour.push_back(PointD{.x = static_cast<double>(world.x),
                             .y = static_cast<double>(world.y)});
  }
  return contour;
}

std::optional<Contour>
ResolveContourReference(const ImportedArtwork &artwork,
                        const ImportedContourReference &reference) {
  if (reference.kind == ImportedElementKind::Path) {
    const ImportedPath *path = FindImportedPath(artwork, reference.item_id);
    if (path == nullptr || !path->closed || path->segments.empty()) {
      return std::nullopt;
    }
    return ConvertPathDToWorldContour(
        artwork, detail::SampleImportedPathToClipperPath(*path));
  }

  const ImportedDxfText *text = FindImportedDxfText(artwork, reference.item_id);
  if (text == nullptr) {
    return std::nullopt;
  }

  int contour_index = 0;
  const auto try_contour =
      [&](const ImportedTextContour &contour) -> std::optional<Contour> {
    if (contour.role == ImportedTextContourRole::Guide ||
        contour.segments.empty()) {
      return std::nullopt;
    }
    if (contour_index++ != reference.contour_index) {
      return std::nullopt;
    }
    return ConvertPathDToWorldContour(
        artwork, detail::SampleImportedTextContourToClipperPath(contour));
  };

  for (const ImportedTextGlyph &glyph : text->glyphs) {
    for (const ImportedTextContour &contour : glyph.contours) {
      if (const auto resolved = try_contour(contour); resolved.has_value()) {
        return resolved;
      }
    }
  }

  for (const ImportedTextContour &contour : text->placeholder_contours) {
    if (const auto resolved = try_contour(contour); resolved.has_value()) {
      return resolved;
    }
  }

  return std::nullopt;
}

std::string BuildPartId(const ImportedArtwork &artwork, size_t outer_index,
                        size_t outer_count) {
  const std::string base =
      artwork.name.empty() ? ("artwork-" + std::to_string(artwork.part.part_id))
                           : artwork.name;
  if (outer_count <= 1) {
    return base;
  }
  return base + "#" + std::to_string(outer_index + 1);
}

} // namespace

std::optional<Sheet> ConvertWorkingAreaToSheet(const WorkingArea &working_area,
                                               const std::string &sheet_id,
                                               int quantity) {
  if (working_area.size.x <= 0.0f || working_area.size.y <= 0.0f) {
    return std::nullopt;
  }

  Sheet sheet;
  sheet.id = sheet_id.empty() ? working_area.name : sheet_id;
  if (sheet.id.empty()) {
    sheet.id = "sheet-" + std::to_string(working_area.id);
  }
  sheet.quantity = std::max(quantity, 1);
  sheet.geometry.outer = Contour{
      {static_cast<double>(working_area.origin.x),
       static_cast<double>(working_area.origin.y)},
      {static_cast<double>(working_area.origin.x + working_area.size.x),
       static_cast<double>(working_area.origin.y)},
      {static_cast<double>(working_area.origin.x + working_area.size.x),
       static_cast<double>(working_area.origin.y + working_area.size.y)},
      {static_cast<double>(working_area.origin.x),
       static_cast<double>(working_area.origin.y + working_area.size.y)}};
  sheet.geometry = NormalizePolygonWinding(sheet.geometry);
  return sheet;
}

std::vector<Part> ConvertImportedArtworkToParts(
    const ImportedArtwork &artwork,
    const std::vector<double> &allowed_rotations_degrees, int quantity) {
  ImportedArtwork prepared = artwork;
  RecomputeImportedArtworkBounds(prepared);
  RefreshImportedArtworkPartMetadata(prepared);

  std::vector<Part> parts;
  if (!prepared.part.nest_ready || prepared.part.outer_contours.empty()) {
    return parts;
  }

  const size_t outer_count = prepared.part.outer_contours.size();
  for (size_t outer_index = 0; outer_index < outer_count; ++outer_index) {
    const ImportedContourReference &outer_ref =
        prepared.part.outer_contours[outer_index];
    const auto outer = ResolveContourReference(prepared, outer_ref);
    if (!outer.has_value() || !IsContourValid(*outer, 1e-9)) {
      continue;
    }

    PolygonWithHoles polygon;
    polygon.outer = *outer;
    for (const ImportedHoleOwnership &ownership :
         prepared.part.hole_attachments) {
      if (!SameContourReference(ownership.outer, outer_ref)) {
        continue;
      }
      const auto hole = ResolveContourReference(prepared, ownership.hole);
      if (!hole.has_value() || !IsContourValid(*hole, 1e-9)) {
        continue;
      }
      polygon.holes.push_back(*hole);
    }

    Part part;
    part.id = BuildPartId(prepared, outer_index, outer_count);
    part.geometry = NormalizePolygonWinding(std::move(polygon));
    part.quantity = std::max(quantity, 1);
    part.allowed_rotations_degrees = allowed_rotations_degrees;
    parts.push_back(std::move(part));
  }

  return parts;
}

} // namespace im2d::nesting