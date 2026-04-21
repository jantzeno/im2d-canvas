#include "../../src/canvas/im2d_canvas_internal.h"
#include "../../src/nesting/im2d_nesting_adapter.h"
#include "../../src/nesting/im2d_nesting_geometry.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

// Standard Bézier circle approximation constant (4/3 * (sqrt(2) - 1)).
constexpr float kBezierCircleK = 0.5522847498f;

im2d::ImportedPath MakeClosedLinePath(int id,
                                      const std::vector<ImVec2> &points) {
  im2d::ImportedPath path;
  path.id = id;
  path.closed = true;
  path.flags = im2d::ImportedPathFlagNone;
  for (size_t i = 0; i < points.size(); ++i) {
    im2d::ImportedPathSegment seg;
    seg.kind = im2d::ImportedPathSegmentKind::Line;
    seg.start = points[i];
    seg.end = points[(i + 1) % points.size()];
    path.segments.push_back(seg);
  }
  return path;
}

std::vector<ImVec2> MakeCirclePoints(float cx, float cy, float radius,
                                     int vertex_count) {
  std::vector<ImVec2> points;
  points.reserve(vertex_count);
  for (int i = 0; i < vertex_count; ++i) {
    const float angle = 2.0f * static_cast<float>(kPi) * static_cast<float>(i) /
                        static_cast<float>(vertex_count);
    points.push_back(
        ImVec2(cx + radius * std::cos(angle), cy + radius * std::sin(angle)));
  }
  return points;
}

double ContourArea(const im2d::nesting::Contour &contour) {
  if (contour.size() < 3) {
    return 0.0;
  }
  double twice_area = 0.0;
  for (size_t i = 0; i < contour.size(); ++i) {
    const size_t j = (i + 1) % contour.size();
    twice_area += contour[i].x * contour[j].y - contour[j].x * contour[i].y;
  }
  return std::abs(twice_area) / 2.0;
}

double MaxRadialDeviation(const im2d::nesting::Contour &contour, double cx,
                          double cy, double expected_radius) {
  double max_dev = 0.0;
  for (const auto &pt : contour) {
    const double dist = std::hypot(pt.x - cx, pt.y - cy);
    max_dev = std::max(max_dev, std::abs(dist - expected_radius));
  }
  return max_dev;
}

double PointToSegmentDistance(double px, double py, double ax, double ay,
                              double bx, double by) {
  const double dx = bx - ax;
  const double dy = by - ay;
  const double len_sq = dx * dx + dy * dy;
  if (len_sq < 1e-30) {
    return std::hypot(px - ax, py - ay);
  }
  double t = ((px - ax) * dx + (py - ay) * dy) / len_sq;
  t = std::clamp(t, 0.0, 1.0);
  const double proj_x = ax + t * dx;
  const double proj_y = ay + t * dy;
  return std::hypot(px - proj_x, py - proj_y);
}

double PointToPolylineDistance(const std::vector<ImVec2> &polyline, double px,
                               double py) {
  double min_dist = std::numeric_limits<double>::max();
  for (size_t i = 0; i + 1 < polyline.size(); ++i) {
    const double d =
        PointToSegmentDistance(px, py, polyline[i].x, polyline[i].y,
                               polyline[i + 1].x, polyline[i + 1].y);
    min_dist = std::min(min_dist, d);
  }
  return min_dist;
}

} // namespace

// ---------------------------------------------------------------------------
// Curve flattening tolerance tests
// ---------------------------------------------------------------------------

TEST_CASE("Adaptive flattening of quarter-circle Bézier stays within tolerance",
          "[flattening][tolerance]") {
  const float R = 10.0f;
  const float k = R * kBezierCircleK;

  im2d::ImportedPathSegment seg;
  seg.kind = im2d::ImportedPathSegmentKind::CubicBezier;
  seg.start = ImVec2(R, 0.0f);
  seg.control1 = ImVec2(R, k);
  seg.control2 = ImVec2(k, R);
  seg.end = ImVec2(0.0f, R);

  std::vector<ImVec2> points;
  im2d::detail::AppendSampledSegmentPointsLocal({seg}, &points);

  // Must have start, end, and interior samples.
  REQUIRE(points.size() >= 4);

  // Every sampled point should lie near the true circle of radius R centred at
  // the origin.  The Bézier quarter-circle itself deviates from the true arc by
  // about 0.027% of R (≈ 0.003 for R=10), so we allow the flatness tolerance
  // plus the inherent Bézier error.
  const double max_allowed_deviation = 0.15; // 0.1 tolerance + margin
  for (const auto &pt : points) {
    const double dist = std::hypot(pt.x, pt.y);
    CHECK(std::abs(dist - R) < max_allowed_deviation);
  }
}

TEST_CASE("Adaptive flattening of full-circle Bézier produces enough samples",
          "[flattening][tolerance]") {
  // Four quarter-circle Bézier arcs forming a complete circle.
  const float R = 20.0f;
  const float k = R * kBezierCircleK;

  std::vector<im2d::ImportedPathSegment> segments(4);
  for (auto &s : segments) {
    s.kind = im2d::ImportedPathSegmentKind::CubicBezier;
  }

  segments[0].start = ImVec2(R, 0.0f);
  segments[0].control1 = ImVec2(R, k);
  segments[0].control2 = ImVec2(k, R);
  segments[0].end = ImVec2(0.0f, R);

  segments[1].start = ImVec2(0.0f, R);
  segments[1].control1 = ImVec2(-k, R);
  segments[1].control2 = ImVec2(-R, k);
  segments[1].end = ImVec2(-R, 0.0f);

  segments[2].start = ImVec2(-R, 0.0f);
  segments[2].control1 = ImVec2(-R, -k);
  segments[2].control2 = ImVec2(-k, -R);
  segments[2].end = ImVec2(0.0f, -R);

  segments[3].start = ImVec2(0.0f, -R);
  segments[3].control1 = ImVec2(k, -R);
  segments[3].control2 = ImVec2(R, -k);
  segments[3].end = ImVec2(R, 0.0f);

  std::vector<ImVec2> points;
  im2d::detail::AppendSampledSegmentPointsLocal(segments, &points);

  // A 20mm-radius circle should produce a healthy number of samples.
  CHECK(points.size() >= 16);

  // Verify every sample is near the circle.
  for (const auto &pt : points) {
    const double dist = std::hypot(pt.x, pt.y);
    CHECK(std::abs(dist - R) < 0.15);
  }
}

TEST_CASE(
    "Flattened Bézier polyline has bounded Hausdorff distance from true curve",
    "[flattening][tolerance]") {
  // Sample the true Bézier at many parameter values and check that each sample
  // is close to the flattened polyline.
  const float R = 15.0f;
  const float k = R * kBezierCircleK;

  im2d::ImportedPathSegment seg;
  seg.kind = im2d::ImportedPathSegmentKind::CubicBezier;
  seg.start = ImVec2(R, 0.0f);
  seg.control1 = ImVec2(R, k);
  seg.control2 = ImVec2(k, R);
  seg.end = ImVec2(0.0f, R);

  std::vector<ImVec2> polyline;
  im2d::detail::AppendSampledSegmentPointsLocal({seg}, &polyline);
  REQUIRE(polyline.size() >= 3);

  // Sample the true curve at 200 points and compute max distance to polyline.
  double max_hausdorff = 0.0;
  for (int i = 0; i <= 200; ++i) {
    const float t = static_cast<float>(i) / 200.0f;
    const ImVec2 true_pt = im2d::detail::CubicBezierPoint(
        seg.start, seg.control1, seg.control2, seg.end, t);
    const double d = PointToPolylineDistance(polyline, true_pt.x, true_pt.y);
    max_hausdorff = std::max(max_hausdorff, d);
  }

  // The Sederberg flatness test with tolerance 0.1 should keep the polyline
  // within roughly that tolerance of the true curve.
  CHECK(max_hausdorff < 0.2);
}

// ---------------------------------------------------------------------------
// Adapter simplification tolerance tests
// ---------------------------------------------------------------------------

TEST_CASE("Adapter simplification preserves area of a 100-vertex circle",
          "[adapter][tolerance]") {
  const float R = 50.0f;
  const auto circle_pts = MakeCirclePoints(R, R, R, 100);

  im2d::ImportedArtwork artwork;
  artwork.name = "circle";
  artwork.paths.push_back(MakeClosedLinePath(1, circle_pts));

  const auto parts = im2d::nesting::ConvertImportedArtworkToParts(artwork);
  REQUIRE(parts.size() == 1);

  const double expected_area = kPi * R * R;
  const double actual_area = ContourArea(parts.front().geometry.outer);

  // A 100-vertex polygon inscribed in a circle has area = R² * N/2 * sin(2π/N)
  // ≈ 99.5% of πR².  Simplification should not reduce it further by more than
  // 1% of the original polygon area.
  const double inscribed_area =
      R * R * 100.0 / 2.0 * std::sin(2.0 * kPi / 100.0);
  CHECK(actual_area > inscribed_area * 0.99);
  CHECK(actual_area < expected_area * 1.01);
}

TEST_CASE("Adapter simplification bounds max deviation for a circle",
          "[adapter][tolerance]") {
  const float R = 50.0f;
  const auto circle_pts = MakeCirclePoints(R, R, R, 100);

  im2d::ImportedArtwork artwork;
  artwork.name = "circle_dev";
  artwork.paths.push_back(MakeClosedLinePath(1, circle_pts));

  const auto parts = im2d::nesting::ConvertImportedArtworkToParts(artwork);
  REQUIRE(parts.size() == 1);

  // With the tightened tolerance (max 0.02 mm), the max deviation from the
  // true circle should be dominated by the polygon approximation error, not
  // by the simplification.  The 100-gon sagitta ≈ R * (1 - cos(π/100)) ≈
  // 0.049mm.
  const double max_dev =
      MaxRadialDeviation(parts.front().geometry.outer, R, R, R);
  CHECK(max_dev < 0.5); // generous bound: polygon sagitta + simplification
}

TEST_CASE("Adapter simplification retains vertex count for circle polygon",
          "[adapter][tolerance]") {
  const float R = 50.0f;
  const auto circle_pts = MakeCirclePoints(R, R, R, 100);

  im2d::ImportedArtwork artwork;
  artwork.name = "circle_verts";
  artwork.paths.push_back(MakeClosedLinePath(1, circle_pts));

  const auto parts = im2d::nesting::ConvertImportedArtworkToParts(artwork);
  REQUIRE(parts.size() == 1);

  // With a 0.02 mm epsilon on a 100-vertex polygon of radius 50mm, the
  // simplifier should remove at most a handful of near-collinear vertices.
  const size_t output_count = parts.front().geometry.outer.size();
  CHECK(output_count >= 60);
}

TEST_CASE("Adapter passes through small polygons unchanged",
          "[adapter][tolerance]") {
  im2d::ImportedArtwork artwork;
  artwork.name = "rect";
  artwork.paths.push_back(MakeClosedLinePath(
      1, {{0.0f, 0.0f}, {10.0f, 0.0f}, {10.0f, 5.0f}, {0.0f, 5.0f}}));

  const auto parts = im2d::nesting::ConvertImportedArtworkToParts(artwork);
  REQUIRE(parts.size() == 1);

  // A 4-vertex rectangle is well below the 24-vertex threshold and must not
  // lose any vertices.
  CHECK(parts.front().geometry.outer.size() == 4);
}

TEST_CASE("Rounded-corner rectangle retains corner detail through adapter",
          "[adapter][tolerance]") {
  // Build a rectangle with one rounded corner approximated by 12 arc segments.
  const float W = 80.0f;
  const float H = 60.0f;
  const float corner_radius = 8.0f;
  const int arc_segments = 12;

  std::vector<ImVec2> points;
  // Bottom-left to bottom-right (straight).
  points.push_back(ImVec2(0.0f, 0.0f));
  points.push_back(ImVec2(W - corner_radius, 0.0f));
  // Rounded bottom-right corner (12-segment arc from 270° to 360°).
  for (int i = 1; i <= arc_segments; ++i) {
    const float angle = static_cast<float>(kPi) * 1.5f +
                        static_cast<float>(kPi) * 0.5f * static_cast<float>(i) /
                            static_cast<float>(arc_segments);
    const float x = (W - corner_radius) + corner_radius * std::cos(angle);
    const float y = corner_radius + corner_radius * std::sin(angle);
    points.push_back(ImVec2(x, y));
  }
  // Top-right to top-left (straight).
  points.push_back(ImVec2(W, H));
  points.push_back(ImVec2(0.0f, H));

  im2d::ImportedArtwork artwork;
  artwork.name = "rounded_rect";
  artwork.paths.push_back(MakeClosedLinePath(1, points));

  const auto parts = im2d::nesting::ConvertImportedArtworkToParts(artwork);
  REQUIRE(parts.size() == 1);

  // The rounded corner has 12 arc segments.  With very conservative
  // simplification the adapter should retain most of them.
  // The total input vertex count is 4 straight + 12 arc = 16, which is below
  // the 24-vertex threshold, so it must pass through completely unchanged.
  CHECK(parts.front().geometry.outer.size() == points.size());
}

TEST_CASE("High-vertex rounded-corner shape retains corners through adapter",
          "[adapter][tolerance]") {
  // A rectangle with FOUR rounded corners, each with 12 arc segments.
  // Total vertex count = 4 × 12 = 48, which exceeds the 24-vertex threshold
  // and therefore exercises the simplifier.
  const float W = 100.0f;
  const float H = 80.0f;
  const float R = 10.0f;
  const int arc_segments = 12;

  std::vector<ImVec2> points;

  auto add_arc = [&](float cx, float cy, float start_angle, float end_angle) {
    for (int i = 0; i <= arc_segments; ++i) {
      const float angle = start_angle + (end_angle - start_angle) *
                                            static_cast<float>(i) /
                                            static_cast<float>(arc_segments);
      points.push_back(
          ImVec2(cx + R * std::cos(angle), cy + R * std::sin(angle)));
    }
  };

  const float pi = static_cast<float>(kPi);
  // Bottom-right corner (arc from -90° to 0°).
  add_arc(W - R, R, -pi / 2.0f, 0.0f);
  // Top-right corner (arc from 0° to 90°).
  add_arc(W - R, H - R, 0.0f, pi / 2.0f);
  // Top-left corner (arc from 90° to 180°).
  add_arc(R, H - R, pi / 2.0f, pi);
  // Bottom-left corner (arc from 180° to 270°).
  add_arc(R, R, pi, pi * 1.5f);

  im2d::ImportedArtwork artwork;
  artwork.name = "rounded_rect_4";
  artwork.paths.push_back(MakeClosedLinePath(1, points));

  const auto parts = im2d::nesting::ConvertImportedArtworkToParts(artwork);
  REQUIRE(parts.size() == 1);

  const size_t output_verts = parts.front().geometry.outer.size();

  // With 52 input vertices and a 0.02mm epsilon on a diagonal ~128mm, very few
  // vertices should be removed.  Certainly must keep ≥ 40.
  CHECK(output_verts >= 40);

  // Each arc quadrant should still be recognisable: verify that all output
  // vertices are within R + 0.5mm of the expected corner centre.
  // Instead check that the area is within 1% of the theoretical
  // rounded-rectangle area.
  const double theoretical_area =
      static_cast<double>(W) * H - (4.0 - kPi) * R * R;
  const double actual_area = ContourArea(parts.front().geometry.outer);
  CHECK(actual_area > theoretical_area * 0.99);
  CHECK(actual_area < theoretical_area * 1.01);
}
