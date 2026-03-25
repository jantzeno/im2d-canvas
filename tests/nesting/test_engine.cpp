#include "../../src/nesting/im2d_nesting_engine.h"

#include "nesting_test_fixture.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>

namespace {

im2d::nesting::Contour MakeRectangle(double min_x, double min_y, double max_x,
                                     double max_y) {
  return {
      {min_x, min_y},
      {max_x, min_y},
      {max_x, max_y},
      {min_x, max_y},
  };
}

bool HasPlacement(const std::vector<im2d::nesting::Placement> &placements,
                  double x, double y, bool placed_in_hole) {
  return std::find_if(placements.begin(), placements.end(),
                      [&](const auto &placement) {
                        return placement.placed_in_hole == placed_in_hole &&
                               placement.translation.x == Catch::Approx(x) &&
                               placement.translation.y == Catch::Approx(y);
                      }) != placements.end();
}

bool PlacementsEqual(const im2d::nesting::Placement &left,
                     const im2d::nesting::Placement &right) {
  return left.part_id == right.part_id && left.sheet_id == right.sheet_id &&
         left.placed_in_hole == right.placed_in_hole &&
         left.translation.x == Catch::Approx(right.translation.x) &&
         left.translation.y == Catch::Approx(right.translation.y);
}

bool HasExactPlacement(const std::vector<im2d::nesting::Placement> &placements,
                       const im2d::nesting::Placement &expected) {
  return std::find_if(placements.begin(), placements.end(),
                      [&](const auto &placement) {
                        return PlacementsEqual(placement, expected);
                      }) != placements.end();
}

} // namespace

TEST_CASE("GeneratePlacementCandidates returns outer placements", "[engine]") {
  im2d::nesting::Sheet sheet;
  sheet.id = "sheet-a";
  sheet.geometry.outer = MakeRectangle(0.0, 0.0, 4.0, 4.0);

  im2d::nesting::Part part;
  part.id = "part-a";
  part.geometry.outer = MakeRectangle(0.0, 0.0, 1.0, 1.0);

  const auto placements = im2d::nesting::GeneratePlacementCandidates(
      sheet, part, im2d::nesting::NestConfig{});

  REQUIRE(placements.size() == 4);
  REQUIRE(HasPlacement(placements, 0.0, 0.0, false));
  REQUIRE(HasPlacement(placements, 3.0, 0.0, false));
  REQUIRE(HasPlacement(placements, 3.0, 3.0, false));
  REQUIRE(HasPlacement(placements, 0.0, 3.0, false));
}

TEST_CASE("GeneratePlacementCandidates includes hole placements when enabled",
          "[engine]") {
  im2d::nesting::Sheet sheet;
  sheet.id = "sheet-a";
  sheet.geometry.outer = MakeRectangle(0.0, 0.0, 4.0, 4.0);
  sheet.geometry.holes.push_back(MakeRectangle(10.0, 10.0, 16.0, 16.0));

  im2d::nesting::Part part;
  part.id = "part-a";
  part.geometry.outer = MakeRectangle(0.0, 0.0, 2.0, 2.0);

  const auto placements = im2d::nesting::GeneratePlacementCandidates(
      sheet, part, im2d::nesting::NestConfig{});

  REQUIRE(placements.size() == 8);
  REQUIRE(HasPlacement(placements, 0.0, 0.0, false));
  REQUIRE(HasPlacement(placements, 2.0, 2.0, false));
  REQUIRE(HasPlacement(placements, 10.0, 10.0, true));
  REQUIRE(HasPlacement(placements, 14.0, 14.0, true));
}

TEST_CASE("GeneratePlacementCandidates can succeed only through holes",
          "[engine]") {
  im2d::nesting::Sheet sheet;
  sheet.id = "sheet-a";
  sheet.geometry.outer = MakeRectangle(0.0, 0.0, 2.0, 2.0);
  sheet.geometry.holes.push_back(MakeRectangle(10.0, 10.0, 16.0, 16.0));

  im2d::nesting::Part part;
  part.id = "part-a";
  part.geometry.outer = MakeRectangle(0.0, 0.0, 3.0, 3.0);

  const auto placements = im2d::nesting::GeneratePlacementCandidates(
      sheet, part, im2d::nesting::NestConfig{});

  REQUIRE(placements.size() == 4);
  REQUIRE(std::all_of(placements.begin(), placements.end(),
                      [](const auto &p) { return p.placed_in_hole; }));
  REQUIRE(HasPlacement(placements, 10.0, 10.0, true));
  REQUIRE(HasPlacement(placements, 13.0, 13.0, true));
}

TEST_CASE("GeneratePlacementCandidates can disable hole usage", "[engine]") {
  im2d::nesting::Sheet sheet;
  sheet.id = "sheet-a";
  sheet.geometry.outer = MakeRectangle(0.0, 0.0, 2.0, 2.0);
  sheet.geometry.holes.push_back(MakeRectangle(10.0, 10.0, 16.0, 16.0));

  im2d::nesting::Part part;
  part.id = "part-a";
  part.geometry.outer = MakeRectangle(0.0, 0.0, 3.0, 3.0);

  im2d::nesting::NestConfig config;
  config.use_holes = false;

  const auto placements =
      im2d::nesting::GeneratePlacementCandidates(sheet, part, config);

  REQUIRE(placements.empty());
}

TEST_CASE("ComputeGreedyPlacement uses multiple sheet instances when needed",
          "[engine]") {
  im2d::nesting::Sheet sheet;
  sheet.id = "sheet";
  sheet.quantity = 2;
  sheet.geometry.outer = MakeRectangle(0.0, 0.0, 4.0, 4.0);

  im2d::nesting::Part part;
  part.id = "part";
  part.quantity = 2;
  part.geometry.outer = MakeRectangle(0.0, 0.0, 3.0, 3.0);

  im2d::nesting::NestingProblem problem;
  problem.parts = {part};
  problem.sheets = {sheet};

  const auto result = im2d::nesting::ComputeGreedyPlacement(
      problem, im2d::nesting::NestConfig{});

  REQUIRE(result.placements.size() == 2);
  REQUIRE(result.fitness.unplaced_part_count == 0);
  REQUIRE(result.fitness.sheet_count == 2);
  REQUIRE(result.fitness.used_width == Catch::Approx(6.0));
  REQUIRE(result.fitness.normalized_width_penalty == Catch::Approx(0.375));
  REQUIRE(result.fitness.score == Catch::Approx(2.375));
  REQUIRE(result.diagnostics.candidate_cache_misses == 1);
  REQUIRE(result.diagnostics.candidate_cache_hits == 2);
  REQUIRE(result.diagnostics.candidate_sets_generated == 1);
  REQUIRE(result.diagnostics.candidate_checks >= 3);
  REQUIRE(result.diagnostics.overlap_rejections >= 1);
  CHECK(result.placements[0].sheet_id == "sheet#1");
  CHECK(result.placements[1].sheet_id == "sheet#2");
}

TEST_CASE("ComputeGreedyPlacement uses hole candidates to reduce sheet count",
          "[engine]") {
  im2d::nesting::Sheet sheet;
  sheet.id = "sheet";
  sheet.quantity = 2;
  sheet.geometry.outer = MakeRectangle(0.0, 0.0, 4.0, 4.0);
  sheet.geometry.holes.push_back(MakeRectangle(10.0, 10.0, 14.0, 14.0));

  im2d::nesting::Part part;
  part.id = "part";
  part.quantity = 2;
  part.geometry.outer = MakeRectangle(0.0, 0.0, 3.0, 3.0);

  im2d::nesting::NestingProblem problem;
  problem.parts = {part};
  problem.sheets = {sheet};

  im2d::nesting::NestConfig without_holes_config;
  without_holes_config.use_holes = false;

  const auto without_holes =
      im2d::nesting::ComputeGreedyPlacement(problem, without_holes_config);
  const auto with_holes = im2d::nesting::ComputeGreedyPlacement(
      problem, im2d::nesting::NestConfig{});

  REQUIRE(without_holes.placements.size() == 2);
  REQUIRE(with_holes.placements.size() == 2);
  CHECK(without_holes.fitness.sheet_count == 2);
  CHECK(with_holes.fitness.sheet_count == 1);
  CHECK(without_holes.fitness.used_width == Catch::Approx(6.0));
  CHECK(without_holes.fitness.normalized_width_penalty == Catch::Approx(0.375));
  CHECK(without_holes.fitness.score == Catch::Approx(2.375));
  CHECK(with_holes.fitness.used_width == Catch::Approx(13.0));
  CHECK(with_holes.fitness.normalized_width_penalty == Catch::Approx(0.8125));
  CHECK(with_holes.fitness.score == Catch::Approx(1.8125));
  CHECK(with_holes.fitness.score < without_holes.fitness.score);
  CHECK(with_holes.diagnostics.candidate_cache_misses == 1);
  CHECK(with_holes.diagnostics.candidate_cache_hits == 1);
  CHECK(with_holes.diagnostics.candidate_sets_generated == 1);
  CHECK(with_holes.diagnostics.candidate_checks >= 2);

  bool saw_hole_placement = false;
  for (const auto &placement : with_holes.placements) {
    saw_hole_placement = saw_hole_placement || placement.placed_in_hole;
  }
  CHECK(saw_hole_placement);
}

TEST_CASE("ComputeGreedyPlacement penalizes unplaced parts ahead of width",
          "[engine]") {
  im2d::nesting::Sheet sheet;
  sheet.id = "sheet";
  sheet.quantity = 1;
  sheet.geometry.outer = MakeRectangle(0.0, 0.0, 4.0, 4.0);

  im2d::nesting::Part part;
  part.id = "part";
  part.quantity = 2;
  part.geometry.outer = MakeRectangle(0.0, 0.0, 3.0, 3.0);

  im2d::nesting::NestingProblem problem;
  problem.parts = {part};
  problem.sheets = {sheet};

  const auto result = im2d::nesting::ComputeGreedyPlacement(
      problem, im2d::nesting::NestConfig{});

  REQUIRE(result.placements.size() == 1);
  REQUIRE(result.fitness.unplaced_part_count == 1);
  REQUIRE(result.fitness.sheet_count == 1);
  REQUIRE(result.fitness.used_width == Catch::Approx(3.0));
  REQUIRE(result.fitness.normalized_width_penalty == Catch::Approx(0.1875));
  REQUIRE(result.fitness.score == Catch::Approx(3.1875));
}

TEST_CASE("Greedy placement fixtures match expected output",
          "[engine][fixture]") {
  const auto fixtures = im2d::nesting::test::LoadPlacementFixtures(
      std::filesystem::path("tests/nesting/fixtures/placement"));

  REQUIRE(fixtures.size() >= 3);

  for (const auto &fixture : fixtures) {
    INFO("fixture: " << fixture.name);

    im2d::nesting::NestingProblem problem;
    problem.sheets = {fixture.sheet};
    problem.parts = fixture.parts;

    const auto result =
        im2d::nesting::ComputeGreedyPlacement(problem, fixture.config);

    REQUIRE(result.fitness.sheet_count == fixture.expected_sheet_count);
    REQUIRE(result.fitness.unplaced_part_count ==
            fixture.expected_unplaced_part_count);
    REQUIRE(result.fitness.used_width ==
            Catch::Approx(fixture.expected_used_width));
    REQUIRE(result.fitness.normalized_width_penalty >= 0.0);
    REQUIRE(result.placements.size() == fixture.expected_placements.size());

    for (const auto &expected : fixture.expected_placements) {
      REQUIRE(HasExactPlacement(result.placements, expected));
    }
  }
}