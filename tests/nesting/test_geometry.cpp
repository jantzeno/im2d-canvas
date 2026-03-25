#include "../../src/nesting/im2d_nesting_engine.h"
#include "../../src/nesting/im2d_nesting_geometry.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

im2d::nesting::Contour MakeCounterClockwiseSquare() {
  return {
      {0.0, 0.0},
      {2.0, 0.0},
      {2.0, 2.0},
      {0.0, 2.0},
  };
}

im2d::nesting::Contour MakeClockwiseSquare() {
  return {
      {0.0, 0.0},
      {0.0, 2.0},
      {2.0, 2.0},
      {2.0, 0.0},
  };
}

} // namespace

TEST_CASE("SignedArea distinguishes contour winding", "[geometry]") {
  const auto ccw = MakeCounterClockwiseSquare();
  const auto cw = MakeClockwiseSquare();

  REQUIRE(im2d::nesting::SignedArea(ccw) > 0.0);
  REQUIRE(im2d::nesting::SignedArea(cw) < 0.0);
  REQUIRE_FALSE(im2d::nesting::IsClockwise(ccw));
  REQUIRE(im2d::nesting::IsClockwise(cw));
}

TEST_CASE("ComputeBounds returns contour extents", "[geometry]") {
  const auto contour =
      im2d::nesting::Contour{{-2.0, 1.0}, {4.0, 3.0}, {1.0, -5.0}};

  const im2d::nesting::BoundsD bounds = im2d::nesting::ComputeBounds(contour);

  REQUIRE(bounds.valid);
  REQUIRE(bounds.min.x == -2.0);
  REQUIRE(bounds.min.y == -5.0);
  REQUIRE(bounds.max.x == 4.0);
  REQUIRE(bounds.max.y == 3.0);
}

TEST_CASE("NormalizeProblemGeometry enforces outer and hole winding",
          "[geometry]") {
  im2d::nesting::NestingProblem problem;
  problem.parts.push_back(im2d::nesting::Part{
      .id = "part-a",
      .geometry =
          im2d::nesting::PolygonWithHoles{
              .outer = MakeClockwiseSquare(),
              .holes = {MakeCounterClockwiseSquare()},
          },
  });

  const im2d::nesting::NestingProblem normalized =
      im2d::nesting::NormalizeProblemGeometry(problem);

  REQUIRE_FALSE(
      im2d::nesting::IsClockwise(normalized.parts.front().geometry.outer));
  REQUIRE(im2d::nesting::IsClockwise(
      normalized.parts.front().geometry.holes.front()));
}

TEST_CASE("IsPointOnSegment detects boundary points", "[geometry]") {
  const im2d::nesting::PointD start{0.0, 0.0};
  const im2d::nesting::PointD end{4.0, 4.0};

  REQUIRE(im2d::nesting::IsPointOnSegment({2.0, 2.0}, start, end));
  REQUIRE(im2d::nesting::IsPointOnSegment({0.0, 0.0}, start, end));
  REQUIRE_FALSE(im2d::nesting::IsPointOnSegment({2.0, 3.0}, start, end));
}

TEST_CASE("IntersectSegments identifies crossings and overlaps", "[geometry]") {
  const im2d::nesting::SegmentIntersection crossing =
      im2d::nesting::IntersectSegments({0.0, 0.0}, {4.0, 4.0}, {0.0, 4.0},
                                       {4.0, 0.0});
  REQUIRE(crossing.intersects);
  REQUIRE_FALSE(crossing.collinear_overlap);
  REQUIRE(crossing.point.x == 2.0);
  REQUIRE(crossing.point.y == 2.0);

  const im2d::nesting::SegmentIntersection overlap =
      im2d::nesting::IntersectSegments({0.0, 0.0}, {4.0, 0.0}, {2.0, 0.0},
                                       {6.0, 0.0});
  REQUIRE(overlap.intersects);
  REQUIRE(overlap.collinear_overlap);
}

TEST_CASE("IsPointInContour handles interior and boundary points",
          "[geometry]") {
  const auto contour = MakeCounterClockwiseSquare();

  REQUIRE(im2d::nesting::IsPointInContour({1.0, 1.0}, contour));
  REQUIRE(im2d::nesting::IsPointInContour({0.0, 1.0}, contour));
  REQUIRE_FALSE(im2d::nesting::IsPointInContour({3.0, 1.0}, contour));
  REQUIRE_FALSE(im2d::nesting::IsPointInContour({0.0, 1.0}, contour, false));
}

TEST_CASE("IsContourValid rejects degenerate and self-intersecting contours",
          "[geometry]") {
  const auto valid = MakeCounterClockwiseSquare();
  const im2d::nesting::Contour line_like{{0.0, 0.0}, {1.0, 0.0}, {2.0, 0.0}};
  const im2d::nesting::Contour bow_tie{
      {0.0, 0.0}, {2.0, 2.0}, {0.0, 2.0}, {2.0, 0.0}};

  REQUIRE(im2d::nesting::IsContourValid(valid));
  REQUIRE_FALSE(im2d::nesting::IsContourValid(line_like));
  REQUIRE_FALSE(im2d::nesting::IsContourValid(bow_tie));
}

TEST_CASE("NormalizeVector normalizes non-zero vectors", "[geometry]") {
  const auto normalized = im2d::nesting::NormalizeVector({3.0, 4.0});
  REQUIRE(normalized.has_value());
  REQUIRE(normalized->x == Catch::Approx(0.6));
  REQUIRE(normalized->y == Catch::Approx(0.8));
  REQUIRE_FALSE(im2d::nesting::NormalizeVector({0.0, 0.0}).has_value());
}

TEST_CASE("PointDistanceAlongDirection projects a point onto a segment ray",
          "[geometry]") {
  const auto distance = im2d::nesting::PointDistanceAlongDirection(
      {2.0, 2.0}, {0.0, 0.0}, {4.0, 0.0}, {0.0, -1.0});
  REQUIRE(distance.has_value());
  REQUIRE(*distance == Catch::Approx(2.0));

  const auto miss = im2d::nesting::PointDistanceAlongDirection(
      {5.0, 2.0}, {0.0, 0.0}, {4.0, 0.0}, {0.0, -1.0});
  REQUIRE_FALSE(miss.has_value());
}

TEST_CASE("PolygonProjectionDistance matches simple downward projection",
          "[geometry]") {
  const im2d::nesting::Contour stationary{
      {0.0, 0.0}, {4.0, 0.0}, {4.0, 1.0}, {0.0, 1.0}};
  const im2d::nesting::Contour moving{
      {1.0, 3.0}, {2.0, 3.0}, {2.0, 4.0}, {1.0, 4.0}};

  const auto projection =
      im2d::nesting::PolygonProjectionDistance(stationary, moving, {0.0, -1.0});
  REQUIRE(projection.has_value());
  REQUIRE(*projection == Catch::Approx(3.0));
}

TEST_CASE("PolygonSlideDistance matches SVGnest-style edge sliding",
          "[geometry]") {
  const im2d::nesting::Contour stationary{{0.0, 0.0}, {6.0, 0.0}, {0.0, 6.0}};
  const im2d::nesting::Contour moving{{0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}};

  const auto slide =
      im2d::nesting::PolygonSlideDistance(stationary, moving, {0.0, 6.0});
  REQUIRE(slide.has_value());
  REQUIRE(*slide == Catch::Approx(5.0));
}