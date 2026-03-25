#include "im2d_nesting_engine.h"

#include "im2d_nesting_geometry.h"
#include "im2d_nesting_nfp.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>

namespace im2d::nesting {

namespace {

struct CandidateCacheKey {
  const Sheet *sheet = nullptr;
  const Part *part = nullptr;
  bool use_holes = true;

  bool operator<(const CandidateCacheKey &other) const {
    if (sheet != other.sheet) {
      return sheet < other.sheet;
    }
    if (part != other.part) {
      return part < other.part;
    }
    return use_holes < other.use_holes;
  }
};

struct SheetInstance {
  const Sheet *sheet = nullptr;
  std::string instance_id;
  std::vector<Contour> occupied;
  double used_width = 0.0;
  bool has_placements = false;
};

bool NearlyEqual(double left, double right, double epsilon) {
  return std::abs(left - right) <= epsilon;
}

bool ContoursStrictlyIntersect(const Contour &a, const Contour &b,
                               double epsilon) {
  if (a.size() < 2 || b.size() < 2) {
    return false;
  }

  for (size_t i = 0; i < a.size(); ++i) {
    const PointD &a_start = a[i];
    const PointD &a_end = a[(i + 1) % a.size()];
    for (size_t j = 0; j < b.size(); ++j) {
      const PointD &b_start = b[j];
      const PointD &b_end = b[(j + 1) % b.size()];
      const SegmentIntersection intersection =
          IntersectSegments(a_start, a_end, b_start, b_end, epsilon);
      if (!intersection.intersects) {
        continue;
      }
      if (intersection.collinear_overlap || !intersection.touches_at_endpoint) {
        return true;
      }
    }
  }

  return false;
}

bool ContoursOverlapOrContain(const Contour &a, const Contour &b,
                              double epsilon) {
  if (ContoursStrictlyIntersect(a, b, epsilon)) {
    return true;
  }

  for (const PointD &point : a) {
    if (IsPointInContour(point, b, false, epsilon)) {
      return true;
    }
  }
  for (const PointD &point : b) {
    if (IsPointInContour(point, a, false, epsilon)) {
      return true;
    }
  }

  return false;
}

bool SamePlacement(const Placement &left, const Placement &right,
                   double epsilon) {
  return left.part_id == right.part_id && left.sheet_id == right.sheet_id &&
         left.placed_in_hole == right.placed_in_hole &&
         NearlyEqual(left.rotation_degrees, right.rotation_degrees, epsilon) &&
         NearlyEqual(left.translation.x, right.translation.x, epsilon) &&
         NearlyEqual(left.translation.y, right.translation.y, epsilon);
}

void AppendPlacementsFromNfp(std::vector<Placement> *placements,
                             const NfpContours &nfp, const Part &part,
                             const Sheet &sheet, bool placed_in_hole,
                             double epsilon) {
  if (placements == nullptr || part.geometry.outer.empty()) {
    return;
  }

  const PointD &anchor = part.geometry.outer.front();
  for (const Contour &contour : nfp) {
    for (const PointD &point : contour) {
      Placement placement;
      placement.part_id = part.id;
      placement.sheet_id = sheet.id;
      placement.translation = {point.x - anchor.x, point.y - anchor.y};
      placement.rotation_degrees = 0.0;
      placement.placed_in_hole = placed_in_hole;

      bool duplicate = false;
      for (const Placement &existing : *placements) {
        if (SamePlacement(existing, placement, epsilon)) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate) {
        placements->push_back(placement);
      }
    }
  }
}

std::string BuildSheetInstanceId(const Sheet &sheet, int instance_index) {
  if (sheet.quantity <= 1) {
    return sheet.id;
  }

  return sheet.id + "#" + std::to_string(instance_index + 1);
}

bool PlacementLess(const Placement &left, const Placement &right) {
  if (left.placed_in_hole != right.placed_in_hole) {
    return left.placed_in_hole && !right.placed_in_hole;
  }
  if (!NearlyEqual(left.translation.y, right.translation.y, 1e-9)) {
    return left.translation.y < right.translation.y;
  }
  if (!NearlyEqual(left.translation.x, right.translation.x, 1e-9)) {
    return left.translation.x < right.translation.x;
  }
  return left.sheet_id < right.sheet_id;
}

bool CanPlacePart(const Part &part, const Placement &placement,
                  const std::vector<Contour> &occupied,
                  EngineDiagnostics *diagnostics, double epsilon) {
  const Contour translated =
      TranslateContour(part.geometry.outer, placement.translation);
  for (const Contour &existing : occupied) {
    if (ContoursOverlapOrContain(translated, existing, epsilon)) {
      if (diagnostics != nullptr) {
        diagnostics->overlap_rejections += 1;
      }
      return false;
    }
  }
  return true;
}

double ComputePlacementMaxX(const Part &part, const Placement &placement) {
  const BoundsD bounds = ComputeBounds(part.geometry.outer);
  if (!bounds.valid) {
    return placement.translation.x;
  }

  return placement.translation.x + bounds.max.x;
}

double ComputeSheetArea(const Sheet &sheet) {
  if (!IsContourValid(sheet.geometry.outer, 1e-9)) {
    return 0.0;
  }
  return std::abs(SignedArea(sheet.geometry.outer));
}

} // namespace

NestingProblem NormalizeProblemGeometry(NestingProblem problem) {
  for (Sheet &sheet : problem.sheets) {
    sheet.geometry = NormalizePolygonWinding(sheet.geometry);
  }

  for (Part &part : problem.parts) {
    part.geometry = NormalizePolygonWinding(part.geometry);
  }

  return problem;
}

std::vector<Placement> GeneratePlacementCandidates(const Sheet &sheet,
                                                   const Part &part,
                                                   const NestConfig &config,
                                                   double epsilon) {
  std::vector<Placement> placements;
  if (!IsContourValid(sheet.geometry.outer, epsilon) ||
      !IsContourValid(part.geometry.outer, epsilon)) {
    return placements;
  }

  if (const auto outer_nfp =
          ComputeInnerNfp(sheet.geometry.outer, part.geometry.outer, epsilon);
      outer_nfp.has_value()) {
    AppendPlacementsFromNfp(&placements, *outer_nfp, part, sheet, false,
                            epsilon);
  }

  if (!config.use_holes) {
    return placements;
  }

  for (const Contour &hole : sheet.geometry.holes) {
    if (!IsContourValid(hole, epsilon)) {
      continue;
    }

    const auto hole_nfp = ComputeInnerNfp(hole, part.geometry.outer, epsilon);
    if (!hole_nfp.has_value()) {
      continue;
    }

    AppendPlacementsFromNfp(&placements, *hole_nfp, part, sheet, true, epsilon);
  }

  return placements;
}

NestingResult ComputeGreedyPlacement(const NestingProblem &problem,
                                     const NestConfig &config, double epsilon) {
  NestingResult result;
  std::map<CandidateCacheKey, std::vector<Placement>> candidate_cache;

  std::vector<SheetInstance> sheets;
  for (const Sheet &sheet : problem.sheets) {
    const int quantity = std::max(sheet.quantity, 0);
    for (int instance_index = 0; instance_index < quantity; ++instance_index) {
      sheets.push_back(SheetInstance{
          &sheet, BuildSheetInstanceId(sheet, instance_index), {}});
    }
  }

  std::vector<const Part *> parts_to_place;
  for (const Part &part : problem.parts) {
    const int quantity = std::max(part.quantity, 0);
    for (int instance_index = 0; instance_index < quantity; ++instance_index) {
      parts_to_place.push_back(&part);
    }
  }

  for (const Part *part : parts_to_place) {
    if (part == nullptr) {
      continue;
    }

    bool placed = false;
    for (SheetInstance &sheet_instance : sheets) {
      const CandidateCacheKey cache_key{sheet_instance.sheet, part,
                                        config.use_holes};
      std::vector<Placement> candidates;

      if (const auto cache_it = candidate_cache.find(cache_key);
          cache_it != candidate_cache.end()) {
        candidates = cache_it->second;
        result.diagnostics.candidate_cache_hits += 1;
      } else {
        candidates = GeneratePlacementCandidates(*sheet_instance.sheet, *part,
                                                 config, epsilon);
        candidate_cache.emplace(cache_key, candidates);
        result.diagnostics.candidate_cache_misses += 1;
        result.diagnostics.candidate_sets_generated += 1;
      }

      for (Placement &candidate : candidates) {
        candidate.sheet_id = sheet_instance.instance_id;
      }
      std::sort(candidates.begin(), candidates.end(), PlacementLess);

      for (const Placement &candidate : candidates) {
        result.diagnostics.candidate_checks += 1;
        if (!CanPlacePart(*part, candidate, sheet_instance.occupied,
                          &result.diagnostics, epsilon)) {
          continue;
        }

        result.placements.push_back(candidate);
        sheet_instance.occupied.push_back(
            TranslateContour(part->geometry.outer, candidate.translation));
        sheet_instance.used_width = std::max(
            sheet_instance.used_width, ComputePlacementMaxX(*part, candidate));
        sheet_instance.has_placements = true;
        placed = true;
        break;
      }

      if (placed) {
        break;
      }
    }

    if (!placed) {
      result.fitness.unplaced_part_count += 1;
    }
  }

  std::set<std::string> used_sheet_ids;
  for (const Placement &placement : result.placements) {
    used_sheet_ids.insert(placement.sheet_id);
  }

  double used_width = 0.0;
  double normalized_width_penalty = 0.0;
  for (const SheetInstance &sheet_instance : sheets) {
    if (!sheet_instance.has_placements) {
      continue;
    }

    used_width += sheet_instance.used_width;
    const double sheet_area = ComputeSheetArea(*sheet_instance.sheet);
    if (sheet_area > epsilon) {
      normalized_width_penalty += sheet_instance.used_width / sheet_area;
    }
  }

  result.fitness.sheet_count = static_cast<int>(used_sheet_ids.size());
  result.fitness.used_width = used_width;
  result.fitness.normalized_width_penalty = normalized_width_penalty;
  result.fitness.score = (result.fitness.unplaced_part_count * 2.0) +
                         result.fitness.sheet_count +
                         result.fitness.normalized_width_penalty;
  return result;
}

} // namespace im2d::nesting