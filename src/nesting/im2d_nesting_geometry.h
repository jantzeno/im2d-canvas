#pragma once

#include "im2d_nesting_types.h"

#include <optional>

namespace im2d::nesting {

struct BoundsD {
  PointD min;
  PointD max;
  bool valid = false;
};

struct SegmentIntersection {
  bool intersects = false;
  bool collinear_overlap = false;
  bool touches_at_endpoint = false;
  PointD point;
};

double DotProduct(const PointD &a, const PointD &b);
double LengthSquared(const PointD &vector);
std::optional<PointD> NormalizeVector(const PointD &vector,
                                      double epsilon = 1e-9);
double SignedArea(const Contour &contour);
bool IsClockwise(const Contour &contour);
BoundsD ComputeBounds(const Contour &contour);
bool IsPointOnSegment(const PointD &point, const PointD &segment_start,
                      const PointD &segment_end, double epsilon = 1e-9);
SegmentIntersection IntersectSegments(const PointD &a_start,
                                      const PointD &a_end,
                                      const PointD &b_start,
                                      const PointD &b_end,
                                      double epsilon = 1e-9);
bool IsPointInContour(const PointD &point, const Contour &contour,
                      bool include_boundary = true, double epsilon = 1e-9);
bool IsContourSimple(const Contour &contour, double epsilon = 1e-9);
bool IsContourValid(const Contour &contour, double epsilon = 1e-9);
std::optional<double>
PointDistanceAlongDirection(const PointD &point, const PointD &segment_start,
                            const PointD &segment_end, const PointD &direction,
                            bool infinite = false, double epsilon = 1e-9);
std::optional<double> PolygonSlideDistance(const Contour &stationary,
                                           const Contour &moving,
                                           const PointD &direction,
                                           bool ignore_negative = true,
                                           double epsilon = 1e-9);
std::optional<double> PolygonProjectionDistance(const Contour &stationary,
                                                const Contour &moving,
                                                const PointD &direction,
                                                double epsilon = 1e-9);
Contour TranslateContour(const Contour &contour, const PointD &translation);
bool ContoursIntersect(const Contour &a, const Contour &b,
                       double epsilon = 1e-9);
bool IsContourInsideContour(const Contour &inner, const Contour &outer,
                            bool allow_boundary = true, double epsilon = 1e-9);
void NormalizeOuterWinding(Contour *contour);
void NormalizeHoleWinding(Contour *contour);
PolygonWithHoles NormalizePolygonWinding(PolygonWithHoles polygon);

} // namespace im2d::nesting