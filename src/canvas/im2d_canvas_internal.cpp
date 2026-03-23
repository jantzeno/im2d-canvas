#include "im2d_canvas_internal.h"

#include "im2d_canvas_document.h"

#include <algorithm>

namespace im2d::detail {

namespace {

constexpr float kImportedCurveFlatteningTolerance = 12.0f;

} // namespace

float DistanceSquared(const ImVec2 &a, const ImVec2 &b) {
  const float dx = a.x - b.x;
  const float dy = a.y - b.y;
  return dx * dx + dy * dy;
}

bool PointsNear(const ImVec2 &a, const ImVec2 &b, float tolerance) {
  return DistanceSquared(a, b) <= tolerance * tolerance;
}

ImVec2 CubicBezierPoint(const ImVec2 &start, const ImVec2 &control1,
                        const ImVec2 &control2, const ImVec2 &end, float t) {
  const float mt = 1.0f - t;
  const float mt2 = mt * mt;
  const float t2 = t * t;
  return ImVec2(mt2 * mt * start.x + 3.0f * mt2 * t * control1.x +
                    3.0f * mt * t2 * control2.x + t2 * t * end.x,
                mt2 * mt * start.y + 3.0f * mt2 * t * control1.y +
                    3.0f * mt * t2 * control2.y + t2 * t * end.y);
}

void AppendSampledSegmentPointsLocal(
    const std::vector<ImportedPathSegment> &segments,
    std::vector<ImVec2> *sample_points) {
  if (segments.empty()) {
    return;
  }

  sample_points->push_back(segments.front().start);
  for (const ImportedPathSegment &segment : segments) {
    if (segment.kind == ImportedPathSegmentKind::Line) {
      sample_points->push_back(segment.end);
      continue;
    }

    for (int sample_index = 1;
         sample_index <= static_cast<int>(kImportedCurveFlatteningTolerance);
         ++sample_index) {
      const float t =
          static_cast<float>(sample_index) / kImportedCurveFlatteningTolerance;
      sample_points->push_back(CubicBezierPoint(
          segment.start, segment.control1, segment.control2, segment.end, t));
    }
  }
}

void AppendPathSamplePointsWorld(const ImportedArtwork &artwork,
                                 const ImportedPath &path,
                                 std::vector<ImVec2> *sample_points) {
  const size_t start_index = sample_points->size();
  AppendSampledSegmentPointsLocal(path.segments, sample_points);
  for (size_t index = start_index; index < sample_points->size(); ++index) {
    sample_points->at(index) =
        ImportedArtworkPointToWorld(artwork, sample_points->at(index));
  }
}

void AppendTextSamplePointsWorld(const ImportedArtwork &artwork,
                                 const ImportedDxfText &text,
                                 std::vector<ImVec2> *sample_points) {
  for (const ImportedTextGlyph &glyph : text.glyphs) {
    for (const ImportedTextContour &contour : glyph.contours) {
      const size_t start_index = sample_points->size();
      AppendSampledSegmentPointsLocal(contour.segments, sample_points);
      for (size_t index = start_index; index < sample_points->size(); ++index) {
        sample_points->at(index) =
            ImportedArtworkPointToWorld(artwork, sample_points->at(index));
      }
    }
  }

  for (const ImportedTextContour &contour : text.placeholder_contours) {
    const size_t start_index = sample_points->size();
    AppendSampledSegmentPointsLocal(contour.segments, sample_points);
    for (size_t index = start_index; index < sample_points->size(); ++index) {
      sample_points->at(index) =
          ImportedArtworkPointToWorld(artwork, sample_points->at(index));
    }
  }
}

SelectionRect NormalizeRect(const ImVec2 &a, const ImVec2 &b) {
  return {ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)),
          ImVec2(std::max(a.x, b.x), std::max(a.y, b.y))};
}

bool PointInsideSelection(const SelectionRect &rect,
                          ImportedArtworkEditMode mode, const ImVec2 &point) {
  if (mode == ImportedArtworkEditMode::SelectRectangle) {
    return point.x >= rect.min.x && point.x <= rect.max.x &&
           point.y >= rect.min.y && point.y <= rect.max.y;
  }

  const ImVec2 center((rect.min.x + rect.max.x) * 0.5f,
                      (rect.min.y + rect.max.y) * 0.5f);
  const float radius_x = std::max((rect.max.x - rect.min.x) * 0.5f, 0.001f);
  const float radius_y = std::max((rect.max.y - rect.min.y) * 0.5f, 0.001f);
  const float dx = (point.x - center.x) / radius_x;
  const float dy = (point.y - center.y) / radius_y;
  return dx * dx + dy * dy <= 1.0f;
}

Clipper2Lib::PathD SampleImportedPathToClipperPath(const ImportedPath &path) {
  std::vector<ImVec2> sampled_points;
  AppendSampledSegmentPointsLocal(path.segments, &sampled_points);
  if (sampled_points.size() > 1 &&
      PointsNear(sampled_points.front(), sampled_points.back(), 0.0001f)) {
    sampled_points.pop_back();
  }

  Clipper2Lib::PathD clipper_path;
  clipper_path.reserve(sampled_points.size());
  for (const ImVec2 &point : sampled_points) {
    clipper_path.emplace_back(static_cast<double>(point.x),
                              static_cast<double>(point.y));
  }
  return clipper_path;
}

Clipper2Lib::PathD
SampleImportedTextContourToClipperPath(const ImportedTextContour &contour) {
  std::vector<ImVec2> sampled_points;
  AppendSampledSegmentPointsLocal(contour.segments, &sampled_points);
  if (sampled_points.size() > 1 &&
      PointsNear(sampled_points.front(), sampled_points.back(), 0.0001f)) {
    sampled_points.pop_back();
  }

  Clipper2Lib::PathD clipper_path;
  clipper_path.reserve(sampled_points.size());
  for (const ImVec2 &point : sampled_points) {
    clipper_path.emplace_back(static_cast<double>(point.x),
                              static_cast<double>(point.y));
  }
  return clipper_path;
}

void ForEachImportedArtworkPoint(
    ImportedArtwork &artwork,
    const std::function<void(ImVec2 &point)> &function) {
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

  for (ImportedDxfText &text : artwork.dxf_text) {
    for (ImportedTextGlyph &glyph : text.glyphs) {
      for (ImportedTextContour &contour : glyph.contours) {
        for (ImportedPathSegment &segment : contour.segments) {
          function(segment.start);
          if (segment.kind == ImportedPathSegmentKind::CubicBezier) {
            function(segment.control1);
            function(segment.control2);
          }
          function(segment.end);
        }
      }
    }

    for (ImportedTextContour &contour : text.placeholder_contours) {
      for (ImportedPathSegment &segment : contour.segments) {
        function(segment.start);
        if (segment.kind == ImportedPathSegmentKind::CubicBezier) {
          function(segment.control1);
          function(segment.control2);
        }
        function(segment.end);
      }
    }
  }
}

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
ComputeImportedTextContourBounds(const ImportedTextContour &contour) {
  ImportedArtworkBounds bounds;
  for (const ImportedPathSegment &segment : contour.segments) {
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
ComputeImportedDxfTextBounds(const ImportedDxfText &text) {
  ImportedArtworkBounds bounds;
  for (const ImportedTextGlyph &glyph : text.glyphs) {
    for (const ImportedTextContour &contour : glyph.contours) {
      const ImportedArtworkBounds contour_bounds =
          ComputeImportedTextContourBounds(contour);
      if (!contour_bounds.valid) {
        continue;
      }
      IncludePoint(bounds, contour_bounds.min);
      IncludePoint(bounds, contour_bounds.max);
    }
  }

  for (const ImportedTextContour &contour : text.placeholder_contours) {
    const ImportedArtworkBounds contour_bounds =
        ComputeImportedTextContourBounds(contour);
    if (!contour_bounds.valid) {
      continue;
    }
    IncludePoint(bounds, contour_bounds.min);
    IncludePoint(bounds, contour_bounds.max);
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

  for (const ImportedDxfText &text : artwork.dxf_text) {
    const ImportedArtworkBounds text_bounds =
        ComputeImportedDxfTextBounds(text);
    if (!text_bounds.valid) {
      continue;
    }
    IncludePoint(bounds, text_bounds.min);
    IncludePoint(bounds, text_bounds.max);
  }

  return bounds;
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

  for (const int text_id : group.dxf_text_ids) {
    const ImportedDxfText *text = FindImportedDxfText(artwork, text_id);
    if (text == nullptr) {
      continue;
    }

    const ImportedArtworkBounds text_bounds =
        ComputeImportedDxfTextBounds(*text);
    if (!text_bounds.valid) {
      continue;
    }

    IncludePoint(bounds, text_bounds.min);
    IncludePoint(bounds, text_bounds.max);
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

ImVec2 ImportedArtworkLocalSize(const ImportedArtwork &artwork) {
  return ImVec2(std::max(artwork.bounds_max.x - artwork.bounds_min.x, 1.0f),
                std::max(artwork.bounds_max.y - artwork.bounds_min.y, 1.0f));
}

ImVec2 ImportedArtworkScaledSize(const ImportedArtwork &artwork) {
  const ImVec2 local_size = ImportedArtworkLocalSize(artwork);
  return ImVec2(std::max(local_size.x * artwork.scale.x, 1.0f),
                std::max(local_size.y * artwork.scale.y, 1.0f));
}

} // namespace im2d::detail