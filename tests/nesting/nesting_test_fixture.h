#pragma once

#include "../../src/nesting/im2d_nesting_nfp.h"
#include "../../src/nesting/im2d_nesting_types.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace im2d::nesting::test {

struct InnerNfpFixture {
  std::string name;
  Contour container;
  Contour part;
  std::optional<NfpContours> existing;
  std::optional<PointD> expected_start;
  std::optional<NfpContours> expected;
};

InnerNfpFixture LoadInnerNfpFixture(const std::filesystem::path &path);
std::vector<InnerNfpFixture>
LoadInnerNfpFixtures(const std::filesystem::path &directory);

struct PlacementFixture {
  std::string name;
  Sheet sheet;
  std::vector<Part> parts;
  NestConfig config;
  int expected_sheet_count = 0;
  int expected_unplaced_part_count = 0;
  double expected_used_width = 0.0;
  std::vector<Placement> expected_placements;
};

PlacementFixture LoadPlacementFixture(const std::filesystem::path &path);
std::vector<PlacementFixture>
LoadPlacementFixtures(const std::filesystem::path &directory);

} // namespace im2d::nesting::test