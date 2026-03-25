#pragma once

#include <string>
#include <vector>

namespace im2d::nesting {

struct PointD {
  double x = 0.0;
  double y = 0.0;
};

using Contour = std::vector<PointD>;

struct PolygonWithHoles {
  Contour outer;
  std::vector<Contour> holes;
};

struct Part {
  std::string id;
  PolygonWithHoles geometry;
  int quantity = 1;
  std::vector<double> allowed_rotations_degrees;
};

struct Sheet {
  std::string id;
  PolygonWithHoles geometry;
  int quantity = 1;
};

struct Placement {
  std::string part_id;
  std::string sheet_id;
  PointD translation;
  double rotation_degrees = 0.0;
  bool placed_in_hole = false;
};

struct FitnessBreakdown {
  int unplaced_part_count = 0;
  int sheet_count = 0;
  double used_width = 0.0;
  double normalized_width_penalty = 0.0;
  double score = 0.0;
};

struct EngineDiagnostics {
  int candidate_cache_hits = 0;
  int candidate_cache_misses = 0;
  int candidate_sets_generated = 0;
  int candidate_checks = 0;
  int overlap_rejections = 0;
};

struct NestConfig {
  double spacing = 0.0;
  int population_size = 10;
  int mutation_rate = 10;
  bool use_holes = true;
  bool explore_concave = true;
};

struct NestingProblem {
  std::vector<Sheet> sheets;
  std::vector<Part> parts;
};

struct NestingResult {
  std::vector<Placement> placements;
  FitnessBreakdown fitness;
  EngineDiagnostics diagnostics;
};

} // namespace im2d::nesting