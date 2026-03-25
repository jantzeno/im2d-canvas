#pragma once

#include "im2d_nesting_types.h"

#include <vector>

namespace im2d::nesting {

NestingProblem NormalizeProblemGeometry(NestingProblem problem);
std::vector<Placement> GeneratePlacementCandidates(const Sheet &sheet,
                                                   const Part &part,
                                                   const NestConfig &config,
                                                   double epsilon = 1e-9);
NestingResult ComputeGreedyPlacement(const NestingProblem &problem,
                                     const NestConfig &config,
                                     double epsilon = 1e-9);

} // namespace im2d::nesting