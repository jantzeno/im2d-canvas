#include "../../src/nesting/im2d_nesting_ga.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

namespace {

im2d::nesting::Contour MakeRectangle(double min_x, double min_y, double max_x,
                                     double max_y) {
  return {
      {min_x, min_y},
      {max_x, min_y},
      {max_x, max_y},
      {min_x, max_y},
  };
}

bool GenesEqual(const im2d::nesting::GenomeGene &left,
                const im2d::nesting::GenomeGene &right) {
  return left.part_id == right.part_id &&
         left.instance_index == right.instance_index &&
         left.rotation_degrees == Catch::Approx(right.rotation_degrees);
}

bool GenomesEqual(const im2d::nesting::Genome &left,
                  const im2d::nesting::Genome &right) {
  if (left.genes.size() != right.genes.size()) {
    return false;
  }

  for (size_t index = 0; index < left.genes.size(); ++index) {
    if (!GenesEqual(left.genes[index], right.genes[index])) {
      return false;
    }
  }

  return true;
}

std::vector<std::pair<std::string, int>>
GeneIdentities(const im2d::nesting::Genome &genome) {
  std::vector<std::pair<std::string, int>> identities;
  for (const auto &gene : genome.genes) {
    identities.emplace_back(gene.part_id, gene.instance_index);
  }
  std::sort(identities.begin(), identities.end());
  return identities;
}

im2d::nesting::NestingProblem MakeGaProblem() {
  im2d::nesting::NestingProblem problem;

  im2d::nesting::Sheet sheet;
  sheet.id = "sheet";
  sheet.quantity = 2;
  sheet.geometry.outer = MakeRectangle(0.0, 0.0, 6.0, 4.0);
  problem.sheets = {sheet};

  im2d::nesting::Part alpha;
  alpha.id = "alpha";
  alpha.quantity = 2;
  alpha.geometry.outer = MakeRectangle(0.0, 0.0, 2.0, 2.0);
  alpha.allowed_rotations_degrees = {0.0, 90.0};

  im2d::nesting::Part beta;
  beta.id = "beta";
  beta.quantity = 1;
  beta.geometry.outer = MakeRectangle(0.0, 0.0, 3.0, 1.0);
  beta.allowed_rotations_degrees = {0.0, 180.0};

  problem.parts = {alpha, beta};
  return problem;
}

} // namespace

TEST_CASE("GenerateInitialPopulation is deterministic for a fixed seed",
          "[ga]") {
  const im2d::nesting::NestingProblem problem = MakeGaProblem();

  im2d::nesting::NestConfig config;
  config.population_size = 4;

  const auto first =
      im2d::nesting::GenerateInitialPopulation(problem, config, 12345U);
  const auto second =
      im2d::nesting::GenerateInitialPopulation(problem, config, 12345U);
  const auto different =
      im2d::nesting::GenerateInitialPopulation(problem, config, 12346U);

  REQUIRE(first.size() == 4);
  REQUIRE(second.size() == first.size());
  REQUIRE(different.size() == first.size());

  for (size_t index = 0; index < first.size(); ++index) {
    REQUIRE(GenomesEqual(first[index], second[index]));
  }

  bool saw_difference = false;
  for (size_t index = 0; index < first.size(); ++index) {
    saw_difference =
        saw_difference || !GenomesEqual(first[index], different[index]);
  }
  CHECK(saw_difference);
}

TEST_CASE("CrossoverGenomes is deterministic and preserves gene identities",
          "[ga]") {
  const im2d::nesting::Genome left =
      im2d::nesting::MakeDefaultGenome(MakeGaProblem());
  const im2d::nesting::Genome right =
      im2d::nesting::MakeRandomGenome(MakeGaProblem(), 77U);

  const auto child_a = im2d::nesting::CrossoverGenomes(left, right, 999U);
  const auto child_b = im2d::nesting::CrossoverGenomes(left, right, 999U);

  REQUIRE(GenomesEqual(child_a, child_b));
  REQUIRE(child_a.genes.size() == left.genes.size());
  REQUIRE(GeneIdentities(child_a) == GeneIdentities(left));
}

TEST_CASE("MutateGenome is deterministic and preserves gene identities",
          "[ga]") {
  const im2d::nesting::NestingProblem problem = MakeGaProblem();
  const im2d::nesting::Genome genome =
      im2d::nesting::MakeDefaultGenome(problem);

  im2d::nesting::NestConfig config;
  config.mutation_rate = 100;

  const auto mutated_a =
      im2d::nesting::MutateGenome(genome, problem, config, 31415U);
  const auto mutated_b =
      im2d::nesting::MutateGenome(genome, problem, config, 31415U);

  REQUIRE(GenomesEqual(mutated_a, mutated_b));
  REQUIRE(GeneIdentities(mutated_a) == GeneIdentities(genome));

  bool changed = false;
  for (size_t index = 0; index < genome.genes.size(); ++index) {
    changed =
        changed || !GenesEqual(genome.genes[index], mutated_a.genes[index]);
  }
  CHECK(changed);
}

TEST_CASE("EvaluateGenome applies rotation genes before greedy placement",
          "[ga]") {
  im2d::nesting::NestingProblem problem;

  im2d::nesting::Sheet sheet;
  sheet.id = "sheet";
  sheet.quantity = 1;
  sheet.geometry.outer = MakeRectangle(0.0, 0.0, 2.0, 3.0);
  problem.sheets = {sheet};

  im2d::nesting::Part part;
  part.id = "rotated";
  part.quantity = 1;
  part.geometry.outer = MakeRectangle(0.0, 0.0, 3.0, 2.0);
  part.allowed_rotations_degrees = {0.0, 90.0};
  problem.parts = {part};

  im2d::nesting::Genome blocked = im2d::nesting::MakeDefaultGenome(problem);
  im2d::nesting::Genome fitting = blocked;
  fitting.genes.front().rotation_degrees = 90.0;

  const auto blocked_result = im2d::nesting::EvaluateGenome(
      problem, blocked, im2d::nesting::NestConfig{});
  const auto fitting_result = im2d::nesting::EvaluateGenome(
      problem, fitting, im2d::nesting::NestConfig{});

  REQUIRE(blocked_result.placements.empty());
  REQUIRE(blocked_result.fitness.unplaced_part_count == 1);
  REQUIRE(blocked_result.fitness.score == Catch::Approx(2.0));

  REQUIRE(fitting_result.placements.size() == 1);
  REQUIRE(fitting_result.fitness.unplaced_part_count == 0);
  REQUIRE(fitting_result.fitness.sheet_count == 1);
  REQUIRE(fitting_result.fitness.used_width == Catch::Approx(2.0));
  REQUIRE(fitting_result.fitness.normalized_width_penalty ==
          Catch::Approx(2.0 / 6.0));
  REQUIRE(fitting_result.fitness.score == Catch::Approx(1.0 + (2.0 / 6.0)));
  REQUIRE(fitting_result.fitness.score < blocked_result.fitness.score);
}