#include "nesting_test_fixture.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace im2d::nesting::test {

namespace {

std::string Trim(std::string value) {
  auto is_space = [](unsigned char character) {
    return std::isspace(character) != 0;
  };

  value.erase(value.begin(), std::find_if(value.begin(), value.end(),
                                          [&](unsigned char character) {
                                            return !is_space(character);
                                          }));
  value.erase(std::find_if(
                  value.rbegin(), value.rend(),
                  [&](unsigned char character) { return !is_space(character); })
                  .base(),
              value.end());
  return value;
}

Contour ParseContour(const std::string &value) {
  Contour contour;
  std::stringstream point_stream(value);
  std::string point_token;
  while (std::getline(point_stream, point_token, ';')) {
    point_token = Trim(point_token);
    if (point_token.empty()) {
      continue;
    }

    const size_t comma_index = point_token.find(',');
    if (comma_index == std::string::npos) {
      throw std::runtime_error("invalid point token: " + point_token);
    }

    contour.push_back(PointD{
        .x = std::stod(Trim(point_token.substr(0, comma_index))),
        .y = std::stod(Trim(point_token.substr(comma_index + 1))),
    });
  }

  return contour;
}

PointD ParsePoint(const std::string &value) {
  const std::string trimmed = Trim(value);
  const size_t comma_index = trimmed.find(',');
  if (comma_index == std::string::npos) {
    throw std::runtime_error("invalid point token: " + trimmed);
  }

  return PointD{
      .x = std::stod(Trim(trimmed.substr(0, comma_index))),
      .y = std::stod(Trim(trimmed.substr(comma_index + 1))),
  };
}

NfpContours ParseExpectedContours(const std::string &value) {
  NfpContours contours;
  std::stringstream contour_stream(value);
  std::string contour_token;
  while (std::getline(contour_stream, contour_token, '|')) {
    contour_token = Trim(contour_token);
    if (contour_token.empty()) {
      continue;
    }
    contours.push_back(ParseContour(contour_token));
  }
  return contours;
}

bool ParseBool(const std::string &value) {
  const std::string trimmed = Trim(value);
  if (trimmed == "true") {
    return true;
  }
  if (trimmed == "false") {
    return false;
  }
  throw std::runtime_error("invalid boolean token: " + trimmed);
}

std::vector<Contour> ParseContourList(const std::string &value) {
  if (Trim(value) == "none") {
    return {};
  }
  return ParseExpectedContours(value);
}

Placement ParseExpectedPlacement(const std::string &value) {
  std::vector<std::string> fields;
  std::stringstream field_stream(value);
  std::string field;
  while (std::getline(field_stream, field, '|')) {
    fields.push_back(Trim(field));
  }

  if (fields.size() != 4) {
    throw std::runtime_error("invalid expected placement: " + value);
  }

  Placement placement;
  placement.part_id = fields[0];
  placement.sheet_id = fields[1];
  placement.translation = ParsePoint(fields[2]);
  placement.placed_in_hole = ParseBool(fields[3]);
  return placement;
}

} // namespace

InnerNfpFixture LoadInnerNfpFixture(const std::filesystem::path &path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("unable to open fixture: " + path.string());
  }

  InnerNfpFixture fixture;
  std::string line;
  while (std::getline(input, line)) {
    line = Trim(line);
    if (line.empty() || line[0] == '#') {
      continue;
    }

    const size_t separator = line.find('=');
    if (separator == std::string::npos) {
      throw std::runtime_error("invalid fixture line: " + line);
    }

    const std::string key = Trim(line.substr(0, separator));
    const std::string value = Trim(line.substr(separator + 1));
    if (key == "name") {
      fixture.name = value;
    } else if (key == "container") {
      fixture.container = ParseContour(value);
    } else if (key == "part") {
      fixture.part = ParseContour(value);
    } else if (key == "existing") {
      if (value == "none") {
        fixture.existing = std::nullopt;
      } else {
        fixture.existing = ParseExpectedContours(value);
      }
    } else if (key == "expected_start") {
      if (value == "none") {
        fixture.expected_start = std::nullopt;
      } else {
        fixture.expected_start = ParsePoint(value);
      }
    } else if (key == "expected") {
      if (value == "none") {
        fixture.expected = std::nullopt;
      } else {
        fixture.expected = ParseExpectedContours(value);
      }
    }
  }

  if (fixture.name.empty()) {
    fixture.name = path.stem().string();
  }
  if (fixture.container.empty()) {
    throw std::runtime_error("fixture missing container: " + path.string());
  }
  if (fixture.part.empty()) {
    throw std::runtime_error("fixture missing part: " + path.string());
  }
  if (!fixture.expected.has_value()) {
    return fixture;
  }
  return fixture;
}

std::vector<InnerNfpFixture>
LoadInnerNfpFixtures(const std::filesystem::path &directory) {
  std::vector<InnerNfpFixture> fixtures;
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    fixtures.push_back(LoadInnerNfpFixture(entry.path()));
  }

  std::sort(fixtures.begin(), fixtures.end(),
            [](const InnerNfpFixture &left, const InnerNfpFixture &right) {
              return left.name < right.name;
            });
  return fixtures;
}

PlacementFixture LoadPlacementFixture(const std::filesystem::path &path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("unable to open fixture: " + path.string());
  }

  PlacementFixture fixture;
  Part *current_part = nullptr;
  std::string line;
  while (std::getline(input, line)) {
    line = Trim(line);
    if (line.empty() || line[0] == '#') {
      continue;
    }

    const size_t separator = line.find('=');
    if (separator == std::string::npos) {
      throw std::runtime_error("invalid fixture line: " + line);
    }

    const std::string key = Trim(line.substr(0, separator));
    const std::string value = Trim(line.substr(separator + 1));

    if (key == "name") {
      fixture.name = value;
    } else if (key == "sheet_id") {
      fixture.sheet.id = value;
    } else if (key == "sheet_quantity") {
      fixture.sheet.quantity = std::stoi(value);
    } else if (key == "sheet_outer") {
      fixture.sheet.geometry.outer = ParseContour(value);
    } else if (key == "sheet_holes") {
      fixture.sheet.geometry.holes = ParseContourList(value);
    } else if (key == "use_holes") {
      fixture.config.use_holes = ParseBool(value);
    } else if (key == "part_id") {
      fixture.parts.push_back(Part{});
      current_part = &fixture.parts.back();
      current_part->id = value;
    } else if (key == "part_quantity") {
      if (current_part == nullptr) {
        throw std::runtime_error("part_quantity without part_id: " +
                                 path.string());
      }
      current_part->quantity = std::stoi(value);
    } else if (key == "part_outer") {
      if (current_part == nullptr) {
        throw std::runtime_error("part_outer without part_id: " +
                                 path.string());
      }
      current_part->geometry.outer = ParseContour(value);
    } else if (key == "part_holes") {
      if (current_part == nullptr) {
        throw std::runtime_error("part_holes without part_id: " +
                                 path.string());
      }
      current_part->geometry.holes = ParseContourList(value);
    } else if (key == "expected_sheet_count") {
      fixture.expected_sheet_count = std::stoi(value);
    } else if (key == "expected_unplaced_part_count") {
      fixture.expected_unplaced_part_count = std::stoi(value);
    } else if (key == "expected_used_width") {
      fixture.expected_used_width = std::stod(value);
    } else if (key == "expected_placement") {
      fixture.expected_placements.push_back(ParseExpectedPlacement(value));
    }
  }

  if (fixture.name.empty()) {
    fixture.name = path.stem().string();
  }
  if (fixture.sheet.id.empty()) {
    fixture.sheet.id = "sheet";
  }
  if (fixture.sheet.quantity <= 0) {
    fixture.sheet.quantity = 1;
  }
  if (fixture.sheet.geometry.outer.empty()) {
    throw std::runtime_error("fixture missing sheet_outer: " + path.string());
  }
  if (fixture.parts.empty()) {
    throw std::runtime_error("fixture missing parts: " + path.string());
  }
  for (const Part &part : fixture.parts) {
    if (part.id.empty()) {
      throw std::runtime_error("fixture part missing id: " + path.string());
    }
    if (part.geometry.outer.empty()) {
      throw std::runtime_error("fixture part missing outer: " + path.string());
    }
  }

  return fixture;
}

std::vector<PlacementFixture>
LoadPlacementFixtures(const std::filesystem::path &directory) {
  std::vector<PlacementFixture> fixtures;
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    fixtures.push_back(LoadPlacementFixture(entry.path()));
  }

  std::sort(fixtures.begin(), fixtures.end(),
            [](const PlacementFixture &left, const PlacementFixture &right) {
              return left.name < right.name;
            });
  return fixtures;
}

} // namespace im2d::nesting::test