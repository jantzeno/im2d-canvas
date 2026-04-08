#pragma once

namespace im2d {

constexpr double kMinimumPreparedContourArea = 0.01;
constexpr float kLineJoinScaleTolerance = 0.05f;
constexpr float kStraightJoinDotThreshold = 0.965f;
constexpr float kTangentJoinDotThreshold = 0.94f;
constexpr float kVectorNormalizationEpsilon = 0.000001f;

} // namespace im2d
