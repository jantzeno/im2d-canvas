#include "../../src/nesting/im2d_nesting_postprocess.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

namespace {

im2d::nesting::ToolpathSegment MakeSegment(double x1, double y1, double x2,
                                           double y2, int contour = -1,
                                           int edge = -1) {
  return im2d::nesting::ToolpathSegment{
      .start = {x1, y1},
      .end = {x2, y2},
      .source_contour_index = contour,
      .source_edge_index = edge,
  };
}

bool HasSegment(const std::vector<im2d::nesting::ToolpathSegment> &segments,
                double x1, double y1, double x2, double y2) {
  return std::find_if(
             segments.begin(), segments.end(), [&](const auto &segment) {
               const bool forward = segment.start.x == Catch::Approx(x1) &&
                                    segment.start.y == Catch::Approx(y1) &&
                                    segment.end.x == Catch::Approx(x2) &&
                                    segment.end.y == Catch::Approx(y2);
               const bool reverse = segment.start.x == Catch::Approx(x2) &&
                                    segment.start.y == Catch::Approx(y2) &&
                                    segment.end.x == Catch::Approx(x1) &&
                                    segment.end.y == Catch::Approx(y1);
               return forward || reverse;
             }) != segments.end();
}

} // namespace

TEST_CASE("ExtractToolpathSegments emits one closed edge per contour edge",
          "[postprocess]") {
  const std::vector<im2d::nesting::Contour> contours{
      {{0.0, 0.0}, {2.0, 0.0}, {2.0, 1.0}, {0.0, 1.0}}};

  const auto segments = im2d::nesting::ExtractToolpathSegments(contours);

  REQUIRE(segments.size() == 4);
  CHECK(HasSegment(segments, 0.0, 0.0, 2.0, 0.0));
  CHECK(HasSegment(segments, 2.0, 0.0, 2.0, 1.0));
  CHECK(HasSegment(segments, 2.0, 1.0, 0.0, 1.0));
  CHECK(HasSegment(segments, 0.0, 1.0, 0.0, 0.0));
}

TEST_CASE("EliminateFullyCoveredCollinearSegments removes shorter covered edge",
          "[postprocess]") {
  const std::vector<im2d::nesting::ToolpathSegment> segments{
      MakeSegment(0.0, 0.0, 10.0, 0.0),
      MakeSegment(2.0, 0.0, 6.0, 0.0),
      MakeSegment(0.0, 1.0, 4.0, 1.0),
  };

  const auto filtered =
      im2d::nesting::EliminateFullyCoveredCollinearSegments(segments);

  REQUIRE(filtered.size() == 2);
  CHECK(HasSegment(filtered, 0.0, 0.0, 10.0, 0.0));
  CHECK_FALSE(HasSegment(filtered, 2.0, 0.0, 6.0, 0.0));
  CHECK(HasSegment(filtered, 0.0, 1.0, 4.0, 1.0));
}

TEST_CASE(
    "EliminateFullyCoveredCollinearSegments keeps partially overlapping edges",
    "[postprocess]") {
  const std::vector<im2d::nesting::ToolpathSegment> segments{
      MakeSegment(0.0, 0.0, 4.0, 0.0),
      MakeSegment(2.0, 0.0, 6.0, 0.0),
  };

  const auto filtered =
      im2d::nesting::EliminateFullyCoveredCollinearSegments(segments);

  REQUIRE(filtered.size() == 2);
  CHECK(HasSegment(filtered, 0.0, 0.0, 4.0, 0.0));
  CHECK(HasSegment(filtered, 2.0, 0.0, 6.0, 0.0));
}

TEST_CASE("EliminateFullyCoveredCollinearSegments removes duplicate coincident "
          "edge once",
          "[postprocess]") {
  const std::vector<im2d::nesting::ToolpathSegment> segments{
      MakeSegment(0.0, 0.0, 5.0, 0.0),
      MakeSegment(5.0, 0.0, 0.0, 0.0),
      MakeSegment(0.0, 2.0, 5.0, 2.0),
  };

  const auto filtered =
      im2d::nesting::EliminateFullyCoveredCollinearSegments(segments);

  REQUIRE(filtered.size() == 2);
  CHECK(HasSegment(filtered, 0.0, 0.0, 5.0, 0.0));
  CHECK(HasSegment(filtered, 0.0, 2.0, 5.0, 2.0));
}