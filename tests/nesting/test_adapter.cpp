#include "../../src/nesting/im2d_nesting_adapter.h"

#include "../../src/nesting/im2d_nesting_engine.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

namespace {

im2d::ImportedPath MakeClosedPath(int id, const std::vector<ImVec2> &points,
                                  uint32_t flags = im2d::ImportedPathFlagNone) {
  im2d::ImportedPath path;
  path.id = id;
  path.closed = true;
  path.flags = flags;
  if (points.size() < 2) {
    return path;
  }

  for (size_t index = 0; index < points.size(); ++index) {
    im2d::ImportedPathSegment segment;
    segment.kind = im2d::ImportedPathSegmentKind::Line;
    segment.start = points[index];
    segment.end = points[(index + 1) % points.size()];
    path.segments.push_back(segment);
  }
  return path;
}

bool HasPoint(const im2d::nesting::Contour &contour, double x, double y) {
  return std::find_if(contour.begin(), contour.end(), [&](const auto &point) {
           return point.x == Catch::Approx(x) && point.y == Catch::Approx(y);
         }) != contour.end();
}

} // namespace

TEST_CASE(
    "ConvertWorkingAreaToSheet builds a rectangular sheet in world coordinates",
    "[adapter]") {
  im2d::WorkingArea area;
  area.id = 7;
  area.name = "bed";
  area.origin = ImVec2(10.0f, 20.0f);
  area.size = ImVec2(100.0f, 50.0f);

  const auto sheet = im2d::nesting::ConvertWorkingAreaToSheet(area, {}, 2);

  REQUIRE(sheet.has_value());
  REQUIRE(sheet->id == "bed");
  REQUIRE(sheet->quantity == 2);
  REQUIRE(sheet->geometry.outer.size() == 4);
  CHECK(HasPoint(sheet->geometry.outer, 10.0, 20.0));
  CHECK(HasPoint(sheet->geometry.outer, 110.0, 20.0));
  CHECK(HasPoint(sheet->geometry.outer, 110.0, 70.0));
  CHECK(HasPoint(sheet->geometry.outer, 10.0, 70.0));
}

TEST_CASE(
    "ConvertImportedArtworkToParts maps outer contours and attached holes",
    "[adapter]") {
  im2d::ImportedArtwork artwork;
  artwork.id = 3;
  artwork.name = "plate";
  artwork.part.part_id = 9;
  artwork.origin = ImVec2(5.0f, 7.0f);
  artwork.scale = ImVec2(2.0f, 3.0f);
  artwork.paths.push_back(MakeClosedPath(
      1, {{0.0f, 0.0f}, {4.0f, 0.0f}, {4.0f, 3.0f}, {0.0f, 3.0f}}));
  artwork.paths.push_back(MakeClosedPath(
      2, {{1.0f, 1.0f}, {1.0f, 2.0f}, {3.0f, 2.0f}, {3.0f, 1.0f}},
      im2d::ImportedPathFlagHoleContour));

  const auto parts =
      im2d::nesting::ConvertImportedArtworkToParts(artwork, {0.0, 90.0}, 2);

  REQUIRE(parts.size() == 1);
  REQUIRE(parts.front().id == "plate");
  REQUIRE(parts.front().quantity == 2);
  REQUIRE(parts.front().allowed_rotations_degrees.size() == 2);
  REQUIRE(parts.front().geometry.outer.size() == 4);
  REQUIRE(parts.front().geometry.holes.size() == 1);

  CHECK(HasPoint(parts.front().geometry.outer, 5.0, 7.0));
  CHECK(HasPoint(parts.front().geometry.outer, 13.0, 7.0));
  CHECK(HasPoint(parts.front().geometry.outer, 13.0, 16.0));
  CHECK(HasPoint(parts.front().geometry.holes.front(), 7.0, 10.0));
  CHECK(HasPoint(parts.front().geometry.holes.front(), 11.0, 13.0));
}

TEST_CASE(
    "ConvertImportedArtworkToParts splits multiple outer contours into parts",
    "[adapter]") {
  im2d::ImportedArtwork artwork;
  artwork.name = "multi";
  artwork.paths.push_back(MakeClosedPath(
      1, {{0.0f, 0.0f}, {2.0f, 0.0f}, {2.0f, 2.0f}, {0.0f, 2.0f}}));
  artwork.paths.push_back(MakeClosedPath(
      2, {{4.0f, 0.0f}, {6.0f, 0.0f}, {6.0f, 2.0f}, {4.0f, 2.0f}}));

  const auto parts = im2d::nesting::ConvertImportedArtworkToParts(artwork);

  REQUIRE(parts.size() == 2);
  CHECK(parts[0].id == "multi#1");
  CHECK(parts[1].id == "multi#2");
}

TEST_CASE("Adapter-backed nesting uses converted artwork and working area",
          "[adapter]") {
  im2d::WorkingArea area;
  area.name = "sheet";
  area.origin = ImVec2(0.0f, 0.0f);
  area.size = ImVec2(8.0f, 4.0f);

  im2d::ImportedArtwork artwork;
  artwork.name = "part";
  artwork.paths.push_back(MakeClosedPath(
      1, {{0.0f, 0.0f}, {3.0f, 0.0f}, {3.0f, 2.0f}, {0.0f, 2.0f}}));

  const auto sheet = im2d::nesting::ConvertWorkingAreaToSheet(area);
  const auto parts = im2d::nesting::ConvertImportedArtworkToParts(artwork);

  REQUIRE(sheet.has_value());
  REQUIRE(parts.size() == 1);

  im2d::nesting::NestingProblem problem;
  problem.sheets = {*sheet};
  problem.parts = parts;

  const auto result = im2d::nesting::ComputeGreedyPlacement(
      problem, im2d::nesting::NestConfig{});

  REQUIRE(result.placements.size() == 1);
  REQUIRE(result.fitness.unplaced_part_count == 0);
  REQUIRE(result.fitness.sheet_count == 1);
  REQUIRE(result.fitness.used_width == Catch::Approx(3.0));
}