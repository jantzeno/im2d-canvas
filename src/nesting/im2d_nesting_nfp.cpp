#include "im2d_nesting_nfp.h"

#include "im2d_nesting_geometry.h"

#include <algorithm>
#include <cmath>

namespace im2d::nesting {

namespace {

enum class TouchType {
  VertexVertex,
  MovingVertexOnStationaryEdge,
  StationaryVertexOnMovingEdge,
};

struct TouchingContact {
  TouchType type;
  size_t stationary_index = 0;
  size_t moving_index = 0;
};

bool NearlyEqual(double a, double b, double epsilon) {
  return std::abs(a - b) <= epsilon;
}

bool PointsEqual(const PointD &a, const PointD &b, double epsilon) {
  return NearlyEqual(a.x, b.x, epsilon) && NearlyEqual(a.y, b.y, epsilon);
}

bool AreCollinear(const PointD &a, const PointD &b, const PointD &c,
                  double epsilon) {
  const double cross =
      ((b.x - a.x) * (c.y - a.y)) - ((b.y - a.y) * (c.x - a.x));
  return std::abs(cross) <= epsilon;
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
      if (!intersection.collinear_overlap &&
          !intersection.touches_at_endpoint) {
        return true;
      }
    }
  }

  return false;
}

bool ContoursOverlapOrContain(const Contour &stationary, const Contour &moving,
                              double epsilon) {
  if (ContoursStrictlyIntersect(stationary, moving, epsilon)) {
    return true;
  }

  for (const PointD &point : moving) {
    if (IsPointInContour(point, stationary, false, epsilon)) {
      return true;
    }
  }
  for (const PointD &point : stationary) {
    if (IsPointInContour(point, moving, false, epsilon)) {
      return true;
    }
  }

  return false;
}

bool StartPointExistsInNfp(const PointD &point, const NfpContours &nfp,
                           double epsilon) {
  for (const Contour &contour : nfp) {
    if (contour.empty()) {
      continue;
    }

    if (contour.size() == 1) {
      if (PointsEqual(point, contour.front(), epsilon)) {
        return true;
      }
      continue;
    }

    if (contour.size() >= 3 &&
        IsPointInContour(point, contour, true, epsilon)) {
      return true;
    }

    for (size_t index = 0; index < contour.size(); ++index) {
      const PointD &start = contour[index];
      const PointD &end = contour[(index + 1) % contour.size()];
      if (IsPointOnSegment(point, start, end, epsilon)) {
        return true;
      }
    }
  }
  return false;
}

bool ShouldRejectReverseVector(const PointD &candidate,
                               const std::optional<PointD> &previous_vector,
                               double epsilon) {
  if (!previous_vector.has_value()) {
    return false;
  }

  if (DotProduct(candidate, *previous_vector) >= 0.0) {
    return false;
  }

  const auto candidate_unit = NormalizeVector(candidate, epsilon);
  const auto previous_unit = NormalizeVector(*previous_vector, epsilon);
  if (!candidate_unit.has_value() || !previous_unit.has_value()) {
    return false;
  }

  const double cross = (candidate_unit->y * previous_unit->x) -
                       (candidate_unit->x * previous_unit->y);
  return std::abs(cross) < 1e-4;
}

bool IsLoopPoint(const PointD &candidate, const Contour &nfp, double epsilon) {
  for (const PointD &point : nfp) {
    if (PointsEqual(candidate, point, epsilon)) {
      return true;
    }
  }
  return false;
}

void AppendTracePoint(Contour *contour, const PointD &point, double epsilon) {
  if (contour == nullptr) {
    return;
  }
  if (contour->empty()) {
    contour->push_back(point);
    return;
  }
  if (PointsEqual(contour->back(), point, epsilon)) {
    return;
  }
  if (contour->size() >= 2) {
    const PointD &previous = (*contour)[contour->size() - 2];
    const PointD &last = contour->back();
    if (AreCollinear(previous, last, point, epsilon)) {
      const PointD previous_to_last{last.x - previous.x, last.y - previous.y};
      const PointD last_to_point{point.x - last.x, point.y - last.y};
      if (DotProduct(previous_to_last, last_to_point) >= -epsilon) {
        contour->back() = point;
        return;
      }
    }
  }

  contour->push_back(point);
}

Contour SimplifyContour(Contour contour, double epsilon) {
  if (contour.size() < 3) {
    return contour;
  }

  for (size_t index = 0; index < contour.size(); ++index) {
    const size_t prev_index = (index + contour.size() - 1) % contour.size();
    const size_t next_index = (index + 1) % contour.size();
    if (AreCollinear(contour[prev_index], contour[index], contour[next_index],
                     epsilon)) {
      continue;
    }

    std::rotate(contour.begin(),
                contour.begin() + static_cast<std::ptrdiff_t>(index),
                contour.end());
    break;
  }

  bool removed = true;
  while (removed && contour.size() >= 3) {
    removed = false;
    for (size_t index = 0; index < contour.size(); ++index) {
      const size_t prev_index = (index + contour.size() - 1) % contour.size();
      const size_t next_index = (index + 1) % contour.size();
      const PointD &previous = contour[prev_index];
      const PointD &current = contour[index];
      const PointD &next = contour[next_index];

      if (PointsEqual(previous, next, epsilon)) {
        continue;
      }
      const bool current_between =
          IsPointOnSegment(current, previous, next, epsilon);
      const bool next_between =
          IsPointOnSegment(next, previous, current, epsilon);
      if (!current_between && !next_between) {
        continue;
      }

      contour.erase(contour.begin() + static_cast<std::ptrdiff_t>(index));
      removed = true;
      break;
    }
  }

  return contour;
}

Contour CanonicalizeContour(Contour contour, double epsilon) {
  contour = SimplifyContour(std::move(contour), epsilon);
  if (contour.size() < 3) {
    return contour;
  }

  if (IsClockwise(contour)) {
    std::reverse(contour.begin(), contour.end());
  }

  size_t first_index = 0;
  for (size_t index = 1; index < contour.size(); ++index) {
    const PointD &candidate = contour[index];
    const PointD &current = contour[first_index];
    if (candidate.x < current.x - epsilon ||
        (NearlyEqual(candidate.x, current.x, epsilon) &&
         candidate.y < current.y - epsilon)) {
      first_index = index;
    }
  }

  std::rotate(contour.begin(),
              contour.begin() + static_cast<std::ptrdiff_t>(first_index),
              contour.end());
  return contour;
}

PointD ComputeOuterStartPoint(const Contour &stationary,
                              const Contour &moving) {
  size_t min_stationary_index = 0;
  double min_stationary_y = stationary.front().y;
  for (size_t index = 1; index < stationary.size(); ++index) {
    if (stationary[index].y < min_stationary_y) {
      min_stationary_y = stationary[index].y;
      min_stationary_index = index;
    }
  }

  size_t max_moving_index = 0;
  double max_moving_y = moving.front().y;
  for (size_t index = 1; index < moving.size(); ++index) {
    if (moving[index].y > max_moving_y) {
      max_moving_y = moving[index].y;
      max_moving_index = index;
    }
  }

  return PointD{stationary[min_stationary_index].x - moving[max_moving_index].x,
                stationary[min_stationary_index].y -
                    moving[max_moving_index].y};
}

std::optional<PointD>
SelectBestTraceStep(const Contour &container, const Contour &part,
                    const PointD &current_translation,
                    const std::optional<PointD> &previous_vector,
                    double epsilon) {
  const Contour translated_part = TranslateContour(part, current_translation);
  const auto candidates = ComputeTouchingCandidateVectors(
      container, part, current_translation, epsilon);

  std::optional<PointD> best_step;
  double best_length_squared = 0.0;
  for (const PointD &candidate : candidates) {
    if (ShouldRejectReverseVector(candidate, previous_vector, epsilon)) {
      continue;
    }

    PointD selected = candidate;
    if (const auto slide =
            ComputeSlideVector(container, translated_part, candidate, epsilon);
        slide.has_value()) {
      selected = *slide;
    }

    const double candidate_length_squared = LengthSquared(selected);
    if (candidate_length_squared <= epsilon * epsilon) {
      continue;
    }

    const PointD next_translation{current_translation.x + selected.x,
                                  current_translation.y + selected.y};
    const Contour next_part = TranslateContour(part, next_translation);
    if (!IsContourInsideContour(next_part, container, true, epsilon)) {
      continue;
    }

    if (!best_step.has_value() ||
        candidate_length_squared > best_length_squared) {
      best_step = selected;
      best_length_squared = candidate_length_squared;
    }
  }

  return best_step;
}

std::optional<PointD>
SelectBestOuterTraceStep(const Contour &stationary, const Contour &moving,
                         const PointD &current_translation,
                         const std::optional<PointD> &previous_vector,
                         double epsilon) {
  const Contour translated_moving =
      TranslateContour(moving, current_translation);
  const auto candidates = ComputeTouchingCandidateVectors(
      stationary, moving, current_translation, epsilon);

  std::optional<PointD> best_step;
  double best_length_squared = 0.0;
  for (const PointD &candidate : candidates) {
    if (ShouldRejectReverseVector(candidate, previous_vector, epsilon)) {
      continue;
    }

    PointD selected = candidate;
    if (const auto slide = ComputeSlideVector(stationary, translated_moving,
                                              candidate, epsilon);
        slide.has_value()) {
      selected = *slide;
    }

    const double candidate_length_squared = LengthSquared(selected);
    if (candidate_length_squared <= epsilon * epsilon) {
      continue;
    }

    const PointD next_translation{current_translation.x + selected.x,
                                  current_translation.y + selected.y};
    const Contour next_moving = TranslateContour(moving, next_translation);
    if (ContoursOverlapOrContain(stationary, next_moving, epsilon)) {
      continue;
    }

    if (!best_step.has_value() ||
        candidate_length_squared > best_length_squared) {
      best_step = selected;
      best_length_squared = candidate_length_squared;
    }
  }

  return best_step;
}

std::optional<Contour> TraceInnerNfpContour(const Contour &container,
                                            const Contour &part,
                                            const PointD &start_point,
                                            double epsilon) {
  PointD translation = start_point;
  Contour nfp;
  nfp.push_back(
      {part.front().x + translation.x, part.front().y + translation.y});

  std::optional<PointD> previous_vector;
  const size_t max_iterations = 10 * (container.size() + part.size());

  for (size_t iteration = 0; iteration < max_iterations; ++iteration) {
    const auto step = SelectBestTraceStep(container, part, translation,
                                          previous_vector, epsilon);
    if (!step.has_value() || LengthSquared(*step) <= epsilon * epsilon) {
      return std::nullopt;
    }

    const PointD next_translation{translation.x + step->x,
                                  translation.y + step->y};
    const Contour translated_part = TranslateContour(part, next_translation);
    if (!IsContourInsideContour(translated_part, container, true, epsilon)) {
      return std::nullopt;
    }

    const PointD next_reference{part.front().x + next_translation.x,
                                part.front().y + next_translation.y};

    previous_vector = step;
    translation = next_translation;

    if (PointsEqual(next_reference, nfp.front(), epsilon) ||
        IsLoopPoint(next_reference, nfp, epsilon)) {
      Contour simplified = CanonicalizeContour(nfp, epsilon);
      if (simplified.size() < 3) {
        return std::nullopt;
      }
      return simplified;
    }

    AppendTracePoint(&nfp, next_reference, epsilon);
  }

  return std::nullopt;
}

std::optional<Contour> TraceOuterNfpContour(const Contour &stationary,
                                            const Contour &moving,
                                            const PointD &start_point,
                                            double epsilon) {
  const Contour initial_moving = TranslateContour(moving, start_point);
  if (ContoursOverlapOrContain(stationary, initial_moving, epsilon)) {
    return std::nullopt;
  }

  PointD translation = start_point;
  Contour nfp;
  nfp.push_back(
      {moving.front().x + translation.x, moving.front().y + translation.y});

  std::optional<PointD> previous_vector;
  const size_t max_iterations = 10 * (stationary.size() + moving.size());

  for (size_t iteration = 0; iteration < max_iterations; ++iteration) {
    const auto step = SelectBestOuterTraceStep(stationary, moving, translation,
                                               previous_vector, epsilon);
    if (!step.has_value() || LengthSquared(*step) <= epsilon * epsilon) {
      return std::nullopt;
    }

    const PointD next_translation{translation.x + step->x,
                                  translation.y + step->y};
    const Contour translated_moving =
        TranslateContour(moving, next_translation);
    if (ContoursOverlapOrContain(stationary, translated_moving, epsilon)) {
      return std::nullopt;
    }

    const PointD next_reference{moving.front().x + next_translation.x,
                                moving.front().y + next_translation.y};
    previous_vector = step;
    translation = next_translation;

    if (PointsEqual(next_reference, nfp.front(), epsilon) ||
        IsLoopPoint(next_reference, nfp, epsilon)) {
      Contour simplified = CanonicalizeContour(nfp, epsilon);
      if (simplified.size() < 3) {
        return std::nullopt;
      }
      return simplified;
    }

    AppendTracePoint(&nfp, next_reference, epsilon);
  }

  return std::nullopt;
}

} // namespace

bool IsAxisAlignedRectangle(const Contour &contour, double epsilon) {
  if (!IsContourValid(contour, epsilon) || contour.size() != 4) {
    return false;
  }

  const BoundsD bounds = ComputeBounds(contour);
  if (!bounds.valid) {
    return false;
  }

  for (const PointD &point : contour) {
    const bool on_min_or_max_x = NearlyEqual(point.x, bounds.min.x, epsilon) ||
                                 NearlyEqual(point.x, bounds.max.x, epsilon);
    const bool on_min_or_max_y = NearlyEqual(point.y, bounds.min.y, epsilon) ||
                                 NearlyEqual(point.y, bounds.max.y, epsilon);
    if (!on_min_or_max_x || !on_min_or_max_y) {
      return false;
    }
  }

  return true;
}

std::optional<NfpContours> ComputeOuterNfp(const Contour &stationary,
                                           const Contour &moving,
                                           double epsilon) {
  if (!IsContourValid(stationary, epsilon) ||
      !IsContourValid(moving, epsilon)) {
    return std::nullopt;
  }

  const PointD start_point = ComputeOuterStartPoint(stationary, moving);
  const auto contour =
      TraceOuterNfpContour(stationary, moving, start_point, epsilon);
  if (!contour.has_value()) {
    return std::nullopt;
  }

  return NfpContours{*contour};
}

std::optional<NfpContours> ComputeRectangleInnerNfp(const Contour &container,
                                                    const Contour &part,
                                                    double epsilon) {
  if (!IsAxisAlignedRectangle(container, epsilon) ||
      !IsContourValid(part, epsilon)) {
    return std::nullopt;
  }

  const BoundsD container_bounds = ComputeBounds(container);
  const BoundsD part_bounds = ComputeBounds(part);
  if (!container_bounds.valid || !part_bounds.valid) {
    return std::nullopt;
  }

  const double container_width =
      container_bounds.max.x - container_bounds.min.x;
  const double container_height =
      container_bounds.max.y - container_bounds.min.y;
  const double part_width = part_bounds.max.x - part_bounds.min.x;
  const double part_height = part_bounds.max.y - part_bounds.min.y;

  if (part_width > container_width + epsilon ||
      part_height > container_height + epsilon) {
    return std::nullopt;
  }

  const PointD &anchor = part.front();
  Contour rectangle;
  rectangle.push_back({container_bounds.min.x - part_bounds.min.x + anchor.x,
                       container_bounds.min.y - part_bounds.min.y + anchor.y});
  rectangle.push_back({container_bounds.max.x - part_bounds.max.x + anchor.x,
                       container_bounds.min.y - part_bounds.min.y + anchor.y});
  rectangle.push_back({container_bounds.max.x - part_bounds.max.x + anchor.x,
                       container_bounds.max.y - part_bounds.max.y + anchor.y});
  rectangle.push_back({container_bounds.min.x - part_bounds.min.x + anchor.x,
                       container_bounds.max.y - part_bounds.max.y + anchor.y});

  return NfpContours{rectangle};
}

std::optional<PointD> FindInnerNfpStartPoint(const Contour &container,
                                             const Contour &part,
                                             const NfpContours &existing_nfp,
                                             double epsilon) {
  if (!IsContourValid(container, epsilon) || !IsContourValid(part, epsilon)) {
    return std::nullopt;
  }

  for (const PointD &container_point : container) {
    for (const PointD &part_point : part) {
      const PointD candidate{
          container_point.x - part_point.x,
          container_point.y - part_point.y,
      };
      if (StartPointExistsInNfp(candidate, existing_nfp, epsilon)) {
        continue;
      }

      const Contour translated = TranslateContour(part, candidate);
      if (IsContourInsideContour(translated, container, true, epsilon)) {
        return candidate;
      }
    }
  }

  return std::nullopt;
}

std::vector<PointD> ComputeTouchingCandidateVectors(
    const Contour &stationary, const Contour &moving,
    const PointD &moving_translation, double epsilon) {
  std::vector<PointD> vectors;
  if (stationary.empty() || moving.empty()) {
    return vectors;
  }

  const Contour translated_moving =
      TranslateContour(moving, moving_translation);

  std::vector<TouchingContact> touching;
  for (size_t i = 0; i < stationary.size(); ++i) {
    const size_t next_i = (i + 1) % stationary.size();
    for (size_t j = 0; j < moving.size(); ++j) {
      const size_t next_j = (j + 1) % moving.size();
      if (NearlyEqual(stationary[i].x, translated_moving[j].x, epsilon) &&
          NearlyEqual(stationary[i].y, translated_moving[j].y, epsilon)) {
        touching.push_back(TouchingContact{TouchType::VertexVertex, i, j});
      } else if (IsPointOnSegment(translated_moving[j], stationary[i],
                                  stationary[next_i], epsilon)) {
        touching.push_back(TouchingContact{
            TouchType::MovingVertexOnStationaryEdge, next_i, j});
      } else if (IsPointOnSegment(stationary[i], translated_moving[j],
                                  translated_moving[next_j], epsilon)) {
        touching.push_back(TouchingContact{
            TouchType::StationaryVertexOnMovingEdge, i, next_j});
      }
    }
  }

  for (const TouchingContact &contact : touching) {
    const size_t prev_stationary =
        (contact.stationary_index + stationary.size() - 1) % stationary.size();
    const size_t next_stationary =
        (contact.stationary_index + 1) % stationary.size();
    const size_t prev_moving =
        (contact.moving_index + moving.size() - 1) % moving.size();
    const size_t next_moving = (contact.moving_index + 1) % moving.size();

    const PointD &vertex_a = stationary[contact.stationary_index];
    const PointD &prev_a = stationary[prev_stationary];
    const PointD &next_a = stationary[next_stationary];
    const PointD &vertex_b = translated_moving[contact.moving_index];
    const PointD &prev_b = translated_moving[prev_moving];
    const PointD &next_b = translated_moving[next_moving];

    if (contact.type == TouchType::VertexVertex) {
      vectors.push_back({prev_a.x - vertex_a.x, prev_a.y - vertex_a.y});
      vectors.push_back({next_a.x - vertex_a.x, next_a.y - vertex_a.y});
      vectors.push_back({vertex_b.x - prev_b.x, vertex_b.y - prev_b.y});
      vectors.push_back({vertex_b.x - next_b.x, vertex_b.y - next_b.y});
    } else if (contact.type == TouchType::MovingVertexOnStationaryEdge) {
      vectors.push_back({vertex_a.x - vertex_b.x, vertex_a.y - vertex_b.y});
      vectors.push_back({prev_a.x - vertex_b.x, prev_a.y - vertex_b.y});
    } else {
      vectors.push_back({vertex_a.x - vertex_b.x, vertex_a.y - vertex_b.y});
      vectors.push_back({vertex_a.x - prev_b.x, vertex_a.y - prev_b.y});
    }
  }

  std::vector<PointD> unique_vectors;
  for (const PointD &vector : vectors) {
    if (NearlyEqual(vector.x, 0.0, epsilon) &&
        NearlyEqual(vector.y, 0.0, epsilon)) {
      continue;
    }

    bool duplicate = false;
    for (const PointD &existing : unique_vectors) {
      if (NearlyEqual(existing.x, vector.x, epsilon) &&
          NearlyEqual(existing.y, vector.y, epsilon)) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate) {
      unique_vectors.push_back(vector);
    }
  }

  return unique_vectors;
}

std::optional<PointD>
SelectBestTranslationVector(const Contour &stationary, const Contour &moving,
                            const PointD &moving_translation,
                            const std::optional<PointD> &previous_vector,
                            double epsilon) {
  const Contour translated_moving =
      TranslateContour(moving, moving_translation);
  const auto candidates = ComputeTouchingCandidateVectors(
      stationary, moving, moving_translation, epsilon);

  std::optional<PointD> best_vector;
  double best_length_squared = 0.0;

  for (const PointD &candidate : candidates) {
    if (ShouldRejectReverseVector(candidate, previous_vector, epsilon)) {
      continue;
    }

    PointD selected = candidate;
    if (const auto slide = ComputeSlideVector(stationary, translated_moving,
                                              candidate, epsilon);
        slide.has_value()) {
      selected = *slide;
    }

    const double candidate_length_squared = LengthSquared(selected);
    if (candidate_length_squared <= epsilon * epsilon) {
      continue;
    }

    if (!best_vector.has_value() ||
        candidate_length_squared > best_length_squared) {
      best_vector = selected;
      best_length_squared = candidate_length_squared;
    }
  }

  return best_vector;
}

std::optional<PointD> ComputeSlideVector(const Contour &stationary,
                                         const Contour &moving,
                                         const PointD &direction,
                                         double epsilon) {
  const auto slide_distance =
      PolygonSlideDistance(stationary, moving, direction, true, epsilon);
  if (!slide_distance.has_value() ||
      NearlyEqual(*slide_distance, 0.0, epsilon) || *slide_distance <= 0.0) {
    return std::nullopt;
  }

  const double input_length_squared = LengthSquared(direction);
  if ((*slide_distance) * (*slide_distance) < input_length_squared &&
      !NearlyEqual((*slide_distance) * (*slide_distance), input_length_squared,
                   epsilon)) {
    const auto unit_direction = NormalizeVector(direction, epsilon);
    if (!unit_direction.has_value()) {
      return std::nullopt;
    }
    return PointD{unit_direction->x * (*slide_distance),
                  unit_direction->y * (*slide_distance)};
  }

  return direction;
}

std::optional<NfpContours>
ComputeInnerNfp(const Contour &container, const Contour &part, double epsilon) {
  if (!IsContourValid(container, epsilon) || !IsContourValid(part, epsilon)) {
    return std::nullopt;
  }

  if (IsAxisAlignedRectangle(container, epsilon)) {
    return ComputeRectangleInnerNfp(container, part, epsilon);
  }

  NfpContours contours;
  while (true) {
    const auto start_point =
        FindInnerNfpStartPoint(container, part, contours, epsilon);
    if (!start_point.has_value()) {
      break;
    }

    const auto contour =
        TraceInnerNfpContour(container, part, *start_point, epsilon);
    if (!contour.has_value()) {
      if (contours.empty()) {
        return std::nullopt;
      }
      break;
    }

    contours.push_back(*contour);
  }

  if (contours.empty()) {
    return std::nullopt;
  }

  return contours;
}

} // namespace im2d::nesting