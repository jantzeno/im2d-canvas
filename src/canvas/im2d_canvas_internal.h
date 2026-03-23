#pragma once

#include "im2d_canvas_types.h"

#include <clipper2/clipper.h>

#include <functional>
#include <vector>

namespace im2d::detail {

struct SelectionRect {
  ImVec2 min = ImVec2(0.0f, 0.0f);
  ImVec2 max = ImVec2(0.0f, 0.0f);
};

struct ImportedArtworkBounds {
  ImVec2 min = ImVec2(0.0f, 0.0f);
  ImVec2 max = ImVec2(0.0f, 0.0f);
  bool valid = false;
};

float DistanceSquared(const ImVec2 &a, const ImVec2 &b);
bool PointsNear(const ImVec2 &a, const ImVec2 &b, float tolerance);
ImVec2 CubicBezierPoint(const ImVec2 &start, const ImVec2 &control1,
                        const ImVec2 &control2, const ImVec2 &end, float t);
void AppendSampledSegmentPointsLocal(
    const std::vector<ImportedPathSegment> &segments,
    std::vector<ImVec2> *sample_points);
void AppendPathSamplePointsWorld(const ImportedArtwork &artwork,
                                 const ImportedPath &path,
                                 std::vector<ImVec2> *sample_points);
void AppendTextSamplePointsWorld(const ImportedArtwork &artwork,
                                 const ImportedDxfText &text,
                                 std::vector<ImVec2> *sample_points);
SelectionRect NormalizeRect(const ImVec2 &a, const ImVec2 &b);
bool PointInsideSelection(const SelectionRect &rect,
                          ImportedArtworkEditMode mode, const ImVec2 &point);
Clipper2Lib::PathD SampleImportedPathToClipperPath(const ImportedPath &path);
Clipper2Lib::PathD
SampleImportedTextContourToClipperPath(const ImportedTextContour &contour);
void ForEachImportedArtworkPoint(
    ImportedArtwork &artwork,
    const std::function<void(ImVec2 &point)> &function);
void IncludePoint(ImportedArtworkBounds &bounds, const ImVec2 &point);
ImportedArtworkBounds ComputeImportedPathBounds(const ImportedPath &path);
ImportedArtworkBounds
ComputeImportedTextContourBounds(const ImportedTextContour &contour);
ImportedArtworkBounds ComputeImportedDxfTextBounds(const ImportedDxfText &text);
ImportedArtworkBounds
ComputeImportedArtworkBounds(const ImportedArtwork &artwork);
ImportedArtworkBounds ComputeImportedGroupBounds(const ImportedArtwork &artwork,
                                                 const ImportedGroup &group);
ImVec2 ImportedArtworkLocalSize(const ImportedArtwork &artwork);
ImVec2 ImportedArtworkScaledSize(const ImportedArtwork &artwork);

} // namespace im2d::detail