#include <array>
#include <cmath>
#include <fstream>
#include <map>
#include <numeric>
#include <string>
#include <vector>

#include "activity/activity_manager.h"
#include "core/config.h"
#include "core/world_state.h"
#include "doctest.h"
#include "test_utils.h"
#include "utils/random.h"

using namespace june;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string writeCSV(const std::string& content) {
  const std::string path = "/tmp/test_schedule_assignment.csv";
  std::ofstream f(path);
  f << content;
  return path;
}

static ScheduleConfig makeConfig(std::initializer_list<std::string> names) {
  ScheduleConfig cfg;
  for (const auto& name : names) {
    ScheduleType st;
    st.name = name;
    cfg.schedule_types.push_back(st);
  }
  if (names.size() > 0) cfg.default_schedule_type = *names.begin();
  return cfg;
}

// ---------------------------------------------------------------------------
// Phase 1 — unit tests (no WorldState lookups needed beyond minimal world)
// ---------------------------------------------------------------------------

TEST_CASE("ScheduleCSV - single row p=1.0 assigns that schedule") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  ScheduleConfig cfg = makeConfig({"worker", "retired"});

  ScheduleAssignmentRow row;
  // catch-all: no criteria; worker cumulative = 1.0
  row.schedule_probs.push_back({0, 1.0});
  cfg.csv_rows.push_back(row);

  SplitMix64 rng(42);
  for (int i = 0; i < 20; ++i) {
    const ScheduleType* result =
        cfg.tryCSVAssignment(world.people[0], world, rng);
    REQUIRE(result != nullptr);
    CHECK(result->name == "worker");
  }
}

TEST_CASE("ScheduleCSV - probabilistic assignment respects proportions") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  ScheduleConfig cfg = makeConfig({"worker", "retired"});

  ScheduleAssignmentRow row;
  row.schedule_probs.push_back({0, 0.7});  // worker: [0, 0.7)
  row.schedule_probs.push_back({1, 1.0});  // retired: [0.7, 1.0)
  cfg.csv_rows.push_back(row);

  SplitMix64 rng(42);
  int worker_count = 0;
  const int TRIALS = 10000;
  for (int i = 0; i < TRIALS; ++i) {
    const ScheduleType* result =
        cfg.tryCSVAssignment(world.people[0], world, rng);
    if (result && result->name == "worker") ++worker_count;
  }
  double fraction = static_cast<double>(worker_count) / TRIALS;
  CHECK(fraction == doctest::Approx(0.7).epsilon(0.02));
}

TEST_CASE("ScheduleCSV - no matching row returns nullptr") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  world.people[0].age = 30.0f;
  ScheduleConfig cfg = makeConfig({"retired"});

  ScheduleAssignmentRow row;
  SelectionCriterion age_crit;
  age_crit.property_path = "age";
  age_crit.operator_type = ">=";
  age_crit.value = 65;
  row.criteria.push_back(age_crit);
  row.schedule_probs.push_back({0, 1.0});
  cfg.csv_rows.push_back(row);

  SplitMix64 rng(42);
  CHECK(cfg.tryCSVAssignment(world.people[0], world, rng) == nullptr);
}

TEST_CASE("ScheduleCSV - filter.age range matches correctly") {
  WorldState world = TestWorldFactory::createMinimalWorld(2, 1);
  world.people[0].age = 40.0f;  // In 18-65 range
  world.people[1].age = 10.0f;  // Outside range

  ScheduleConfig cfg = makeConfig({"worker"});

  ScheduleAssignmentRow row;
  SelectionCriterion age_min, age_max;
  age_min.property_path = "age";
  age_min.operator_type = ">=";
  age_min.value = 18;
  age_max.property_path = "age";
  age_max.operator_type = "<=";
  age_max.value = 65;
  row.criteria.push_back(age_min);
  row.criteria.push_back(age_max);
  row.schedule_probs.push_back({0, 1.0});
  cfg.csv_rows.push_back(row);

  SplitMix64 rng(42);
  CHECK(cfg.tryCSVAssignment(world.people[0], world, rng) != nullptr);
  CHECK(cfg.tryCSVAssignment(world.people[1], world, rng) == nullptr);
}

TEST_CASE("ScheduleCSV - filter.sex matches correctly") {
  WorldState world = TestWorldFactory::createMinimalWorld(2, 1);
  world.people[0].sex = Sex::MALE;
  world.people[1].sex = Sex::FEMALE;

  ScheduleConfig cfg = makeConfig({"worker"});

  ScheduleAssignmentRow row;
  SelectionCriterion sex_crit;
  sex_crit.property_path = "sex";
  sex_crit.operator_type = "==";
  sex_crit.value = std::string("male");
  row.criteria.push_back(sex_crit);
  row.schedule_probs.push_back({0, 1.0});
  cfg.csv_rows.push_back(row);

  SplitMix64 rng(42);
  CHECK(cfg.tryCSVAssignment(world.people[0], world, rng) !=
        nullptr);  // male → match
  CHECK(cfg.tryCSVAssignment(world.people[1], world, rng) ==
        nullptr);  // female → no match
}

TEST_CASE("ScheduleCSV - catch-all row (no filters) matches every person") {
  WorldState world = TestWorldFactory::createMinimalWorld(3, 1);
  world.people[0].age = 5.0f;
  world.people[1].age = 50.0f;
  world.people[2].age = 90.0f;

  ScheduleConfig cfg = makeConfig({"universal"});

  ScheduleAssignmentRow row;
  // No criteria — matches everyone
  row.schedule_probs.push_back({0, 1.0});
  cfg.csv_rows.push_back(row);

  SplitMix64 rng(42);
  for (auto& person : world.people) {
    const ScheduleType* result = cfg.tryCSVAssignment(person, world, rng);
    REQUIRE(result != nullptr);
    CHECK(result->name == "universal");
  }
}

TEST_CASE("ScheduleCSV - fallback_prob range returns nullptr") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  ScheduleConfig cfg = makeConfig({"worker"});

  // worker=0.6, fallback=0.4
  ScheduleAssignmentRow row;
  row.schedule_probs.push_back({0, 0.6});
  row.fallback_prob = 0.4;
  cfg.csv_rows.push_back(row);

  SplitMix64 rng(42);
  int null_count = 0;
  const int TRIALS = 10000;
  for (int i = 0; i < TRIALS; ++i) {
    const ScheduleType* result =
        cfg.tryCSVAssignment(world.people[0], world, rng);
    if (result == nullptr) ++null_count;
  }
  double null_fraction = static_cast<double>(null_count) / TRIALS;
  CHECK(null_fraction == doctest::Approx(0.4).epsilon(0.02));
}

// ---------------------------------------------------------------------------
// CSV loading tests (write temp files and call resolveCSV)
// ---------------------------------------------------------------------------

TEST_CASE("ScheduleCSV - probabilities within 0.02 of 1.0 are normalized") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  ScheduleConfig cfg = makeConfig({"worker", "retired"});
  cfg.csv_path = writeCSV(
      "filter.age,schedule.worker,schedule.retired\n"
      "18-65,0.699,0.299\n");  // sum = 0.998, within 0.02 → normalize

  cfg.resolveCSV(world);

  REQUIRE(cfg.csv_rows.size() == 1);
  CHECK(cfg.csv_rows[0].fallback_prob == doctest::Approx(0.0));
  // Last cumulative upper bound should be 1.0 after normalization
  REQUIRE(!cfg.csv_rows[0].schedule_probs.empty());
  CHECK(cfg.csv_rows[0].schedule_probs.back().second ==
        doctest::Approx(1.0).epsilon(0.001));
}

TEST_CASE(
    "ScheduleCSV - probabilities more than 0.02 from 1.0 set fallback_prob") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  ScheduleConfig cfg = makeConfig({"worker", "retired"});
  cfg.csv_path = writeCSV(
      "schedule.worker,schedule.retired\n"
      "0.6,0.3\n");  // sum = 0.9, fallback = 0.1

  cfg.resolveCSV(world);

  REQUIRE(cfg.csv_rows.size() == 1);
  CHECK(cfg.csv_rows[0].fallback_prob == doctest::Approx(0.1).epsilon(0.001));
}

TEST_CASE("ScheduleCSV - probabilities > 1.02 throw at load time") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  ScheduleConfig cfg = makeConfig({"worker", "retired"});
  cfg.csv_path = writeCSV(
      "schedule.worker,schedule.retired\n"
      "0.8,0.8\n");  // sum = 1.6 > 1.02

  CHECK_THROWS(cfg.resolveCSV(world));
}

TEST_CASE("ScheduleCSV - a filter the world cannot answer throws at load time") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  ScheduleConfig cfg = makeConfig({"worker", "retired"});
  cfg.csv_path = writeCSV(
      "filter.properties.no_such_property,schedule.worker,schedule.retired\n"
      "x,0.5,0.5\n");

  CHECK_THROWS_AS(cfg.resolveCSV(world), std::runtime_error);
}

TEST_CASE("ScheduleCSV - unknown schedule name throws at load time") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  ScheduleConfig cfg = makeConfig({"worker", "retired"});
  cfg.csv_path = writeCSV(
      "schedule.worker,schedule.nurse\n"  // "nurse" not in schedule_types
      "0.7,0.3\n");

  CHECK_THROWS(cfg.resolveCSV(world));
}

// ---------------------------------------------------------------------------
// Phase 2 — integration tests (with ActivityManager)
// ---------------------------------------------------------------------------

TEST_CASE(
    "ActivityManager - CSV assignment takes priority over YAML "
    "selection_criteria") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  world.schedule_type_names = {"worker", "retired"};

  Config config;
  config.schedule.day_type_cycle = {"workday"};
  config.schedule.day_type_names = {"workday"};

  ScheduleType worker, retired;
  worker.name = "worker";
  retired.name = "retired";
  config.schedule.default_schedule_type = "retired";
  config.schedule.schedule_types.push_back(worker);
  config.schedule.schedule_types.push_back(retired);

  // CSV catch-all: assign "worker" with p=1.0 (index 0)
  ScheduleAssignmentRow csv_row;
  csv_row.schedule_probs.push_back({0, 1.0});
  config.schedule.csv_rows.push_back(csv_row);

  config.resolve(world);
  ActivityManager mgr(world, config);
  mgr.assignScheduleTypes();

  REQUIRE(world.people[0].cached_schedule_type_ != nullptr);
  CHECK(world.people[0].cached_schedule_type_->name == "worker");
}

TEST_CASE("ActivityManager - falls back to YAML when no CSV row matches") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  world.people[0].sex = Sex::MALE;
  world.schedule_type_names = {"worker", "retired"};

  Config config;
  config.schedule.day_type_cycle = {"workday"};
  config.schedule.day_type_names = {"workday"};

  ScheduleType worker, retired;
  worker.name = "worker";
  retired.name = "retired";

  // YAML: worker only applies to female, retired is catch-all default
  SelectionCriterion yaml_female;
  yaml_female.property_path = "sex";
  yaml_female.operator_type = "==";
  yaml_female.value = std::string("female");
  worker.selection_criteria.push_back(yaml_female);

  config.schedule.default_schedule_type = "retired";
  config.schedule.schedule_types.push_back(worker);
  config.schedule.schedule_types.push_back(retired);

  // CSV row: only female → male person won't match CSV either
  ScheduleAssignmentRow csv_row;
  SelectionCriterion female_crit;
  female_crit.property_path = "sex";
  female_crit.operator_type = "==";
  female_crit.value = std::string("female");
  csv_row.criteria.push_back(female_crit);
  csv_row.schedule_probs.push_back({0, 1.0});  // worker
  config.schedule.csv_rows.push_back(csv_row);

  config.resolve(world);
  ActivityManager mgr(world, config);
  mgr.assignScheduleTypes();

  // Male: CSV misses, YAML default ("retired") assigned
  REQUIRE(world.people[0].cached_schedule_type_ != nullptr);
  CHECK(world.people[0].cached_schedule_type_->name == "retired");
}

TEST_CASE(
    "ActivityManager - geo_unit hierarchical filter matches person in "
    "sub-unit") {
  WorldState world;
  world.geo_level_names = {"County", "SGU"};

  GeographicalUnit county;
  county.id = 1;
  county.name = "DURHAM";
  county.level_id = 0;
  county.parent_id = -1;
  world.geo_units.push_back(county);

  GeographicalUnit sgu;
  sgu.id = 2;
  sgu.name = "Durham_SGU";
  sgu.level_id = 1;
  sgu.parent_id = 1;
  world.geo_units.push_back(sgu);

  world.people.emplace_back();
  Person& p = world.people.back();
  p.id = 0;
  p.age = 30.0f;
  p.sex = Sex::MALE;
  p.geo_unit_id = 2;  // Lives in SGU, which is inside DURHAM county
  world.schedule_type_names = {"durham_schedule"};
  world.buildIndices();

  Config config;
  config.schedule.day_type_cycle = {"workday"};
  config.schedule.day_type_names = {"workday"};
  ScheduleType sched;
  sched.name = "durham_schedule";
  config.schedule.schedule_types.push_back(sched);
  config.schedule.default_schedule_type = "durham_schedule";

  // CSV with hierarchical geo filter
  config.schedule.csv_path = writeCSV(
      "geo_level,geo_unit,schedule.durham_schedule\n"
      "County,DURHAM,1.0\n");

  config.resolve(world);
  ActivityManager mgr(world, config);
  mgr.assignScheduleTypes();

  REQUIRE(world.people[0].cached_schedule_type_ != nullptr);
  CHECK(world.people[0].cached_schedule_type_->name == "durham_schedule");
}

TEST_CASE("ActivityManager - fallback_prob range triggers YAML fallback") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  world.schedule_type_names = {"worker", "yaml_sched"};

  Config config;
  config.schedule.day_type_cycle = {"workday"};
  config.schedule.day_type_names = {"workday"};

  ScheduleType worker, yaml_sched;
  worker.name = "worker";
  yaml_sched.name = "yaml_sched";
  // worker has an impossible YAML criterion so YAML never assigns it
  SelectionCriterion impossible;
  impossible.property_path = "age";
  impossible.operator_type = ">=";
  impossible.value = 200;  // no person is 200+ years old
  worker.selection_criteria.push_back(impossible);
  config.schedule.default_schedule_type = "yaml_sched";
  config.schedule.schedule_types.push_back(worker);
  config.schedule.schedule_types.push_back(yaml_sched);

  // CSV row: worker=0.6, fallback=0.4 → 40% of the time YAML is used
  ScheduleAssignmentRow csv_row;
  csv_row.schedule_probs.push_back({0, 0.6});
  csv_row.fallback_prob = 0.4;
  config.schedule.csv_rows.push_back(csv_row);

  config.resolve(world);

  // Run many trials with different seeds to exercise the probability
  // distribution.  The per-entity RNG is seeded from (base_seed, person_id),
  // so we vary the simulation seed across trials.
  int yaml_count = 0;
  const int TRIALS = 1000;
  for (int i = 0; i < TRIALS; ++i) {
    world.people[0].schedule_type_id = 0xFFFF;
    world.people[0].cached_schedule_type_ = nullptr;
    config.simulation.random_seed = static_cast<uint64_t>(i);

    ActivityManager mgr(world, config);
    mgr.assignScheduleTypes();

    if (world.people[0].cached_schedule_type_ &&
        world.people[0].cached_schedule_type_->name == "yaml_sched") {
      ++yaml_count;
    }
  }
  double fraction = static_cast<double>(yaml_count) / TRIALS;
  CHECK(fraction == doctest::Approx(0.4).epsilon(0.05));
}

TEST_CASE(
    "ActivityManager - HDF5-persisted schedule_type_id not overridden by CSV") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  world.schedule_type_names = {"worker", "retired"};
  world.people[0].schedule_type_id = 1;  // "retired" — HDF5-loaded
  world.people[0].cached_schedule_type_ = nullptr;

  Config config;
  config.schedule.day_type_cycle = {"workday"};
  config.schedule.day_type_names = {"workday"};

  ScheduleType worker, retired;
  worker.name = "worker";
  retired.name = "retired";
  config.schedule.default_schedule_type = "retired";
  config.schedule.schedule_types.push_back(worker);
  config.schedule.schedule_types.push_back(retired);

  // CSV catch-all: assigns "worker" — should NOT override HDF5 value
  ScheduleAssignmentRow csv_row;
  csv_row.schedule_probs.push_back({0, 1.0});
  config.schedule.csv_rows.push_back(csv_row);

  config.resolve(world);
  ActivityManager mgr(world, config);
  mgr.assignScheduleTypes();

  // HDF5 wins: retired (index 1) stays
  REQUIRE(world.people[0].cached_schedule_type_ != nullptr);
  CHECK(world.people[0].cached_schedule_type_->name == "retired");
}

// ---------------------------------------------------------------------------
// Properties of the draw itself
// ---------------------------------------------------------------------------

namespace {

// Three schedule types with a catch-all CSV row: a 0.2, b 0.3, c 0.5.
Config threeTypeConfig(uint64_t run_seed) {
  Config config;
  config.simulation.random_seed = static_cast<unsigned int>(run_seed);
  config.schedule.day_type_cycle = {"workday"};
  config.schedule.day_type_names = {"workday"};
  for (const char* name : {"a", "b", "c"}) {
    ScheduleType t;
    t.name = name;
    config.schedule.schedule_types.push_back(t);
  }
  config.schedule.default_schedule_type = "a";
  ScheduleAssignmentRow row;
  row.schedule_probs = {{0, 0.2}, {1, 0.5}, {2, 1.0}};
  config.schedule.csv_rows.push_back(row);
  return config;
}

// Assigns schedule types to the given people and returns type index by id.
std::map<PersonId, int> assignFor(const std::vector<int>& ids,
                                  uint64_t run_seed) {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  world.schedule_type_names = {"a", "b", "c"};
  world.people.clear();
  for (int id : ids) {
    Person& p = world.people.emplace_back();
    p.id = id;
    p.age = 30.0f;
    p.sex = Sex::MALE;
    p.geo_unit_id = 0;
  }
  Config config = threeTypeConfig(run_seed);
  config.resolve(world);
  ActivityManager mgr(world, config);
  mgr.assignScheduleTypes();
  std::map<PersonId, int> out;
  for (const auto& p : world.people) out[p.id] = p.schedule_type_id;
  return out;
}

}  // namespace

TEST_CASE("schedule CSV draw is a fixed function of run seed and person") {
  // Golden values from an independent implementation of mix_seed and
  // SplitMix64; they must hold on every build.
  SplitMix64 r0(mix_seed(777, 0, 0xC5A0ULL));
  CHECK(uniform01(r0) == 0x1.8d98ac27aba60p-3);
  SplitMix64 r5(mix_seed(777, 5, 0xC5A0ULL));
  CHECK(uniform01(r5) == 0x1.74132fb8c93f9p-2);

  std::vector<int> ids(10);
  std::iota(ids.begin(), ids.end(), 0);
  const auto assigned = assignFor(ids, 777);
  const std::vector<int> expected = {0, 2, 0, 1, 2, 1, 2, 2, 0, 1};
  for (int id : ids) CHECK(assigned.at(id) == expected[id]);
}

TEST_CASE("schedule CSV assignment is the same at any rank split and order") {
  const int n = 4000;
  std::vector<int> all(n);
  std::iota(all.begin(), all.end(), 0);
  const auto reference = assignFor(all, 2024);

  // Reverse order, one process.
  std::vector<int> reversed(all.rbegin(), all.rend());
  CHECK(assignFor(reversed, 2024) == reference);

  // Three ranks, each holding the people whose id falls in its residue class,
  // in an order that differs from the reference.
  for (int rank = 0; rank < 3; ++rank) {
    std::vector<int> mine;
    for (int id = n - 1; id >= 0; --id)
      if (id % 3 == rank) mine.push_back(id);
    for (const auto& [id, type] : assignFor(mine, 2024))
      CHECK(type == reference.at(id));
  }

  // The run seed matters.
  CHECK(assignFor(all, 2025) != reference);
}

TEST_CASE("schedule CSV assignment hits the target shares") {
  const int n = 20000;
  std::vector<int> all(n);
  std::iota(all.begin(), all.end(), 0);
  std::array<int, 3> counts{0, 0, 0};
  for (const auto& [id, type] : assignFor(all, 31)) counts.at(type)++;
  const std::array<double, 3> share = {0.2, 0.3, 0.5};
  for (int t = 0; t < 3; ++t) {
    // Four standard deviations of a binomial with n = 20000.
    const double tolerance = 4.0 * std::sqrt(share[t] * (1 - share[t]) / n);
    CHECK(std::abs(static_cast<double>(counts[t]) / n - share[t]) < tolerance);
  }
}
