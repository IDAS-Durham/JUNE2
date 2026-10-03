#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "epidemiology/disease.h"
#include "epidemiology/infection_seed.h"
#include "epidemiology/infectiousness_curves.h"
#include "utils/event_logging/event_logger.h"
#include "utils/time_utils.h"

using namespace june;

namespace {

// The window of the slot starting at `date_time`, as the run's first slot.
SeedWindow windowEndingAt(const std::string& date_time) {
  const long long minutes = parseDateTimeMinutes(date_time);
  return {minutes - 1, minutes};
}

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
// order. With a single target group that matches everyone, a household's
// density is matched^2 / size = size, so the ranking is three tied blocks: four
// households at 4, fifty-nine at 2, seven at 1. Only the household key can
// order a block, and the middle one is far larger than any standard library's
// insertion-sort cutoff, so a comparator that left ties undecided would show.
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
  SeedBudget budget;
  budget.cases = cases;
  budget.eligible_target_groups = {0};
  seed.structured_config.unit_cases = {{"U1", {budget}}};

  InfectionSeedConfig config;
  config.seeds.push_back(seed);
  return config;
}

// A plague-shaped disease with two modes and the given outcome rows.
Disease makePlagueDisease(std::vector<OutcomeRow> rows = {},
                          std::vector<TrajectoryDefinition> trajectories = {}) {
  SymptomTag recovered{.name = "recovered", .value = -3, .id = 0};
  SymptomTag pneumonic{.name = "primary_pneumonic", .value = 2, .id = 1};
  SymptomTag bubonic{.name = "bubonic", .value = 1, .id = 2};
  SymptomTag mild{.name = "mild", .value = 1, .id = 3};

  TransmissionParams transmission;
  TransmissionMode respiratory;
  respiratory.name = "respiratory";
  TransmissionMode rat_flea_bite;
  rat_flea_bite.name = "rat_flea_bite";
  transmission.modes = {respiratory, rat_flea_bite};

  OutcomeRates rates;
  rates.rows = std::move(rows);
  return Disease("Plague", {recovered, pneumonic, bubonic, mild},
                 DiseaseStageSettings{}, trajectories, rates, transmission);
}

// A clustered seed of one case that declares the given Infection Context.
InfectionSeedConfig declaredSeedConfig(const std::string& infector_symptom,
                                       const std::string& transmission_mode) {
  InfectionSeedConfig config = clusteredConfig(1);
  config.seeds[0].infector_symptom = infector_symptom;
  config.seeds[0].transmission_mode = transmission_mode;
  return config;
}

SelectionCriterion contextCriterion(const std::string& fact,
                                    const std::string& value) {
  SelectionCriterion criterion;
  criterion.property_path = fact;
  criterion.operator_type = "==";
  criterion.value = value;
  return criterion;
}

OutcomeRow rowInto(std::vector<SelectionCriterion> criteria,
                   const std::string& selection_key) {
  OutcomeRow row;
  row.criteria = std::move(criteria);
  row.probabilities = {{selection_key, 1.0}};
  return row;
}

// A one-stage trajectory, so the chosen trajectory is visible as its symptom.
TrajectoryDefinition trajectoryInto(const std::string& selection_key,
                                    const std::string& symptom) {
  TrajectoryDefinition trajectory;
  trajectory.selection_key = selection_key;
  TrajectoryStage stage;
  stage.symptom_tag = symptom;
  stage.completion_time.type = "constant";
  stage.completion_time.params = {{"value", 1.0}};
  trajectory.stages = {stage};
  return trajectory;
}

// Rows keyed to the flea mode, to a pneumonic infector, then a default; each
// leads to a trajectory whose one symptom names the row.
Disease makeRoutedPlagueDisease() {
  return makePlagueDisease(
      {rowInto({contextCriterion("transmission_mode", "rat_flea_bite")},
               "flea_route"),
       rowInto({contextCriterion("infector_symptom", "primary_pneumonic")},
               "symptom_route"),
       rowInto({}, "default_route")},
      {trajectoryInto("flea_route", "bubonic"),
       trajectoryInto("symptom_route", "primary_pneumonic"),
       trajectoryInto("default_route", "mild")});
}

// Seeds the one case and returns the symptom its trajectory starts in.
std::string seededStartSymptom(WorldState& world, const Disease& disease,
                               const InfectionSeedConfig& config) {
  InfectionSeeder seeder(world, &disease, config, nullptr, 12345);
  seeder.resolveConfig(world);
  const std::vector<PersonId> infected =
      seeder.seedInfections(windowEndingAt("2024-01-01 08:00"), 0.0);
  REQUIRE(infected.size() == 1);
  const Person& person = world.people[world.person_index.at(infected[0])];
  return disease.getSymptomName(
      person.infection->getTrajectory().transitions.at(0).second);
}

std::string resolveError(InfectionSeeder& seeder, const WorldState& world) {
  try {
    seeder.resolveConfig(world);
  } catch (const std::runtime_error& error) {
    return error.what();
  }
  return "";
}

}  // namespace

TEST_CASE(
    "a seed declaring an unknown symptom or mode is refused at load, naming "
    "the seed and value") {
  WorldState world = makeHouseholdWorld();
  Disease disease = makePlagueDisease();

  SUBCASE("unknown infector symptom") {
    InfectionSeeder seeder(world, &disease,
                           declaredSeedConfig("pneumonik", ""));
    const std::string message = resolveError(seeder, world);
    CHECK(message.find("ties") != std::string::npos);
    CHECK(message.find("pneumonik") != std::string::npos);
  }
  SUBCASE("unknown transmission mode") {
    InfectionSeeder seeder(world, &disease,
                           declaredSeedConfig("", "rat_flee_bite"));
    const std::string message = resolveError(seeder, world);
    CHECK(message.find("ties") != std::string::npos);
    CHECK(message.find("rat_flee_bite") != std::string::npos);
  }
  SUBCASE("known names resolve") {
    InfectionSeeder seeder(world, &disease,
                           declaredSeedConfig("bubonic", "rat_flea_bite"));
    CHECK_NOTHROW(seeder.resolveConfig(world));
  }
}

TEST_CASE(
    "Clustered seeding breaks density ties by the portable household key") {
  WorldState world = makeHouseholdWorld();
  Disease disease = makeDisease();
  InfectionSeeder seeder(world, &disease, clusteredConfig(26), nullptr, 12345);

  const std::vector<PersonId> infected =
      seeder.seedInfections(windowEndingAt("2024-01-01 08:00"), 0.0);

  // Worked out independently of this code: the seed's identity hash and FNV-1a
  // of "U1" key each household through mix_seed, denser households go first,
  // equal density falls back to the key, and members fill in person-id order.
  // All four size-4 households come first, then the first five size-2
  // households by key. A comparator that let ties fall where the sort left
  // them, or any change to the key, moves these.
  const std::vector<PersonId> expected = {
      113, 114, 115, 116, 9,  10, 11, 12, 44, 45, 46, 47, 78,
      79,  80,  81,  34,  35, 99, 100, 76, 77, 67, 68, 120, 121};
  CHECK(infected == expected);
}

TEST_CASE(
    "an undeclared seed is logged with source Seed and no symptom or mode") {
  WorldState world = makeHouseholdWorld();
  Disease disease = makeDisease();
  EventLogger logger;
  InfectionSeeder seeder(world, &disease, clusteredConfig(1), &logger, 12345);

  seeder.seedInfections(windowEndingAt("2024-01-01 08:00"), 0.0);

  const std::vector<InfectionEvent>& infections = logger.getInfectionEvents();
  REQUIRE(infections.size() == 1);
  CHECK(infections[0].source == InfectionSource::Seed);
  CHECK(infections[0].infector_symptom_id == kNoSymptomId);
  CHECK(infections[0].transmission_mode_index == kNoModeIndex);
}

TEST_CASE("a seed's declared context picks the outcome row keyed to it") {
  WorldState world = makeHouseholdWorld();
  Disease disease = makeRoutedPlagueDisease();

  SUBCASE("declared mode") {
    CHECK(seededStartSymptom(world, disease,
                             declaredSeedConfig("", "rat_flea_bite")) ==
          "bubonic");
  }
  SUBCASE("declared symptom only") {
    CHECK(seededStartSymptom(world, disease,
                             declaredSeedConfig("primary_pneumonic", "")) ==
          "primary_pneumonic");
  }
  SUBCASE("nothing declared") {
    CHECK(seededStartSymptom(world, disease, declaredSeedConfig("", "")) ==
          "mild");
  }
}

TEST_CASE("a declared seed logs its declared ids, 255 for any fact left out") {
  WorldState world = makeHouseholdWorld();
  Disease disease = makePlagueDisease();
  EventLogger logger;

  auto loggedInfection = [&](const InfectionSeedConfig& config) {
    InfectionSeeder seeder(world, &disease, config, &logger, 12345);
    seeder.resolveConfig(world);
    seeder.seedInfections(windowEndingAt("2024-01-01 08:00"), 0.0);
    REQUIRE(logger.getInfectionEvents().size() == 1);
    return logger.getInfectionEvents()[0];
  };

  SUBCASE("symptom only") {
    const InfectionEvent logged =
        loggedInfection(declaredSeedConfig("primary_pneumonic", ""));
    CHECK(logged.source == InfectionSource::Seed);
    CHECK(logged.infector_symptom_id == 1);
    CHECK(logged.transmission_mode_index == kNoModeIndex);
  }
  SUBCASE("mode only") {
    const InfectionEvent logged =
        loggedInfection(declaredSeedConfig("", "rat_flea_bite"));
    CHECK(logged.infector_symptom_id == kNoSymptomId);
    CHECK(logged.transmission_mode_index == 1);
  }
}

TEST_CASE(
    "a forced trajectory wins over a declared context, which is still logged") {
  WorldState world = makeHouseholdWorld();
  Disease disease = makeRoutedPlagueDisease();
  EventLogger logger;
  InfectionSeedConfig config = declaredSeedConfig("", "rat_flea_bite");
  config.seeds[0].trajectory_key = "default_route";

  InfectionSeeder seeder(world, &disease, config, &logger, 12345);
  seeder.resolveConfig(world);
  const std::vector<PersonId> infected =
      seeder.seedInfections(windowEndingAt("2024-01-01 08:00"), 0.0);

  REQUIRE(infected.size() == 1);
  const Person& person = world.people[world.person_index.at(infected[0])];
  CHECK(disease.getSymptomName(
            person.infection->getTrajectory().transitions.at(0).second) ==
        "mild");
  REQUIRE(logger.getInfectionEvents().size() == 1);
  CHECK(logger.getInfectionEvents()[0].transmission_mode_index == 1);
}

TEST_CASE("seeds differing only in declared context each fire") {
  WorldState world = makeHouseholdWorld();
  Disease disease = makePlagueDisease();
  EventLogger logger;
  InfectionSeedConfig config = declaredSeedConfig("", "respiratory");
  config.seeds.push_back(declaredSeedConfig("", "rat_flea_bite").seeds[0]);
  for (auto& seed : config.seeds) seed.type = InfectionSeedType::EXACT;

  InfectionSeeder seeder(world, &disease, config, &logger, 12345);
  seeder.resolveConfig(world);
  seeder.seedInfections(windowEndingAt("2024-01-01 08:00"), 0.0);

  const std::vector<InfectionEvent>& infections = logger.getInfectionEvents();
  REQUIRE(infections.size() == 2);
  CHECK(infections[0].transmission_mode_index == 0);
  CHECK(infections[1].transmission_mode_index == 1);
}

TEST_CASE("seeds sharing a name each fire on their own date") {
  WorldState world = makeHouseholdWorld();
  Disease disease = makeDisease();
  InfectionSeedConfig config = clusteredConfig(1);
  config.seeds.push_back(config.seeds[0]);
  config.seeds[1].date_time = "2024-01-08 08:00";

  InfectionSeeder seeder(world, &disease, config, nullptr, 12345);

  CHECK(seeder.seedInfections(windowEndingAt("2024-01-01 08:00"), 0.0).size() == 1);
  CHECK(seeder.seedInfections(windowEndingAt("2024-01-08 08:00"), 7.0).size() == 1);
}

TEST_CASE("seeds sharing a name and date but not a type each fire") {
  WorldState world = makeHouseholdWorld();
  Disease disease = makeDisease();
  InfectionSeedConfig config = clusteredConfig(1);
  config.seeds.push_back(config.seeds[0]);
  config.seeds[1].type = InfectionSeedType::EXACT;

  InfectionSeeder seeder(world, &disease, config, nullptr, 12345);
  const std::vector<PersonId> infected =
      seeder.seedInfections(windowEndingAt("2024-01-01 08:00"), 0.0);

  REQUIRE(infected.size() == 2);
  CHECK(infected[0] != infected[1]);
}

TEST_CASE("seeds with equal identity are refused") {
  // Nothing but position would tell them apart.
  WorldState world = makeHouseholdWorld();
  Disease disease = makeDisease();
  InfectionSeedConfig config = clusteredConfig(1);
  config.seeds.push_back(config.seeds[0]);
  config.seeds[1].structured_config.unit_cases[0].budgets[0].cases = 3;

  CHECK_THROWS_WITH(
      InfectionSeeder(world, &disease, config, nullptr, 12345),
      doctest::Contains("share name 'ties'"));
}

TEST_CASE("seeds differing only in an optional identity field are allowed") {
  WorldState world = makeHouseholdWorld();
  Disease disease = makeDisease();
  InfectionSeedConfig config = clusteredConfig(1);
  config.seeds.push_back(config.seeds[0]);
  config.seeds[1].trajectory_key = "general";

  CHECK_NOTHROW(InfectionSeeder(world, &disease, config, nullptr, 12345));
}

TEST_CASE("a seed with a malformed date is refused, naming seed and date") {
  WorldState world = makeHouseholdWorld();
  Disease disease = makeDisease();
  InfectionSeedConfig config = clusteredConfig(1);
  config.seeds[0].date_time = "2024-01-01 8am";

  CHECK_THROWS_WITH(
      InfectionSeeder(world, &disease, config, nullptr, 12345),
      doctest::Contains("seed 'ties': invalid date '2024-01-01 8am'"));
}

TEST_CASE("a seed dated before 1970 is accepted") {
  WorldState world = makeHouseholdWorld();
  Disease disease = makeDisease();
  InfectionSeedConfig config = clusteredConfig(1);
  config.seeds[0].date_time = "1665-06-01 08:00";

  CHECK_NOTHROW(InfectionSeeder(world, &disease, config, nullptr, 12345));
}

TEST_CASE("a seed on a day its month lacks is refused") {
  WorldState world = makeHouseholdWorld();
  Disease disease = makeDisease();
  InfectionSeedConfig config = clusteredConfig(1);
  config.seeds[0].date_time = "2023-02-29 08:00";

  CHECK_THROWS_WITH(
      InfectionSeeder(world, &disease, config, nullptr, 12345),
      doctest::Contains("invalid date '2023-02-29 08:00'"));
}

TEST_CASE("date-time minutes count across day and year boundaries") {
  CHECK(parseDateTimeMinutes("2024-01-01 00:00") -
            parseDateTimeMinutes("2023-12-31 23:59") ==
        1);
  CHECK(parseDateTimeMinutes("2024-03-01 08:00") -
            parseDateTimeMinutes("2024-02-28 08:00") ==
        2 * 1440);
  CHECK(parseDateTimeMinutes("1970-01-01 00:00") -
            parseDateTimeMinutes("1969-12-31 21:00") ==
        180);
}

TEST_CASE("a slot start fires seeds dated after the previous start, up to it") {
  WorldState world = makeHouseholdWorld();
  Disease disease = makeDisease();
  InfectionSeedConfig config = clusteredConfig(1);
  config.seeds.push_back(config.seeds[0]);
  config.seeds.push_back(config.seeds[0]);
  config.seeds[0].date_time = "2024-01-01 08:00";  // previous start: earlier
  config.seeds[1].date_time = "2024-01-01 10:30";  // inside the slot
  config.seeds[2].date_time = "2024-01-01 12:00";  // this start

  InfectionSeeder seeder(world, &disease, config, nullptr, 12345);
  const SeedWindow slot_at_noon{parseDateTimeMinutes("2024-01-01 08:00"),
                                parseDateTimeMinutes("2024-01-01 12:00")};

  CHECK(seeder.seedInfections(slot_at_noon, 0.0).size() == 2);
}

TEST_CASE("consecutive slot windows fire each seed exactly once") {
  WorldState world = makeHouseholdWorld();
  Disease disease = makeDisease();
  InfectionSeedConfig config = clusteredConfig(1);
  config.seeds.push_back(config.seeds[0]);
  config.seeds.push_back(config.seeds[0]);
  config.seeds[0].date_time = "2024-01-01 08:00";
  config.seeds[1].date_time = "2024-01-01 12:00";
  config.seeds[2].date_time = "2024-01-01 15:30";

  InfectionSeeder seeder(world, &disease, config, nullptr, 12345);
  const std::vector<std::string> slot_starts = {
      "2024-01-01 07:59", "2024-01-01 08:00", "2024-01-01 12:00",
      "2024-01-01 21:00"};
  size_t total_infected = 0;
  for (size_t i = 1; i < slot_starts.size(); ++i) {
    const SeedWindow slot{parseDateTimeMinutes(slot_starts[i - 1]),
                          parseDateTimeMinutes(slot_starts[i])};
    total_infected += seeder.seedInfections(slot, 0.0).size();
  }

  CHECK(total_infected == 3);
}

namespace {

// Seeds a fresh household world on 2024-01-01 and returns who was infected,
// sorted.
std::vector<PersonId> infectedOnFirstDate(const InfectionSeedConfig& config) {
  WorldState world = makeHouseholdWorld();
  Disease disease = makeDisease();
  InfectionSeeder seeder(world, &disease, config, nullptr, 12345);
  std::vector<PersonId> infected =
      seeder.seedInfections(windowEndingAt("2024-01-01 08:00"), 0.0);
  std::sort(infected.begin(), infected.end());
  return infected;
}

}  // namespace

TEST_CASE("a seed infects the same people when a seed is inserted ahead of it") {
  InfectionSeedConfig config = clusteredConfig(3);
  SUBCASE("uniform") {
    config.seeds[0].type = InfectionSeedType::UNIFORM;
    config.seeds[0].uniform_config.cases_per_capita = 0.2;
  }
  SUBCASE("exact") { config.seeds[0].type = InfectionSeedType::EXACT; }
  SUBCASE("clustered") {}

  // Fires on another date, so only the position of the seed under test moves.
  InfectionSeedEvent inserted_seed = config.seeds[0];
  inserted_seed.name = "inserted";
  inserted_seed.date_time = "2024-01-08 08:00";
  InfectionSeedConfig edited_config = config;
  edited_config.seeds.insert(edited_config.seeds.begin(), inserted_seed);

  const std::vector<PersonId> infected = infectedOnFirstDate(config);
  REQUIRE_FALSE(infected.empty());
  CHECK(infectedOnFirstDate(edited_config) == infected);
}

TEST_CASE("an exact seed raised from one case to two keeps its first pick") {
  InfectionSeedConfig one_case_config = clusteredConfig(1);
  one_case_config.seeds[0].type = InfectionSeedType::EXACT;
  InfectionSeedConfig two_case_config = clusteredConfig(2);
  two_case_config.seeds[0].type = InfectionSeedType::EXACT;

  const std::vector<PersonId> one_case = infectedOnFirstDate(one_case_config);
  const std::vector<PersonId> two_cases = infectedOnFirstDate(two_case_config);
  REQUIRE(one_case.size() == 1);
  REQUIRE(two_cases.size() == 2);
  CHECK(std::find(two_cases.begin(), two_cases.end(), one_case[0]) !=
        two_cases.end());
}
