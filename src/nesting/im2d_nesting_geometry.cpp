#include "im2d_nesting_geometry.h"

#include <algorithm>
#include <cmath>

namespace im2d::nesting {

namespace {

double CrossProduct(const PointD &origin, const PointD &a, const PointD &b) {
  return ((a.x - origin.x) * (b.y - origin.y)) -
         ((a.y - origin.y) * (b.x - origin.x));
}

bool NearlyEqual(double a, double b, double epsilon) {
  return std::abs(a - b) <= epsilon;
}

bool PointsEqual(const PointD &a, const PointD &b, double epsilon) {
  return NearlyEqual(a.x, b.x, epsilon) && NearlyEqual(a.y, b.y, epsilon);
}

std::optional<double> SegmentSlideDistance(const PointD &a, const PointD &b,
                                           const PointD &e, const PointD &f,
                                           const PointD &direction,
                                           double epsilon) {
  const PointD normal{direction.y, -direction.x};
  const PointD reverse{-direction.x, -direction.y};

  const double dot_a = DotProduct(a, normal);
  const double dot_b = DotProduct(b, normal);
  const double dot_e = DotProduct(e, normal);
  const double dot_f = DotProduct(f, normal);

  const double cross_a = DotProduct(a, direction);
  const double cross_b = DotProduct(b, direction);
  const double cross_e = DotProduct(e, direction);
  const double cross_f = DotProduct(f, direction);

  const double ab_min = std::min(dot_a, dot_b);
  const double ab_max = std::max(dot_a, dot_b);
  const double ef_min = std::min(dot_e, dot_f);
  const double ef_max = std::max(dot_e, dot_f);

  if (NearlyEqual(ab_max, ef_min, epsilon) ||
      NearlyEqual(ab_min, ef_max, epsilon)) {
    return std::nullopt;
  }
  if (ab_max < ef_min || ab_min > ef_max) {
    return std::nullopt;
  }

  double overlap = 0.0;
  if ((ab_max > ef_max && ab_min < ef_min) ||
      (ef_max > ab_max && ef_min < ab_min)) {
    overlap = 1.0;
  } else {
    const double min_max = std::min(ab_max, ef_max);
    const double max_min = std::max(ab_min, ef_min);
    const double max_max = std::max(ab_max, ef_max);
    const double min_min = std::min(ab_min, ef_min);
    overlap = (min_max - max_min) / (max_max - min_min);
  }

  const double cross_abe =
      ((e.y - a.y) * (b.x - a.x)) - ((e.x - a.x) * (b.y - a.y));
  const double cross_abf =
      ((f.y - a.y) * (b.x - a.x)) - ((f.x - a.x) * (b.y - a.y));

  if (NearlyEqual(cross_abe, 0.0, epsilon) &&
      NearlyEqual(cross_abf, 0.0, epsilon)) {
    PointD ab_normal{b.y - a.y, a.x - b.x};
    PointD ef_normal{f.y - e.y, e.x - f.x};

    const auto normalized_ab = NormalizeVector(ab_normal, epsilon);
    const auto normalized_ef = NormalizeVector(ef_normal, epsilon);
    if (!normalized_ab.has_value() || !normalized_ef.has_value()) {
      return std::nullopt;
    }

    const double cross_norm = (normalized_ab->y * normalized_ef->x) -
                              (normalized_ab->x * normalized_ef->y);
    const double dot_norm = DotProduct(*normalized_ab, *normalized_ef);
    if (std::abs(cross_norm) < epsilon && dot_norm < 0.0) {
      const double norm_dot = DotProduct(*normalized_ab, direction);
      if (NearlyEqual(norm_dot, 0.0, epsilon)) {
        return std::nullopt;
      }
      if (norm_dot < 0.0) {
        return 0.0;
      }
    }
    return std::nullopt;
  }

  std::vector<double> distances;

  if (NearlyEqual(dot_a, dot_e, epsilon)) {
    distances.push_back(cross_a - cross_e);
  } else if (NearlyEqual(dot_a, dot_f, epsilon)) {
    distances.push_back(cross_a - cross_f);
  } else if (dot_a > ef_min && dot_a < ef_max) {
    auto distance =
        PointDistanceAlongDirection(a, e, f, reverse, false, epsilon);
    if (distance.has_value() && NearlyEqual(*distance, 0.0, epsilon)) {
      const auto other =
          PointDistanceAlongDirection(b, e, f, reverse, true, epsilon);
      if (other.has_value() &&
          (*other < 0.0 || NearlyEqual((*other) * overlap, 0.0, epsilon))) {
        distance = std::nullopt;
      }
    }
    if (distance.has_value()) {
      distances.push_back(*distance);
    }
  }

  if (NearlyEqual(dot_b, dot_e, epsilon)) {
    distances.push_back(cross_b - cross_e);
  } else if (NearlyEqual(dot_b, dot_f, epsilon)) {
    distances.push_back(cross_b - cross_f);
  } else if (dot_b > ef_min && dot_b < ef_max) {
    auto distance =
        PointDistanceAlongDirection(b, e, f, reverse, false, epsilon);
    if (distance.has_value() && NearlyEqual(*distance, 0.0, epsilon)) {
      const auto other =
          PointDistanceAlongDirection(a, e, f, reverse, true, epsilon);
      if (other.has_value() &&
          (*other < 0.0 || NearlyEqual((*other) * overlap, 0.0, epsilon))) {
        distance = std::nullopt;
      }
    }
    if (distance.has_value()) {
      distances.push_back(*distance);
    }
  }

  if (dot_e > ab_min && dot_e < ab_max) {
    auto distance =
        PointDistanceAlongDirection(e, a, b, direction, false, epsilon);
    if (distance.has_value() && NearlyEqual(*distance, 0.0, epsilon)) {
      const auto other =
          PointDistanceAlongDirection(f, a, b, direction, true, epsilon);
      if (other.has_value() &&
          (*other < 0.0 || NearlyEqual((*other) * overlap, 0.0, epsilon))) {
        distance = std::nullopt;
      }
    }
    if (distance.has_value()) {
      distances.push_back(*distance);
    }
  }

  if (dot_f > ab_min && dot_f < ab_max) {
    auto distance =
        PointDistanceAlongDirection(f, a, b, direction, false, epsilon);
    if (distance.has_value() && NearlyEqual(*distance, 0.0, epsilon)) {
      const auto other =
          PointDistanceAlongDirection(e, a, b, direction, true, epsilon);
      if (other.has_value() &&
          (*other < 0.0 || NearlyEqual((*other) * overlap, 0.0, epsilon))) {
        distance = std::nullopt;
      }
    }
    if (distance.has_value()) {
      distances.push_back(*distance);
    }
  }

  if (distances.empty()) {
    return std::nullopt;
  }

  return *std::min_element(distances.begin(), distances.end());
}

void ReverseIfNeeded(Contour *contour, bool should_be_clockwise) {
  if (contour == nullptr || contour->size() < 3) {
    return;
  }

  const bool is_clockwise = IsClockwise(*contour);
  if (is_clockwise != should_be_clockwise) {
    std::reverse(contour->begin(), contour->end());
  }
}

} // namespace

double DotProduct(const PointD &a, const PointD &b) {
  return (a.x * b.x) + (a.y * b.y);
}

double LengthSquared(const PointD &vector) {
  return DotProduct(vector, vector);
}

std::optional<PointD> NormalizeVector(const PointD &vector, double epsilon) {
  const double length_squared = LengthSquared(vector);
  if (length_squared <= epsilon * epsilon) {
    return std::nullopt;
  }

  const double length = std::sqrt(length_squared);
  if (NearlyEqual(length, 1.0, epsilon)) {
    return vector;
  }

  return PointD{vector.x / length, vector.y / length};
}

double SignedArea(const Contour &contour) {
  if (contour.size() < 3) {
    return 0.0;
  }

  double doubled_area = 0.0;
  for (size_t i = 0; i < contour.size(); ++i) {
    const PointD &current = contour[i];
    const PointD &next = contour[(i + 1) % contour.size()];
    doubled_area += (current.x * next.y) - (next.x * current.y);
  }

  return doubled_area * 0.5;
}

bool IsClockwise(const Contour &contour) { return SignedArea(contour) < 0.0; }

BoundsD ComputeBounds(const Contour &contour) {
  BoundsD bounds;
  if (contour.empty()) {
    return bounds;
  }

  bounds.min = contour.front();
  bounds.max = contour.front();
  bounds.valid = true;

  for (const PointD &point : contour) {
    bounds.min.x = std::min(bounds.min.x, point.x);
    bounds.min.y = std::min(bounds.min.y, point.y);
    bounds.max.x = std::max(bounds.max.x, point.x);
    bounds.max.y = std::max(bounds.max.y, point.y);
  }

  return bounds;
}

bool IsPointOnSegment(const PointD &point, const PointD &segment_start,
                      const PointD &segment_end, double epsilon) {
  if (std::abs(CrossProduct(segment_start, segment_end, point)) > epsilon) {
    return false;
  }

  const double min_x = std::min(segment_start.x, segment_end.x) - epsilon;
  const double max_x = std::max(segment_start.x, segment_end.x) + epsilon;
  const double min_y = std::min(segment_start.y, segment_end.y) - epsilon;
  const double max_y = std::max(segment_start.y, segment_end.y) + epsilon;
  return point.x >= min_x && point.x <= max_x && point.y >= min_y &&
         point.y <= max_y;
}

SegmentIntersection IntersectSegments(const PointD &a_start,
                                      const PointD &a_end,
                                      const PointD &b_start,
                                      const PointD &b_end, double epsilon) {
  SegmentIntersection result;

  const double denominator = ((a_end.x - a_start.x) * (b_end.y - b_start.y)) -
                             ((a_end.y - a_start.y) * (b_end.x - b_start.x));
  const double numerator_a = ((b_start.x - a_start.x) * (b_end.y - b_start.y)) -
                             ((b_start.y - a_start.y) * (b_end.x - b_start.x));
  const double numerator_b = ((b_start.x - a_start.x) * (a_end.y - a_start.y)) -
                             ((b_start.y - a_start.y) * (a_end.x - a_start.x));

  if (NearlyEqual(denominator, 0.0, epsilon)) {
    if (!NearlyEqual(numerator_a, 0.0, epsilon) ||
        !NearlyEqual(numerator_b, 0.0, epsilon)) {
      return result;
    }

    const bool overlap = IsPointOnSegment(a_start, b_start, b_end, epsilon) ||
                         IsPointOnSegment(a_end, b_start, b_end, epsilon) ||
                         IsPointOnSegment(b_start, a_start, a_end, epsilon) ||
                         IsPointOnSegment(b_end, a_start, a_end, epsilon);
    if (!overlap) {
      return result;
    }

    result.intersects = true;
    result.collinear_overlap = true;
    if (IsPointOnSegment(a_start, b_start, b_end, epsilon)) {
      result.point = a_start;
    } else if (IsPointOnSegment(a_end, b_start, b_end, epsilon)) {
      result.point = a_end;
    } else if (IsPointOnSegment(b_start, a_start, a_end, epsilon)) {
      result.point = b_start;
    } else {
      result.point = b_end;
    }
    result.touches_at_endpoint = PointsEqual(result.point, a_start, epsilon) ||
                                 PointsEqual(result.point, a_end, epsilon) ||
                                 PointsEqual(result.point, b_start, epsilon) ||
                                 PointsEqual(result.point, b_end, epsilon);
    return result;
  }

  const double ua = numerator_a / denominator;
  const double ub = numerator_b / denominator;
  if (ua < -epsilon || ua > 1.0 + epsilon || ub < -epsilon ||
      ub > 1.0 + epsilon) {
    return result;
  }

  result.intersects = true;
  result.point = {
      a_start.x + (ua * (a_end.x - a_start.x)),
      a_start.y + (ua * (a_end.y - a_start.y)),
  };
  result.touches_at_endpoint = PointsEqual(result.point, a_start, epsilon) ||
                               PointsEqual(result.point, a_end, epsilon) ||
                               PointsEqual(result.point, b_start, epsilon) ||
                               PointsEqual(result.point, b_end, epsilon);
  return result;
}

bool IsPointInContour(const PointD &point, const Contour &contour,
                      bool include_boundary, double epsilon) {
  if (contour.size() < 3) {
    return false;
  }

  bool inside = false;
  for (size_t i = 0, j = contour.size() - 1; i < contour.size(); j = i++) {
    const PointD &current = contour[i];
    const PointD &previous = contour[j];

    if (IsPointOnSegment(point, previous, current, epsilon)) {
      return include_boundary;
    }

    const bool crosses_scanline =
        ((current.y > point.y) != (previous.y > point.y));
    if (!crosses_scanline) {
      continue;
    }

    const double x_intersection =
        previous.x + ((current.x - previous.x) * (point.y - previous.y)) /
                         (current.y - previous.y);
    if (x_intersection >= point.x - epsilon) {
      inside = !inside;
    }
  }

  return inside;
}

bool IsContourSimple(const Contour &contour, double epsilon) {
  if (contour.size() < 3) {
    return false;
  }

  for (size_t i = 0; i < contour.size(); ++i) {
    const PointD &a_start = contour[i];
    const PointD &a_end = contour[(i + 1) % contour.size()];
    if (PointsEqual(a_start, a_end, epsilon)) {
      return false;
    }

    for (size_t j = i + 1; j < contour.size(); ++j) {
      const size_t a_next = (i + 1) % contour.size();
      const size_t b_next = (j + 1) % contour.size();
      if (i == j || a_next == j || b_next == i) {
        continue;
      }
      if (i == 0 && b_next == 0) {
        continue;
      }

      const SegmentIntersection intersection = IntersectSegments(
          a_start, a_end, contour[j], contour[b_next], epsilon);
      if (intersection.intersects) {
        return false;
      }
    }
  }

  return true;
}

bool IsContourValid(const Contour &contour, double epsilon) {
  if (contour.size() < 3) {
    return false;
  }
  if (std::abs(SignedArea(contour)) <= epsilon) {
    return false;
  }
  return IsContourSimple(contour, epsilon);
}

std::optional<double>
PointDistanceAlongDirection(const PointD &point, const PointD &segment_start,
                            const PointD &segment_end, const PointD &direction,
                            bool infinite, double epsilon) {
  const auto normalized_direction = NormalizeVector(direction, epsilon);
  if (!normalized_direction.has_value()) {
    return std::nullopt;
  }

  const PointD tangent{normalized_direction->y, -normalized_direction->x};

  const double point_tangent = DotProduct(point, tangent);
  const double start_tangent = DotProduct(segment_start, tangent);
  const double end_tangent = DotProduct(segment_end, tangent);

  const double point_normal = DotProduct(point, *normalized_direction);
  const double start_normal = DotProduct(segment_start, *normalized_direction);
  const double end_normal = DotProduct(segment_end, *normalized_direction);

  if (!infinite) {
    const bool before_both =
        ((point_tangent < start_tangent ||
          NearlyEqual(point_tangent, start_tangent, epsilon)) &&
         (point_tangent < end_tangent ||
          NearlyEqual(point_tangent, end_tangent, epsilon)));
    const bool after_both =
        ((point_tangent > start_tangent ||
          NearlyEqual(point_tangent, start_tangent, epsilon)) &&
         (point_tangent > end_tangent ||
          NearlyEqual(point_tangent, end_tangent, epsilon)));
    if (before_both || after_both) {
      return std::nullopt;
    }

    if (NearlyEqual(point_tangent, start_tangent, epsilon) &&
        NearlyEqual(point_tangent, end_tangent, epsilon)) {
      if (point_normal > start_normal && point_normal > end_normal) {
        return std::min(point_normal - start_normal, point_normal - end_normal);
      }
      if (point_normal < start_normal && point_normal < end_normal) {
        return -std::min(start_normal - point_normal,
                         end_normal - point_normal);
      }
    }
  }

  const double tangent_delta = start_tangent - end_tangent;
  if (NearlyEqual(tangent_delta, 0.0, epsilon)) {
    return std::nullopt;
  }

  const double interpolated = point_normal - start_normal +
                              ((start_normal - end_normal) *
                               (start_tangent - point_tangent) / tangent_delta);
  return -interpolated;
}

std::optional<double> PolygonSlideDistance(const Contour &stationary,
                                           const Contour &moving,
                                           const PointD &direction,
                                           bool ignore_negative,
                                           double epsilon) {
  if (stationary.size() < 2 || moving.size() < 2) {
    return std::nullopt;
  }

  const auto normalized_direction = NormalizeVector(direction, epsilon);
  if (!normalized_direction.has_value()) {
    return std::nullopt;
  }

  std::optional<double> distance;
  for (size_t moving_index = 0; moving_index < moving.size(); ++moving_index) {
    const PointD &b1 = moving[moving_index];
    const PointD &b2 = moving[(moving_index + 1) % moving.size()];
    if (PointsEqual(b1, b2, epsilon)) {
      continue;
    }

    for (size_t stationary_index = 0; stationary_index < stationary.size();
         ++stationary_index) {
      const PointD &a1 = stationary[stationary_index];
      const PointD &a2 = stationary[(stationary_index + 1) % stationary.size()];
      if (PointsEqual(a1, a2, epsilon)) {
        continue;
      }

      const auto candidate =
          SegmentSlideDistance(a1, a2, b1, b2, *normalized_direction, epsilon);
      if (!candidate.has_value()) {
        continue;
      }
      if (ignore_negative && *candidate < 0.0 &&
          !NearlyEqual(*candidate, 0.0, epsilon)) {
        continue;
      }
      if (!distance.has_value() || *candidate < *distance) {
        distance = *candidate;
      }
    }
  }

  return distance;
}

std::optional<double> PolygonProjectionDistance(const Contour &stationary,
                                                const Contour &moving,
                                                const PointD &direction,
                                                double epsilon) {
  if (stationary.size() < 2 || moving.empty()) {
    return std::nullopt;
  }

  std::optional<double> distance;
  for (const PointD &point : moving) {
    std::optional<double> min_projection;
    for (size_t i = 0; i < stationary.size(); ++i) {
      const PointD &segment_start = stationary[i];
      const PointD &segment_end = stationary[(i + 1) % stationary.size()];
      if (NearlyEqual(CrossProduct({0.0, 0.0},
                                   {segment_end.x - segment_start.x,
                                    segment_end.y - segment_start.y},
                                   direction),
                      0.0, epsilon)) {
        continue;
      }

      const auto projection = PointDistanceAlongDirection(
          point, segment_start, segment_end, direction, false, epsilon);
      if (projection.has_value() &&
          (!min_projection.has_value() || *projection < *min_projection)) {
        min_projection = projection;
      }
    }

    if (min_projection.has_value() &&
        (!distance.has_value() || *min_projection > *distance)) {
      distance = min_projection;
    }
  }

  return distance;
}

Contour TranslateContour(const Contour &contour, const PointD &translation) {
  Contour translated;
  translated.reserve(contour.size());
  for (const PointD &point : contour) {
    translated.push_back({point.x + translation.x, point.y + translation.y});
  }
  return translated;
}

bool ContoursIntersect(const Contour &a, const Contour &b, double epsilon) {
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
      if (intersection.collinear_overlap) {
        return true;
      }
      if (!intersection.touches_at_endpoint) {
        return true;
      }
    }
  }

  return false;
}

bool IsContourInsideContour(const Contour &inner, const Contour &outer,
                            bool allow_boundary, double epsilon) {
  if (!IsContourValid(inner, epsilon) || !IsContourValid(outer, epsilon)) {
    return false;
  }

  for (const PointD &point : inner) {
    if (!IsPointInContour(point, outer, allow_boundary, epsilon)) {
      return false;
    }
  }

  for (size_t i = 0; i < inner.size(); ++i) {
    const PointD &inner_start = inner[i];
    const PointD &inner_end = inner[(i + 1) % inner.size()];
    for (size_t j = 0; j < outer.size(); ++j) {
      const PointD &outer_start = outer[j];
      const PointD &outer_end = outer[(j + 1) % outer.size()];
      const SegmentIntersection intersection = IntersectSegments(
          inner_start, inner_end, outer_start, outer_end, epsilon);
      if (!intersection.intersects) {
        continue;
      }
      if (intersection.collinear_overlap || intersection.touches_at_endpoint) {
        continue;
      }
      return false;
    }
  }

  return true;
}

void NormalizeOuterWinding(Contour *contour) {
  ReverseIfNeeded(contour, false);
}

void NormalizeHoleWinding(Contour *contour) { ReverseIfNeeded(contour, true); }

PolygonWithHoles NormalizePolygonWinding(PolygonWithHoles polygon) {
  NormalizeOuterWinding(&polygon.outer);
  for (Contour &hole : polygon.holes) {
    NormalizeHoleWinding(&hole);
  }

  return polygon;
}

} // namespace im2d::nesting