#pragma once

#include "im2d_nesting_types.h"

#include <optional>
#include <vector>

namespace im2d::nesting {

using NfpContours = std::vector<Contour>;

bool IsAxisAlignedRectangle(const Contour &contour, double epsilon = 1e-9);
std::optional<NfpContours> ComputeOuterNfp(const Contour &stationary,
                                           const Contour &moving,
                                           double epsilon = 1e-9);
std::optional<NfpContours> ComputeRectangleInnerNfp(const Contour &container,
                                                    const Contour &part,
                                                    double epsilon = 1e-9);
std::optional<PointD>
FindInnerNfpStartPoint(const Contour &container, const Contour &part,
                       const NfpContours &existing_nfp = {},
                       double epsilon = 1e-9);
std::vector<PointD> ComputeTouchingCandidateVectors(
    const Contour &stationary, const Contour &moving,
    const PointD &moving_translation, double epsilon = 1e-9);
std::optional<PointD> SelectBestTranslationVector(
    const Contour &stationary, const Contour &moving,
    const PointD &moving_translation,
    const std::optional<PointD> &previous_vector = std::nullopt,
    double epsilon = 1e-9);
std::optional<PointD> ComputeSlideVector(const Contour &stationary,
                                         const Contour &moving,
                                         const PointD &direction,
                                         double epsilon = 1e-9);
std::optional<NfpContours> ComputeInnerNfp(const Contour &container,
                                           const Contour &part,
                                           double epsilon = 1e-9);

} // namespace im2d::nesting