#include "im2d_nesting_ga.h"

#include "im2d_nesting_engine.h"
#include "im2d_nesting_geometry.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <random>
#include <set>
#include <string>
#include <utility>

namespace im2d::nesting {

namespace {

struct ExpandedPartInstance {
  GenomeGene gene;
  Part part;
  std::vector<double> allowed_rotations;
};

bool NearlyEqual(double left, double right, double epsilon = 1e-9) {
  return std::abs(left - right) <= epsilon;
}

std::string MakeGeneKey(const GenomeGene &gene) {
  return gene.part_id + "#" + std::to_string(gene.instance_index);
}

std::vector<double> GetAllowedRotations(const Part &part) {
  if (part.allowed_rotations_degrees.empty()) {
    return {0.0};
  }
  return part.allowed_rotations_degrees;
}

std::vector<ExpandedPartInstance>
ExpandPartInstances(const NestingProblem &problem) {
  std::vector<ExpandedPartInstance> instances;
  std::map<std::string, int> part_instance_counts;

  for (const Part &part : problem.parts) {
    const int quantity = std::max(part.quantity, 0);
    for (int quantity_index = 0; quantity_index < quantity; ++quantity_index) {
      const int instance_index = part_instance_counts[part.id]++;
      ExpandedPartInstance instance;
      instance.gene.part_id = part.id;
      instance.gene.instance_index = instance_index;
      instance.allowed_rotations = GetAllowedRotations(part);
      instance.gene.rotation_degrees = instance.allowed_rotations.front();
      instance.part = part;
      instance.part.quantity = 1;
      instances.push_back(instance);
    }
  }

  return instances;
}

size_t NextIndex(std::mt19937 &rng, size_t upper_bound) {
  if (upper_bound == 0) {
    return 0;
  }
  return static_cast<size_t>(rng() % upper_bound);
}

PointD RotatePoint(const PointD &point, double degrees) {
  const double radians = degrees * std::numbers::pi / 180.0;
  const double cosine = std::cos(radians);
  const double sine = std::sin(radians);

  PointD rotated{.x = (point.x * cosine) - (point.y * sine),
                 .y = (point.x * sine) + (point.y * cosine)};
  if (NearlyEqual(rotated.x, 0.0)) {
    rotated.x = 0.0;
  }
  if (NearlyEqual(rotated.y, 0.0)) {
    rotated.y = 0.0;
  }
  return rotated;
}

Contour RotateContour(const Contour &contour, double degrees) {
  Contour rotated;
  rotated.reserve(contour.size());
  for (const PointD &point : contour) {
    rotated.push_back(RotatePoint(point, degrees));
  }
  return rotated;
}

PolygonWithHoles RotatePolygon(const PolygonWithHoles &polygon,
                               double degrees) {
  PolygonWithHoles rotated;
  rotated.outer = RotateContour(polygon.outer, degrees);
  for (const Contour &hole : polygon.holes) {
    rotated.holes.push_back(RotateContour(hole, degrees));
  }
  return NormalizePolygonWinding(rotated);
}

std::map<std::string, std::vector<double>>
BuildRotationMap(const NestingProblem &problem) {
  std::map<std::string, std::vector<double>> rotations;
  for (const ExpandedPartInstance &instance : ExpandPartInstances(problem)) {
    rotations.emplace(MakeGeneKey(instance.gene), instance.allowed_rotations);
  }
  return rotations;
}

} // namespace

Genome MakeDefaultGenome(const NestingProblem &problem) {
  Genome genome;
  for (const ExpandedPartInstance &instance : ExpandPartInstances(problem)) {
    genome.genes.push_back(instance.gene);
  }
  return genome;
}

Genome MakeRandomGenome(const NestingProblem &problem, uint32_t seed) {
  Genome genome = MakeDefaultGenome(problem);
  std::mt19937 rng(seed);
  const std::map<std::string, std::vector<double>> rotations =
      BuildRotationMap(problem);

  for (size_t index = genome.genes.size(); index > 1; --index) {
    const size_t swap_index = NextIndex(rng, index);
    std::swap(genome.genes[index - 1], genome.genes[swap_index]);
  }

  for (GenomeGene &gene : genome.genes) {
    const auto rotation_it = rotations.find(MakeGeneKey(gene));
    if (rotation_it == rotations.end() || rotation_it->second.empty()) {
      continue;
    }
    gene.rotation_degrees =
        rotation_it->second[NextIndex(rng, rotation_it->second.size())];
  }

  return genome;
}

std::vector<Genome> GenerateInitialPopulation(const NestingProblem &problem,
                                              const NestConfig &config,
                                              uint32_t seed) {
  const int population_size = std::max(config.population_size, 1);
  std::vector<Genome> population;
  population.reserve(static_cast<size_t>(population_size));
  population.push_back(MakeDefaultGenome(problem));

  for (int index = 1; index < population_size; ++index) {
    population.push_back(
        MakeRandomGenome(problem, seed + static_cast<uint32_t>(index)));
  }

  return population;
}

Genome CrossoverGenomes(const Genome &left, const Genome &right,
                        uint32_t seed) {
  Genome child;
  if (left.genes.size() != right.genes.size()) {
    return child;
  }
  if (left.genes.empty()) {
    return left;
  }

  std::mt19937 rng(seed);
  const size_t start = NextIndex(rng, left.genes.size());
  const size_t end = start + NextIndex(rng, left.genes.size() - start);

  child.genes.resize(left.genes.size());
  std::vector<bool> filled(left.genes.size(), false);
  std::set<std::string> used_keys;

  for (size_t index = start; index <= end; ++index) {
    child.genes[index] = left.genes[index];
    filled[index] = true;
    used_keys.insert(MakeGeneKey(left.genes[index]));
  }

  size_t write_index = 0;
  for (const GenomeGene &gene : right.genes) {
    if (used_keys.contains(MakeGeneKey(gene))) {
      continue;
    }
    while (write_index < child.genes.size() && filled[write_index]) {
      ++write_index;
    }
    if (write_index >= child.genes.size()) {
      break;
    }
    child.genes[write_index] = gene;
    filled[write_index] = true;
    ++write_index;
  }

  return child;
}

Genome MutateGenome(const Genome &genome, const NestingProblem &problem,
                    const NestConfig &config, uint32_t seed) {
  Genome mutated = genome;
  if (mutated.genes.empty() || config.mutation_rate <= 0) {
    return mutated;
  }

  std::mt19937 rng(seed);
  const auto rotations = BuildRotationMap(problem);
  const size_t mutation_count = std::max<size_t>(
      1,
      (mutated.genes.size() * static_cast<size_t>(config.mutation_rate) + 99) /
          100);

  for (size_t mutation_index = 0; mutation_index < mutation_count;
       ++mutation_index) {
    const size_t left_index = NextIndex(rng, mutated.genes.size());
    const size_t right_index = NextIndex(rng, mutated.genes.size());
    std::swap(mutated.genes[left_index], mutated.genes[right_index]);

    const size_t rotation_index = NextIndex(rng, mutated.genes.size());
    GenomeGene &gene = mutated.genes[rotation_index];
    const auto rotation_it = rotations.find(MakeGeneKey(gene));
    if (rotation_it == rotations.end() || rotation_it->second.size() <= 1) {
      continue;
    }

    size_t next_rotation_index = NextIndex(rng, rotation_it->second.size() - 1);
    double next_rotation = rotation_it->second[next_rotation_index];
    if (NearlyEqual(next_rotation, gene.rotation_degrees)) {
      next_rotation =
          rotation_it
              ->second[(next_rotation_index + 1) % rotation_it->second.size()];
    }
    gene.rotation_degrees = next_rotation;
  }

  return mutated;
}

NestingProblem ApplyGenome(const NestingProblem &problem,
                           const Genome &genome) {
  NestingProblem reordered;
  reordered.sheets = problem.sheets;

  std::map<std::string, ExpandedPartInstance> instances;
  for (ExpandedPartInstance &instance : ExpandPartInstances(problem)) {
    instances.emplace(MakeGeneKey(instance.gene), std::move(instance));
  }

  std::set<std::string> consumed_keys;
  for (const GenomeGene &gene : genome.genes) {
    const std::string key = MakeGeneKey(gene);
    const auto instance_it = instances.find(key);
    if (instance_it == instances.end() || consumed_keys.contains(key)) {
      continue;
    }

    Part part = instance_it->second.part;
    part.geometry = RotatePolygon(part.geometry, gene.rotation_degrees);
    part.quantity = 1;
    reordered.parts.push_back(part);
    consumed_keys.insert(key);
  }

  for (const auto &[key, instance] : instances) {
    if (consumed_keys.contains(key)) {
      continue;
    }

    Part part = instance.part;
    part.geometry =
        RotatePolygon(part.geometry, instance.gene.rotation_degrees);
    part.quantity = 1;
    reordered.parts.push_back(part);
  }

  return reordered;
}

NestingResult EvaluateGenome(const NestingProblem &problem,
                             const Genome &genome, const NestConfig &config,
                             double epsilon) {
  return ComputeGreedyPlacement(ApplyGenome(problem, genome), config, epsilon);
}

} // namespace im2d::nesting