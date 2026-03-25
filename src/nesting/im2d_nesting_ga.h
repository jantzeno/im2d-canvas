#pragma once

#include "im2d_nesting_types.h"

#include <cstdint>
#include <vector>

namespace im2d::nesting {

struct GenomeGene {
  std::string part_id;
  int instance_index = 0;
  double rotation_degrees = 0.0;
};

struct Genome {
  std::vector<GenomeGene> genes;
};

Genome MakeDefaultGenome(const NestingProblem &problem);
Genome MakeRandomGenome(const NestingProblem &problem, uint32_t seed);
std::vector<Genome> GenerateInitialPopulation(const NestingProblem &problem,
                                              const NestConfig &config,
                                              uint32_t seed);
Genome CrossoverGenomes(const Genome &left, const Genome &right, uint32_t seed);
Genome MutateGenome(const Genome &genome, const NestingProblem &problem,
                    const NestConfig &config, uint32_t seed);
NestingProblem ApplyGenome(const NestingProblem &problem, const Genome &genome);
NestingResult EvaluateGenome(const NestingProblem &problem,
                             const Genome &genome, const NestConfig &config,
                             double epsilon = 1e-9);

} // namespace im2d::nesting