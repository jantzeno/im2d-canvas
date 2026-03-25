#include "../../src/nesting/im2d_nesting_geometry.h"
#include "../../src/nesting/im2d_nesting_nfp.h"

#include "nesting_test_fixture.h"

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>

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

bool NearlyEqual(double left, double right, double epsilon = 1e-9) {
  return std::abs(left - right) <= epsilon;
}

bool PointsEqual(const im2d::nesting::PointD &left,
                 const im2d::nesting::PointD &right, double epsilon = 1e-9) {
  return NearlyEqual(left.x, right.x, epsilon) &&
         NearlyEqual(left.y, right.y, epsilon);
}

bool IsPointOnContourBoundary(const im2d::nesting::PointD &point,
                              const im2d::nesting::Contour &contour,
                              double epsilon = 1e-9) {
  if (contour.empty()) {
    return false;
  }

  if (contour.size() == 1) {
    return PointsEqual(point, contour.front(), epsilon);
  }

  for (size_t index = 0; index < contour.size(); ++index) {
    if (im2d::nesting::IsPointOnSegment(point, contour[index],
                                        contour[(index + 1) % contour.size()],
                                        epsilon)) {
      return true;
    }
  }

  return false;
}

bool AreEquivalentContours(const im2d::nesting::Contour &actual,
                           const im2d::nesting::Contour &expected,
                           double epsilon = 1e-9) {
  if (actual.empty() || expected.empty()) {
    return actual.empty() == expected.empty();
  }

  const double actual_area = std::abs(im2d::nesting::SignedArea(actual));
  const double expected_area = std::abs(im2d::nesting::SignedArea(expected));
  if (!NearlyEqual(actual_area, expected_area, epsilon)) {
    return false;
  }

  for (const auto &point : actual) {
    if (!IsPointOnContourBoundary(point, expected, epsilon)) {
      return false;
    }
  }

  for (const auto &point : expected) {
    if (!IsPointOnContourBoundary(point, actual, epsilon)) {
      return false;
    }
  }

  return true;
}

bool AreEquivalentContourSets(const im2d::nesting::NfpContours &actual,
                              const im2d::nesting::NfpContours &expected,
                              double epsilon = 1e-9) {
  if (actual.size() != expected.size()) {
    return false;
  }

  std::vector<bool> matched_expected(expected.size(), false);
  for (const auto &actual_contour : actual) {
    bool matched = false;
    for (size_t expected_index = 0; expected_index < expected.size();
         ++expected_index) {
      if (matched_expected[expected_index]) {
        continue;
      }
      if (AreEquivalentContours(actual_contour, expected[expected_index],
                                epsilon)) {
        matched_expected[expected_index] = true;
        matched = true;
        break;
      }
    }
    if (!matched) {
      return false;
    }
  }

  for (bool matched : matched_expected) {
    if (!matched) {
      return false;
    }
  }

  return true;
}

bool HasExactContour(const im2d::nesting::NfpContours &actual,
                     const im2d::nesting::Contour &expected,
                     double epsilon = 1e-9) {
  return std::find_if(actual.begin(), actual.end(), [&](const auto &contour) {
           if (contour.size() != expected.size()) {
             return false;
           }
           for (size_t index = 0; index < contour.size(); ++index) {
             if (!PointsEqual(contour[index], expected[index], epsilon)) {
               return false;
             }
           }
           return true;
         }) != actual.end();
}

} // namespace

TEST_CASE("IsAxisAlignedRectangle accepts axis-aligned rectangles", "[nfp]") {
  const auto rectangle = MakeRectangle(0.0, 0.0, 10.0, 5.0);
  const im2d::nesting::Contour diamond{
      {0.0, 1.0}, {1.0, 2.0}, {2.0, 1.0}, {1.0, 0.0}};

  REQUIRE(im2d::nesting::IsAxisAlignedRectangle(rectangle));
  REQUIRE_FALSE(im2d::nesting::IsAxisAlignedRectangle(diamond));
}

TEST_CASE("ComputeRectangleInnerNfp matches SVGnest rectangle fast path",
          "[nfp]") {
  const auto container = MakeRectangle(0.0, 0.0, 10.0, 6.0);
  const auto part = MakeRectangle(3.0, 2.0, 5.0, 3.0);

  const auto nfp = im2d::nesting::ComputeRectangleInnerNfp(container, part);

  REQUIRE(nfp.has_value());
  REQUIRE(nfp->size() == 1);
  REQUIRE(nfp->front().size() == 4);
  REQUIRE(nfp->front()[0].x == 0.0);
  REQUIRE(nfp->front()[0].y == 0.0);
  REQUIRE(nfp->front()[1].x == 8.0);
  REQUIRE(nfp->front()[1].y == 0.0);
  REQUIRE(nfp->front()[2].x == 8.0);
  REQUIRE(nfp->front()[2].y == 5.0);
  REQUIRE(nfp->front()[3].x == 0.0);
  REQUIRE(nfp->front()[3].y == 5.0);
}

TEST_CASE("ComputeRectangleInnerNfp rejects parts larger than the container",
          "[nfp]") {
  const auto container = MakeRectangle(0.0, 0.0, 10.0, 6.0);
  const auto too_wide = MakeRectangle(0.0, 0.0, 12.0, 4.0);
  const auto too_tall = MakeRectangle(0.0, 0.0, 8.0, 7.0);

  REQUIRE_FALSE(
      im2d::nesting::ComputeRectangleInnerNfp(container, too_wide).has_value());
  REQUIRE_FALSE(
      im2d::nesting::ComputeRectangleInnerNfp(container, too_tall).has_value());
}

TEST_CASE("ComputeOuterNfp traces the outer loop for rectangles", "[nfp]") {
  const auto stationary = MakeRectangle(0.0, 0.0, 4.0, 3.0);
  const auto moving = MakeRectangle(0.0, 0.0, 2.0, 1.0);

  const auto nfp = im2d::nesting::ComputeOuterNfp(stationary, moving);

  REQUIRE(nfp.has_value());
  REQUIRE(nfp->size() == 1);

  const im2d::nesting::Contour expected{
      {-2.0, -1.0}, {4.0, -1.0}, {4.0, 3.0}, {-2.0, 3.0}};
  REQUIRE(AreEquivalentContourSets(*nfp, im2d::nesting::NfpContours{expected}));
  REQUIRE(HasExactContour(*nfp, expected));
}

TEST_CASE("ComputeOuterNfp traces a bounded outer loop for triangles",
          "[nfp]") {
  const im2d::nesting::Contour stationary{{0.0, 0.0}, {4.0, 0.0}, {0.0, 4.0}};
  const im2d::nesting::Contour moving{{0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}};

  const auto nfp = im2d::nesting::ComputeOuterNfp(stationary, moving);

  REQUIRE(nfp.has_value());
  REQUIRE(nfp->size() == 1);
  const im2d::nesting::Contour expected{
      {0.0, -1.0}, {0.0, 0.0}, {3.0, 0.0}, {0.0, 3.0}};
  REQUIRE(HasExactContour(*nfp, expected));
}

TEST_CASE("ComputeOuterNfp rejects invalid contours", "[nfp]") {
  const im2d::nesting::Contour line_like{{0.0, 0.0}, {1.0, 0.0}, {2.0, 0.0}};
  const auto moving = MakeRectangle(0.0, 0.0, 1.0, 1.0);

  REQUIRE_FALSE(im2d::nesting::ComputeOuterNfp(line_like, moving).has_value());
  REQUIRE_FALSE(im2d::nesting::ComputeOuterNfp(moving, line_like).has_value());
}

TEST_CASE("ComputeInnerNfp dispatches to the rectangle fast path", "[nfp]") {
  const auto container = MakeRectangle(-2.0, -1.0, 4.0, 5.0);
  const auto part = MakeRectangle(1.0, 1.0, 3.0, 2.0);

  const auto nfp = im2d::nesting::ComputeInnerNfp(container, part);

  REQUIRE(nfp.has_value());
  REQUIRE(nfp->front()[0].x == -2.0);
  REQUIRE(nfp->front()[0].y == -1.0);
  REQUIRE(nfp->front()[2].x == 2.0);
  REQUIRE(nfp->front()[2].y == 4.0);
}

TEST_CASE("ComputeInnerNfp traces a bounded loop for non-rectangle inputs",
          "[nfp]") {
  const im2d::nesting::Contour container{{0.0, 0.0}, {6.0, 0.0}, {0.0, 6.0}};
  const im2d::nesting::Contour part{{0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}};

  const auto nfp = im2d::nesting::ComputeInnerNfp(container, part);

  REQUIRE(nfp.has_value());
  REQUIRE(nfp->size() == 1);
  REQUIRE(nfp->front().size() == 3);
  REQUIRE(nfp->front()[0].x == Catch::Approx(0.0));
  REQUIRE(nfp->front()[0].y == Catch::Approx(0.0));
  REQUIRE(nfp->front()[1].x == Catch::Approx(5.0));
  REQUIRE(nfp->front()[1].y == Catch::Approx(0.0).margin(1e-9));
  REQUIRE(nfp->front()[2].x == Catch::Approx(0.0));
  REQUIRE(nfp->front()[2].y == Catch::Approx(5.0));
}

TEST_CASE("ComputeInnerNfp returns multiple contours for disconnected pockets",
          "[nfp]") {
  const im2d::nesting::Contour container{
      {0.0, 0.0},  {4.0, 0.0}, {4.0, 1.5}, {6.0, 1.5}, {6.0, 0.0}, {10.0, 0.0},
      {10.0, 4.0}, {6.0, 4.0}, {6.0, 2.5}, {4.0, 2.5}, {4.0, 4.0}, {0.0, 4.0}};
  const auto part = MakeRectangle(0.0, 0.0, 1.5, 1.5);

  const auto nfp = im2d::nesting::ComputeInnerNfp(container, part);

  REQUIRE(nfp.has_value());
  REQUIRE(nfp->size() == 2);

  const im2d::nesting::Contour left_expected{
      {0.0, 0.0}, {2.5, 0.0}, {2.5, 2.5}, {0.0, 2.5}};
  const im2d::nesting::Contour right_expected{
      {6.0, 0.0}, {8.5, 0.0}, {8.5, 2.5}, {6.0, 2.5}};

  REQUIRE(AreEquivalentContourSets(
      *nfp, im2d::nesting::NfpContours{left_expected, right_expected}));
  REQUIRE(std::all_of(nfp->begin(), nfp->end(),
                      [](const auto &contour) { return contour.size() == 4; }));
  REQUIRE(HasExactContour(*nfp, left_expected));
  REQUIRE(HasExactContour(*nfp, right_expected));
}

TEST_CASE("Inner-NFP fixtures match expected output", "[nfp][fixture]") {
  const auto fixtures = im2d::nesting::test::LoadInnerNfpFixtures(
      std::filesystem::path("tests/nesting/fixtures/nfp"));

  REQUIRE(fixtures.size() >= 5);

  for (const auto &fixture : fixtures) {
    INFO("fixture: " << fixture.name);
    const auto actual =
        im2d::nesting::ComputeInnerNfp(fixture.container, fixture.part);

    if (!fixture.expected.has_value()) {
      REQUIRE_FALSE(actual.has_value());
      continue;
    }

    REQUIRE(actual.has_value());
    REQUIRE(AreEquivalentContourSets(*actual, *fixture.expected));
    for (const auto &expected_contour : *fixture.expected) {
      REQUIRE(HasExactContour(*actual, expected_contour));
    }
  }
}

TEST_CASE("Outer-NFP fixtures match exact canonical output", "[nfp][fixture]") {
  const auto fixtures = im2d::nesting::test::LoadInnerNfpFixtures(
      std::filesystem::path("tests/nesting/fixtures/nfp_outer"));

  REQUIRE(fixtures.size() >= 2);

  for (const auto &fixture : fixtures) {
    INFO("fixture: " << fixture.name);
    const auto actual =
        im2d::nesting::ComputeOuterNfp(fixture.container, fixture.part);

    if (!fixture.expected.has_value()) {
      REQUIRE_FALSE(actual.has_value());
      continue;
    }

    REQUIRE(actual.has_value());
    REQUIRE(AreEquivalentContourSets(*actual, *fixture.expected));
    for (const auto &expected_contour : *fixture.expected) {
      REQUIRE(HasExactContour(*actual, expected_contour));
    }
  }
}

TEST_CASE("FindInnerNfpStartPoint finds fixture-backed non-rectangle starts",
          "[nfp][fixture]") {
  const auto fixtures = im2d::nesting::test::LoadInnerNfpFixtures(
      std::filesystem::path("tests/nesting/fixtures/nfp_start"));

  REQUIRE(fixtures.size() >= 3);

  for (const auto &fixture : fixtures) {
    INFO("fixture: " << fixture.name);
    const im2d::nesting::NfpContours existing =
        fixture.existing.value_or(im2d::nesting::NfpContours{});

    const auto actual = im2d::nesting::FindInnerNfpStartPoint(
        fixture.container, fixture.part, existing);

    if (!fixture.expected_start.has_value()) {
      REQUIRE_FALSE(actual.has_value());
      continue;
    }

    REQUIRE(actual.has_value());
    REQUIRE(actual->x == fixture.expected_start->x);
    REQUIRE(actual->y == fixture.expected_start->y);
  }
}

TEST_CASE(
    "ComputeSlideVector caps movement to SVGnest-style projection distance",
    "[nfp]") {
  const im2d::nesting::Contour stationary{
      {0.0, 0.0}, {4.0, 0.0}, {4.0, 1.0}, {0.0, 1.0}};
  const im2d::nesting::Contour moving{
      {1.0, 3.0}, {2.0, 3.0}, {2.0, 4.0}, {1.0, 4.0}};

  const auto slide =
      im2d::nesting::ComputeSlideVector(stationary, moving, {0.0, -5.0});

  REQUIRE(slide.has_value());
  REQUIRE(slide->x == 0.0);
  REQUIRE(slide->y == Catch::Approx(-2.0));
}

TEST_CASE("ComputeSlideVector rejects zero-length directions", "[nfp]") {
  const auto rectangle = MakeRectangle(0.0, 0.0, 2.0, 2.0);
  const auto slide =
      im2d::nesting::ComputeSlideVector(rectangle, rectangle, {0.0, 0.0});

  REQUIRE_FALSE(slide.has_value());
}

TEST_CASE("ComputeTouchingCandidateVectors finds vertex contact directions",
          "[nfp]") {
  const auto stationary = MakeRectangle(0.0, 0.0, 2.0, 2.0);
  const auto moving = MakeRectangle(0.0, 0.0, 1.0, 1.0);

  const auto vectors = im2d::nesting::ComputeTouchingCandidateVectors(
      stationary, moving, {2.0, 2.0});

  REQUIRE(vectors.size() == 4);
  REQUIRE(std::find_if(vectors.begin(), vectors.end(), [](const auto &vector) {
            return vector.x == 0.0 && vector.y == -2.0;
          }) != vectors.end());
  REQUIRE(std::find_if(vectors.begin(), vectors.end(), [](const auto &vector) {
            return vector.x == -2.0 && vector.y == 0.0;
          }) != vectors.end());
  REQUIRE(std::find_if(vectors.begin(), vectors.end(), [](const auto &vector) {
            return vector.x == 0.0 && vector.y == -1.0;
          }) != vectors.end());
  REQUIRE(std::find_if(vectors.begin(), vectors.end(), [](const auto &vector) {
            return vector.x == -1.0 && vector.y == 0.0;
          }) != vectors.end());
}

TEST_CASE("ComputeTouchingCandidateVectors returns no vectors without contact",
          "[nfp]") {
  const auto stationary = MakeRectangle(0.0, 0.0, 2.0, 2.0);
  const auto moving = MakeRectangle(0.0, 0.0, 1.0, 1.0);

  const auto vectors = im2d::nesting::ComputeTouchingCandidateVectors(
      stationary, moving, {5.0, 5.0});

  REQUIRE(vectors.empty());
}

TEST_CASE("SelectBestTranslationVector chooses the longest valid move",
          "[nfp]") {
  const auto stationary = MakeRectangle(0.0, 0.0, 2.0, 2.0);
  const auto moving = MakeRectangle(0.0, 0.0, 1.0, 1.0);

  const auto best = im2d::nesting::SelectBestTranslationVector(
      stationary, moving, {2.0, 2.0});

  REQUIRE(best.has_value());
  REQUIRE(best->x == Catch::Approx(0.0));
  REQUIRE(best->y == Catch::Approx(-2.0));
}

TEST_CASE("SelectBestTranslationVector skips immediate reverse directions",
          "[nfp]") {
  const auto stationary = MakeRectangle(0.0, 0.0, 2.0, 2.0);
  const auto moving = MakeRectangle(0.0, 0.0, 1.0, 1.0);

  const auto best = im2d::nesting::SelectBestTranslationVector(
      stationary, moving, {2.0, 2.0}, im2d::nesting::PointD{0.0, 2.0});

  REQUIRE(best.has_value());
  REQUIRE(best->x == Catch::Approx(-2.0));
  REQUIRE(best->y == Catch::Approx(0.0));
}