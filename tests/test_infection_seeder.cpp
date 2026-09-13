#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "epidemiology/disease.h"
#include "epidemiology/infection_seed.h"
#include "epidemiology/infectiousness_curves.h"

using namespace june;

namespace {

// A one-stage disease, enough for the seeder to construct an Infection.
Disease makeDisease() {
  TransmissionParams transmission;
  transmission.mode = InfectiousnessMode::STAGE_DRIVEN;
  auto constant_curve = std::make_shared<ConstantCurve>(1.0);
  transmission.stage_curves["mild"] = constant_curve;
  transmission.symptom_id_curves = {nullptr, constant_curve};

  std::vector<SymptomTag> symptom_tags = {{"healthy", -1, 0}, {"mild", 1, 1}};
  TrajectoryDefinition trajectory;
  trajectory.selection_key = "general";
  trajectory.severity = 1.0;
  trajectory.stages.push_back({"mild", {"constant", {{"value", 10.0}}}});

  return Disease("TestDisease", symptom_tags, {}, {trajectory}, {},
                 transmission);
}

// Household size by venue id: 4 when h % 17 == 5, 1 when h % 11 == 3, else 2.
int householdSize(int h) {
  if (h % 17 == 5) return 4;
  if (h % 11 == 3) return 1;
  return 2;
}

// One geographical unit "U1" with 70 households, people numbered in household
// order. With a single target group that matches everyone a household scores
// size / sqrt(size), so the pool is three tied blocks: four households at 2,
// fifty-nine at sqrt(2), seven at 1. The middle block is far larger than the
// insertion-sort cutoff of any standard library, so an unstable sort would
// reorder it.
WorldState makeHouseholdWorld() {
  WorldState world;
  world.activity_names = {"residence"};
  world.venue_type_names = {"home"};
  world.geo_level_names = {"MGU"};

  GeographicalUnit unit;
  unit.id = 0;
  unit.name = "U1";
  unit.level_id = 0;
  unit.parent_id = -1;
  world.geo_units.push_back(unit);

  PersonId next_id = 0;
  for (int h = 0; h < 70; ++h) {
    Venue home;
    home.id = h;
    home.type_id = 0;
    home.geo_unit_id = 0;
    world.venues.push_back(home);

    for (int m = 0; m < householdSize(h); ++m) {
      Person& person = world.people.emplace_back();
      person.id = next_id++;
      person.age = 30;
      person.geo_unit_id = 0;
      person.activity_meta_start =
          static_cast<uint32_t>(world.activity_meta.size());
      person.activity_meta_count = 1;
      world.activity_meta.push_back(
          {0, static_cast<uint32_t>(world.activity_venues.size()), 1});
      world.activity_venues.push_back({h, 0});
    }
  }

  world.buildIndices();
  return world;
}

InfectionSeedConfig clusteredConfig(int cases) {
  InfectionSeedEvent seed;
  seed.name = "ties";
  seed.type = InfectionSeedType::CLUSTERED;
  seed.date_time = "2024-01-01 08:00";
  seed.structured_config.geo_level = "MGU";
  seed.structured_config.target_groups = {SeedTargetGroup{}};
  seed.structured_config.unit_cases = {{"U1", {cases}}};

  InfectionSeedConfig config;
  config.seeds.push_back(seed);
  return config;
}

}  // namespace

TEST_CASE(
    "Clustered seeding ranks tied households in their portable shuffled "
    "order") {
  WorldState world = makeHouseholdWorld();
  Disease disease = makeDisease();
  InfectionSeeder seeder(world, &disease, clusteredConfig(26), nullptr, 12345);

  const std::vector<PersonId> infected =
      seeder.seedInfections("2024-01-01 08:00", 0.0);

  // Worked out independently of this code: FNV-1a of "U1" and the run seed
  // key a SplitMix64, Fisher-Yates from the back reorders the households, and
  // a stable ranking by score keeps that order within each tie. All four
  // size-4 households come first, then the first five size-2 households of the
  // shuffle. An unstable sort, or any change to the shuffle, moves these.
  const std::vector<PersonId> expected = {
      113, 114, 115, 116, 44, 45,  46,  47, 78, 79, 80, 81, 9,
      10,  11,  12,  0,   1,  128, 129, 42, 43, 69, 70, 82, 83};
  CHECK(infected == expected);
}
