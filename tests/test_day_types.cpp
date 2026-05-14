#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "activity/activity_manager.h"
#include "core/config.h"
#include "doctest.h"
#include "test_utils.h"

using namespace june;

// Helper: build a minimal ScheduleConfig and call resolve() to populate
// cycle_to_type_idx.
static ScheduleConfig makeConfig(std::vector<std::string> cycle,
                                 std::vector<std::string> names) {
  WorldState world;
  world.activity_names = {"work", "leisure", "residence", "none", "dead"};
  ScheduleConfig cfg;
  cfg.day_type_cycle = std::move(cycle);
  cfg.day_type_names = std::move(names);
  cfg.resolve(world);
  return cfg;
}

// ============================================================
// Tests 1-3: getDayTypeIndex cycling behaviour
// ============================================================

TEST_CASE("ScheduleConfig - getDayTypeIndex cycles with 2 unique types") {
  // Cycle: workday, workday, rest_day  (length 3, 2 unique types)
  auto cfg = makeConfig({"workday", "workday", "rest_day"},
                        {"workday", "rest_day"});

  CHECK(cfg.getDayTypeIndex(0) == 0);  // cycle pos 0 → workday
  CHECK(cfg.getDayTypeIndex(1) == 0);  // cycle pos 1 → workday
  CHECK(cfg.getDayTypeIndex(2) == 1);  // cycle pos 2 → rest_day
  CHECK(cfg.getDayTypeIndex(3) == 0);  // wraps: pos 0 → workday
  CHECK(cfg.getDayTypeIndex(4) == 0);  // pos 1 → workday
  CHECK(cfg.getDayTypeIndex(5) == 1);  // pos 2 → rest_day
}

TEST_CASE("ScheduleConfig - getDayTypeIndex with 3 distinct day types") {
  // Each position in the 3-day cycle maps to a unique type.
  auto cfg = makeConfig({"monday", "tuesday", "wednesday"},
                        {"monday", "tuesday", "wednesday"});

  CHECK(cfg.getDayTypeIndex(0) == 0);  // monday
  CHECK(cfg.getDayTypeIndex(1) == 1);  // tuesday
  CHECK(cfg.getDayTypeIndex(2) == 2);  // wednesday
  CHECK(cfg.getDayTypeIndex(3) == 0);  // wraps: monday
  CHECK(cfg.getDayTypeIndex(6) == 0);  // 6 % 3 == 0: monday
  CHECK(cfg.getDayTypeIndex(8) == 2);  // 8 % 3 == 2: wednesday
}

TEST_CASE("ScheduleConfig - getDayTypeIndex with 4 distinct day types") {
  // 4-shift rotation: A, B, C, rest.
  auto cfg = makeConfig({"shift_a", "shift_b", "shift_c", "rest"},
                        {"shift_a", "shift_b", "shift_c", "rest"});

  CHECK(cfg.getDayTypeIndex(0) == 0);  // shift_a
  CHECK(cfg.getDayTypeIndex(1) == 1);  // shift_b
  CHECK(cfg.getDayTypeIndex(2) == 2);  // shift_c
  CHECK(cfg.getDayTypeIndex(3) == 3);  // rest
  CHECK(cfg.getDayTypeIndex(4) == 0);  // wraps: shift_a
  CHECK(cfg.getDayTypeIndex(7) == 3);  // 7 % 4 == 3: rest
  CHECK(cfg.getDayTypeIndex(9) == 1);  // 9 % 4 == 1: shift_b
}

// ============================================================
// Tests 4-6: ActivityManager integration
// ============================================================

// Set up a Config with 2 day types and the given schedule types, resolving
// only schedule and performance sub-configs (avoids checkConfigConsistency).
static Config makeActivityConfig(WorldState& world,
                                 std::vector<ScheduleType> sched_types,
                                 std::string default_type) {
  Config config;
  config.schedule.day_type_cycle = {"workday", "rest_day"};
  config.schedule.day_type_names = {"workday", "rest_day"};
  config.schedule.schedule_types = std::move(sched_types);
  config.schedule.default_schedule_type = std::move(default_type);
  config.performance.precompute_schedules = false;
  config.performance.stochastic_activities = {"work", "leisure", "residence"};
  // Resolve only the sub-configs we need, skipping checkConfigConsistency.
  config.schedule.resolve(world);
  config.performance.resolve(world);
  return config;
}

TEST_CASE("ActivityManager - assignActivitiesFromSchedule uses per-day-type slots") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  world.activity_names = {"work", "leisure", "residence", "none", "dead"};
  // work=0, leisure=1, residence=2, none=3, dead=4

  ScheduleType sched;
  sched.name = "default";
  TimeSlot work_slot, rest_slot;
  work_slot.name = "work";  work_slot.allowed_activities = {"work"};
  rest_slot.name = "rest";  rest_slot.allowed_activities = {"leisure"};
  sched.slots_by_day_type["workday"] = {work_slot};
  sched.slots_by_day_type["rest_day"] = {rest_slot};
  sched.participation_by_day_type["workday"]["work"] = 1.0;
  sched.participation_by_day_type["rest_day"]["leisure"] = 1.0;

  Config config = makeActivityConfig(world, {sched}, "default");
  world.people[0].cached_schedule_type_ = &config.schedule.schedule_types[0];
  world.people[0].schedule_computed = true;

  // Manually install deterministic precomputed entries for each day type.
  // activity_index 0 = work (workday), 1 = leisure (rest_day).
  world.num_day_types = 2;
  world.precomputed_schedules.resize(2);
  world.precomputed_schedules[0].emplace_back(0, -1, -1, true);  // workday
  world.precomputed_schedules[1].emplace_back(1, -1, -1, true);  // rest_day
  // schedule_starts/counts: [person_idx * num_day_types + dt_idx]
  world.schedule_starts.assign(2, 0);
  world.schedule_counts.assign(2, 1);

  ActivityManager manager(world, config);
  std::vector<PersonLocation> locs(1);

  manager.assignActivitiesFromSchedule(0, 0, locs);
  CHECK(locs[0].activity_index == 0);  // workday → work

  manager.assignActivitiesFromSchedule(0, 1, locs);
  CHECK(locs[0].activity_index == 1);  // rest_day → leisure
}

TEST_CASE("ActivityManager - precomputeSchedules populates per-day-type arrays") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  world.activity_names = {"work", "leisure", "residence", "none", "dead"};
  world.schedule_type_names = {"worker"};

  ScheduleType sched;
  sched.name = "worker";
  TimeSlot s1, s2, s3;
  s1.name = "morning";  s1.allowed_activities = {"work"};
  s2.name = "evening";  s2.allowed_activities = {"leisure"};
  s3.name = "rest";     s3.allowed_activities = {"leisure"};
  sched.slots_by_day_type["workday"] = {s1, s2};  // 2 slots on workday
  sched.slots_by_day_type["rest_day"] = {s3};      // 1 slot on rest_day
  sched.participation_by_day_type["workday"]["work"]    = 1.0;
  sched.participation_by_day_type["workday"]["leisure"] = 1.0;
  sched.participation_by_day_type["rest_day"]["leisure"] = 1.0;

  Config config = makeActivityConfig(world, {sched}, "worker");
  config.performance.precompute_schedules = true;
  world.people[0].cached_schedule_type_ = &config.schedule.schedule_types[0];

  ActivityManager manager(world, config);
  manager.precomputeSchedules();

  REQUIRE(world.num_day_types == 2);
  REQUIRE((int)world.precomputed_schedules.size() == 2);

  // One person × 2 workday slots + 1 rest_day slot = 3 entries total.
  CHECK(world.precomputed_schedules[0].size() == 2);  // workday
  CHECK(world.precomputed_schedules[1].size() == 1);  // rest_day

  // schedule_starts/counts for person 0.
  CHECK(world.schedule_starts[0 * 2 + 0] == 0);  // workday starts at 0
  CHECK(world.schedule_counts[0 * 2 + 0] == 2);  // workday: 2 entries
  CHECK(world.schedule_starts[0 * 2 + 1] == 0);  // rest_day starts at 0
  CHECK(world.schedule_counts[0 * 2 + 1] == 1);  // rest_day: 1 entry
}

TEST_CASE("ActivityManager - multiple schedule types store independent slot counts") {
  // Worker:     workday=1 slot, rest_day=1 slot
  // Non-worker: workday=1 slot, rest_day=2 slots
  // Persons are assigned their schedule types via cached_schedule_type_.
  WorldState world = TestWorldFactory::createMinimalWorld(2, 1);
  world.activity_names = {"work", "leisure", "residence", "none", "dead"};
  world.schedule_type_names = {"worker", "non_worker"};

  TimeSlot work_slot, leisure_slot;
  work_slot.name    = "work";    work_slot.allowed_activities    = {"work"};
  leisure_slot.name = "leisure"; leisure_slot.allowed_activities = {"leisure"};

  ScheduleType worker_sched;
  worker_sched.name = "worker";
  worker_sched.slots_by_day_type["workday"]  = {work_slot};
  worker_sched.slots_by_day_type["rest_day"] = {leisure_slot};
  worker_sched.participation_by_day_type["workday"]["work"]     = 1.0;
  worker_sched.participation_by_day_type["rest_day"]["leisure"] = 1.0;

  ScheduleType non_worker_sched;
  non_worker_sched.name = "non_worker";
  non_worker_sched.slots_by_day_type["workday"]  = {leisure_slot};
  non_worker_sched.slots_by_day_type["rest_day"] = {leisure_slot, leisure_slot};
  non_worker_sched.participation_by_day_type["workday"]["leisure"]  = 1.0;
  non_worker_sched.participation_by_day_type["rest_day"]["leisure"] = 1.0;

  Config config = makeActivityConfig(world, {worker_sched, non_worker_sched},
                                     "worker");
  config.performance.precompute_schedules = true;

  // Point each person at their schedule type (index into schedule_types).
  world.people[0].cached_schedule_type_ = &config.schedule.schedule_types[0];
  world.people[1].cached_schedule_type_ = &config.schedule.schedule_types[1];
  world.people[0].schedule_type_id = 0;
  world.people[1].schedule_type_id = 1;

  ActivityManager manager(world, config);
  manager.precomputeSchedules();

  REQUIRE(world.num_day_types == 2);

  // Person 0 (worker): 1 slot per day type.
  CHECK(world.schedule_counts[0 * 2 + 0] == 1);  // workday
  CHECK(world.schedule_counts[0 * 2 + 1] == 1);  // rest_day

  // Person 1 (non_worker): 1 workday slot, 2 rest_day slots.
  CHECK(world.schedule_counts[1 * 2 + 0] == 1);  // workday
  CHECK(world.schedule_counts[1 * 2 + 1] == 2);  // rest_day

  // Flat arrays accumulate in person order:
  // precomputed_schedules[0] (workday): p0 contributes 1, p1 contributes 1 → 2
  // precomputed_schedules[1] (rest_day): p0 contributes 1, p1 contributes 2 → 3
  CHECK(world.precomputed_schedules[0].size() == 2);
  CHECK(world.precomputed_schedules[1].size() == 3);

  // Person 1's rest_day entries start right after person 0's.
  CHECK(world.schedule_starts[1 * 2 + 1] == 1);
}
