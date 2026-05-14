#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "activity/activity_manager.h"
#include "core/config.h"
#include "doctest.h"
#include "test_utils.h"

using namespace june;

// Helper: pre-resolve a TimeSlot's allowed_activity_indices from world
static void resolveSlotIndices(TimeSlot& slot, const WorldState& world) {
  slot.allowed_activity_indices.clear();
  for (const auto& act : slot.allowed_activities) {
    int idx = world.getActivityIndex(act);
    if (idx >= 0)
      slot.allowed_activity_indices.push_back(static_cast<int16_t>(idx));
  }
}

// =============================================================================
// Cycle 1: hop_on_activity config resolution
// =============================================================================

TEST_CASE("hop_on_activity resolves to schedule index") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  world.activity_names = {"residence", "work", "none", "dead"};

  ScheduleConfig sched_config;
  sched_config.day_type_cycle = {"workday"};
  sched_config.day_type_names = {"workday"};

  ScheduleType regular;
  regular.name = "regular";
  ScheduleType work_sched;
  work_sched.name = "work_schedule";

  TimeSlot slot;
  slot.name = "morning";
  slot.allowed_activities = {"work", "residence"};
  slot.hop_on_activity["work"] = "work_schedule";
  regular.slots_by_day_type["workday"].push_back(slot);

  sched_config.schedule_types.push_back(regular);
  sched_config.schedule_types.push_back(work_sched);

  sched_config.resolveSlots(world);

  const TimeSlot& resolved = sched_config.schedule_types[0].slots_by_day_type["workday"][0];

  CHECK(resolved.hop_schedule_by_activity_idx[1] == 1);  // "work" -> "work_schedule" (idx 1)
  CHECK(resolved.hop_schedule_by_activity_idx[0] == -1); // "residence" -> no hop
}

// =============================================================================
// Cycle 2: activity triggers immediate hop — person assigned from flat_slots[0]
// =============================================================================

TEST_CASE("activity triggers immediate hop to new schedule slot 0") {
  // activity layout: residence=0, work=1, special_work=2, none=3, dead=4
  WorldState world = TestWorldFactory::createMinimalWorld(1, 2);
  world.activity_names = {"residence", "work", "special_work", "none", "dead"};
  world.venue_type_names = {"office", "special_office"};
  world.venues[0].type_id = 0;  // office (work venue)
  world.venues[1].type_id = 1;  // special_office (special_work venue)
  world.buildIndices();

  // Person has work (venue 0) and special_work (venue 1)
  world.people[0].activity_meta_start =
      static_cast<uint32_t>(world.activity_meta.size());
  world.people[0].activity_meta_count = 2;
  world.activity_meta.push_back(
      {1, static_cast<uint32_t>(world.activity_venues.size()), 1});  // work
  world.activity_venues.push_back({0, 0});
  world.activity_meta.push_back(
      {2, static_cast<uint32_t>(world.activity_venues.size()), 1});  // special_work
  world.activity_venues.push_back({1, 0});

  // Schedule 0: "regular" — has a slot that hops to temp_sched on "work"
  ScheduleType regular;
  regular.name = "regular";
  TimeSlot trigger_slot;
  trigger_slot.name = "morning";
  trigger_slot.allowed_activities = {"work"};
  trigger_slot.hop_on_activity["work"] = "temp_sched";
  resolveSlotIndices(trigger_slot, world);
  regular.slots_by_day_type["workday"].push_back(trigger_slot);

  // Schedule 1: "temp_sched" (temporary) — flat_slots[0] has special_work only
  ScheduleType temp_sched;
  temp_sched.name = "temp_sched";
  temp_sched.is_temporary = true;
  TimeSlot temp_slot0;
  temp_slot0.name = "temp_morning";
  temp_slot0.allowed_activities = {"special_work"};
  resolveSlotIndices(temp_slot0, world);
  temp_sched.flat_slots.push_back(temp_slot0);

  Config config;
  config.schedule.day_type_cycle = {"workday"};
  config.schedule.day_type_names = {"workday"};
  config.schedule.schedule_types.push_back(regular);
  config.schedule.schedule_types.push_back(temp_sched);
  config.schedule.default_schedule_type = "regular";
  config.performance.precompute_schedules = false;

  // Resolve so hop_schedule_by_activity_idx is populated
  config.resolve(world);

  // Precomputed: person 0 deterministically assigned "work" (idx 1) at venue 0
  world.num_day_types = 1;
  world.precomputed_schedules.resize(1);
  world.precomputed_schedules[0].push_back(ScheduleEntry(1, 0, 0, true));  // work
  world.schedule_starts.assign(1, 0);
  world.schedule_counts.assign(1, 1);
  world.people[0].schedule_computed = true;
  world.people[0].schedule_type_id = 0;  // "regular"

  world.schedule_type_names = {"regular", "temp_sched"};
  ActivityManager manager(world, config);
  manager.assignScheduleTypes();

  std::vector<PersonLocation> locations(1);
  manager.assignActivitiesFromSchedule(0, 0, locations);

  CHECK(world.people[0].hopped_schedule_id == 1);   // hopped to temp_sched
  CHECK(world.people[0].temp_slot_progress == 1);   // advanced past slot 0
  CHECK(locations[0].activity_index == 2);           // special_work (from flat_slots[0])
}

// =============================================================================
// Cycle 3: non-triggering activity leaves person unhopped
// =============================================================================

TEST_CASE("non-triggering activity does not hop") {
  // Same setup as Cycle 2 but precomputed entry forces "residence" (idx 0)
  WorldState world = TestWorldFactory::createMinimalWorld(1, 2);
  world.activity_names = {"residence", "work", "special_work", "none", "dead"};
  world.venue_type_names = {"home", "office", "special_office"};
  world.venues[0].type_id = 0;
  world.venues[1].type_id = 1;
  world.buildIndices();

  // Person has residence (venue 0) and work (venue 1)
  world.people[0].activity_meta_start =
      static_cast<uint32_t>(world.activity_meta.size());
  world.people[0].activity_meta_count = 2;
  world.activity_meta.push_back(
      {0, static_cast<uint32_t>(world.activity_venues.size()), 1});  // residence
  world.activity_venues.push_back({0, 0});
  world.activity_meta.push_back(
      {1, static_cast<uint32_t>(world.activity_venues.size()), 1});  // work
  world.activity_venues.push_back({1, 0});

  ScheduleType regular;
  regular.name = "regular";
  TimeSlot trigger_slot;
  trigger_slot.name = "morning";
  trigger_slot.allowed_activities = {"work", "residence"};
  trigger_slot.hop_on_activity["work"] = "temp_sched";
  resolveSlotIndices(trigger_slot, world);
  regular.slots_by_day_type["workday"].push_back(trigger_slot);

  ScheduleType temp_sched;
  temp_sched.name = "temp_sched";
  temp_sched.is_temporary = true;
  TimeSlot temp_slot0;
  temp_slot0.name = "temp_morning";
  temp_slot0.allowed_activities = {"special_work"};
  resolveSlotIndices(temp_slot0, world);
  temp_sched.flat_slots.push_back(temp_slot0);

  Config config;
  config.schedule.day_type_cycle = {"workday"};
  config.schedule.day_type_names = {"workday"};
  config.schedule.schedule_types.push_back(regular);
  config.schedule.schedule_types.push_back(temp_sched);
  config.schedule.default_schedule_type = "regular";
  config.performance.precompute_schedules = false;
  config.resolve(world);

  // Precomputed: person assigned "residence" (idx 0) — no hop should trigger
  world.num_day_types = 1;
  world.precomputed_schedules.resize(1);
  world.precomputed_schedules[0].push_back(ScheduleEntry(0, 0, 0, true));  // residence
  world.schedule_starts.assign(1, 0);
  world.schedule_counts.assign(1, 1);
  world.people[0].schedule_computed = true;
  world.people[0].schedule_type_id = 0;
  world.schedule_type_names = {"regular", "temp_sched"};

  ActivityManager manager(world, config);
  manager.assignScheduleTypes();

  std::vector<PersonLocation> locations(1);
  manager.assignActivitiesFromSchedule(0, 0, locations);

  CHECK(world.people[0].hopped_schedule_id == -1);  // no hop
  CHECK(locations[0].activity_index == 0);           // stayed at residence
}

// =============================================================================
// Cycle 4: person on temp schedule advances temp_slot_progress each call
// =============================================================================

TEST_CASE("person on temp schedule advances through flat_slots") {
  // 3-slot temp schedule; 2 calls advance progress without exhausting all slots
  WorldState world = TestWorldFactory::createMinimalWorld(1, 2);
  world.activity_names = {"residence", "task_a", "task_b", "none", "dead"};
  world.venue_type_names = {"home", "venue_a", "venue_b"};
  world.venues[0].type_id = 1;  // venue_a
  world.venues[1].type_id = 2;  // venue_b
  world.buildIndices();

  world.people[0].activity_meta_start =
      static_cast<uint32_t>(world.activity_meta.size());
  world.people[0].activity_meta_count = 2;
  world.activity_meta.push_back(
      {1, static_cast<uint32_t>(world.activity_venues.size()), 1});  // task_a
  world.activity_venues.push_back({0, 0});
  world.activity_meta.push_back(
      {2, static_cast<uint32_t>(world.activity_venues.size()), 1});  // task_b
  world.activity_venues.push_back({1, 0});

  ScheduleType temp_sched;
  temp_sched.name = "temp_sched";
  temp_sched.is_temporary = true;
  TimeSlot slot_a, slot_b, slot_a2;
  slot_a.name = "step_a";
  slot_a.allowed_activities = {"task_a"};
  resolveSlotIndices(slot_a, world);
  slot_b.name = "step_b";
  slot_b.allowed_activities = {"task_b"};
  resolveSlotIndices(slot_b, world);
  slot_a2.name = "step_a2";
  slot_a2.allowed_activities = {"task_a"};
  resolveSlotIndices(slot_a2, world);
  temp_sched.flat_slots.push_back(slot_a);
  temp_sched.flat_slots.push_back(slot_b);
  temp_sched.flat_slots.push_back(slot_a2);  // 3rd slot keeps person hopped after Call 2

  Config config;
  config.schedule.day_type_cycle = {"workday"};
  config.schedule.day_type_names = {"workday"};
  config.schedule.schedule_types.push_back(temp_sched);
  config.schedule.default_schedule_type = "temp_sched";
  config.resolve(world);

  // Manually put the person in a hopped state at progress=0
  world.people[0].hopped_schedule_id = 0;   // temp_sched is index 0
  world.people[0].return_schedule_id = -1;
  world.people[0].temp_slot_progress = 0;
  world.people[0].schedule_computed = true;
  world.people[0].schedule_type_id = 0;
  world.schedule_type_names = {"temp_sched"};
  // Precomputed schedule must exist (but hopped path bypasses it)
  world.num_day_types = 1;
  world.precomputed_schedules.resize(1);
  world.precomputed_schedules[0].push_back(ScheduleEntry(0, -1, -1, false));
  world.schedule_starts.assign(1, 0);
  world.schedule_counts.assign(1, 1);

  ActivityManager manager(world, config);
  manager.assignScheduleTypes();

  std::vector<PersonLocation> locations(1);

  // Call 1: uses flat_slots[0] (task_a)
  manager.assignActivitiesFromSchedule(0, 0, locations);
  CHECK(locations[0].activity_index == 1);                      // task_a
  CHECK(world.people[0].temp_slot_progress == 1);
  CHECK(world.people[0].hopped_schedule_id == 0);               // still hopped

  // Call 2: uses flat_slots[1] (task_b), still before end
  manager.assignActivitiesFromSchedule(0, 0, locations);
  CHECK(locations[0].activity_index == 2);                      // task_b
  CHECK(world.people[0].temp_slot_progress == 2);
  CHECK(world.people[0].hopped_schedule_id == 0);               // still hopped (return happens at boundary)
}

// =============================================================================
// Cycle 5: auto-return to original schedule after last flat_slot
// =============================================================================

TEST_CASE("temp schedule auto-returns to original schedule after last slot") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 2);
  world.activity_names = {"residence", "task_a", "none", "dead"};
  world.venue_type_names = {"home", "venue_a"};
  world.venues[0].type_id = 0;  // home
  world.venues[1].type_id = 1;  // venue_a
  world.buildIndices();

  // Person has residence (venue 0) and task_a (venue 1)
  world.people[0].activity_meta_start =
      static_cast<uint32_t>(world.activity_meta.size());
  world.people[0].activity_meta_count = 2;
  world.activity_meta.push_back(
      {0, static_cast<uint32_t>(world.activity_venues.size()), 1});  // residence
  world.activity_venues.push_back({0, 0});
  world.activity_meta.push_back(
      {1, static_cast<uint32_t>(world.activity_venues.size()), 1});  // task_a
  world.activity_venues.push_back({1, 0});

  // Schedule 0: "regular" (the original)
  ScheduleType regular;
  regular.name = "regular";
  TimeSlot reg_slot;
  reg_slot.name = "day";
  reg_slot.allowed_activities = {"residence"};
  resolveSlotIndices(reg_slot, world);
  regular.slots_by_day_type["workday"].push_back(reg_slot);

  // Schedule 1: "temp_sched" — 1 flat_slot only
  ScheduleType temp_sched;
  temp_sched.name = "temp_sched";
  temp_sched.is_temporary = true;
  TimeSlot temp_slot;
  temp_slot.name = "step_a";
  temp_slot.allowed_activities = {"task_a"};
  resolveSlotIndices(temp_slot, world);
  temp_sched.flat_slots.push_back(temp_slot);

  Config config;
  config.schedule.day_type_cycle = {"workday"};
  config.schedule.day_type_names = {"workday"};
  config.schedule.schedule_types.push_back(regular);
  config.schedule.schedule_types.push_back(temp_sched);
  config.schedule.default_schedule_type = "regular";
  config.resolve(world);

  world.schedule_type_names = {"regular", "temp_sched"};
  world.num_day_types = 1;
  world.precomputed_schedules.resize(1);
  world.precomputed_schedules[0].push_back(ScheduleEntry(0, 0, 0, true));
  world.schedule_starts.assign(1, 0);
  world.schedule_counts.assign(1, 1);
  world.people[0].schedule_computed = true;
  world.people[0].schedule_type_id = 0;  // "regular" is the original

  // Put person in hopped state: on temp_sched (idx 1), 1 slot remaining
  world.people[0].hopped_schedule_id = 1;
  world.people[0].return_schedule_id = -1;  // default: return to original
  world.people[0].temp_slot_progress = 0;

  ActivityManager manager(world, config);
  manager.assignScheduleTypes();

  std::vector<PersonLocation> locations(1);

  // Executing the 1 temp slot triggers return
  manager.assignActivitiesFromSchedule(0, 0, locations);

  CHECK(world.people[0].hopped_schedule_id == -1);                     // returned
  CHECK(world.people[0].temp_slot_progress == 0);                      // reset
  CHECK(world.people[0].cached_schedule_type_->name == "regular");     // back to original
}

// =============================================================================
// Cycle 6: return_schedule sends person to a specified schedule, not the original
// =============================================================================

TEST_CASE("temp schedule returns to specified return_schedule") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 2);
  world.activity_names = {"residence", "task_a", "none", "dead"};
  world.venue_type_names = {"home", "venue_a"};
  world.venues[0].type_id = 0;
  world.venues[1].type_id = 1;
  world.buildIndices();

  world.people[0].activity_meta_start =
      static_cast<uint32_t>(world.activity_meta.size());
  world.people[0].activity_meta_count = 2;
  world.activity_meta.push_back(
      {0, static_cast<uint32_t>(world.activity_venues.size()), 1});  // residence
  world.activity_venues.push_back({0, 0});
  world.activity_meta.push_back(
      {1, static_cast<uint32_t>(world.activity_venues.size()), 1});  // task_a
  world.activity_venues.push_back({1, 0});

  // Schedule 0: "original"
  ScheduleType original;
  original.name = "original";
  TimeSlot orig_slot;
  orig_slot.name = "day";
  orig_slot.allowed_activities = {"residence"};
  resolveSlotIndices(orig_slot, world);
  original.slots_by_day_type["workday"].push_back(orig_slot);

  // Schedule 1: "special_return" — the intended return target
  ScheduleType special_return;
  special_return.name = "special_return";
  TimeSlot sr_slot;
  sr_slot.name = "day";
  sr_slot.allowed_activities = {"residence"};
  resolveSlotIndices(sr_slot, world);
  special_return.slots_by_day_type["workday"].push_back(sr_slot);

  // Schedule 2: "temp_sched" — 1 flat_slot, returns to "special_return"
  ScheduleType temp_sched;
  temp_sched.name = "temp_sched";
  temp_sched.is_temporary = true;
  temp_sched.return_schedule = "special_return";
  TimeSlot temp_slot;
  temp_slot.name = "step";
  temp_slot.allowed_activities = {"task_a"};
  resolveSlotIndices(temp_slot, world);
  temp_sched.flat_slots.push_back(temp_slot);

  Config config;
  config.schedule.day_type_cycle = {"workday"};
  config.schedule.day_type_names = {"workday"};
  config.schedule.schedule_types.push_back(original);
  config.schedule.schedule_types.push_back(special_return);
  config.schedule.schedule_types.push_back(temp_sched);
  config.schedule.default_schedule_type = "original";
  config.resolve(world);

  world.schedule_type_names = {"original", "special_return", "temp_sched"};
  world.num_day_types = 1;
  world.precomputed_schedules.resize(1);
  world.precomputed_schedules[0].push_back(ScheduleEntry(0, 0, 0, true));
  world.schedule_starts.assign(1, 0);
  world.schedule_counts.assign(1, 1);
  world.people[0].schedule_computed = true;
  world.people[0].schedule_type_id = 0;  // "original"

  // Hop to temp_sched; return_schedule_id must be set from return_schedule_idx
  world.people[0].hopped_schedule_id = 2;  // temp_sched
  int16_t return_idx = config.schedule.schedule_types[2].return_schedule_idx;
  world.people[0].return_schedule_id = return_idx;
  world.people[0].temp_slot_progress = 0;

  ActivityManager manager(world, config);
  manager.assignScheduleTypes();

  std::vector<PersonLocation> locations(1);
  manager.assignActivitiesFromSchedule(0, 0, locations);

  REQUIRE(return_idx == 1);  // "special_return" is index 1
  CHECK(world.people[0].hopped_schedule_id == -1);
  CHECK(world.people[0].cached_schedule_type_->name == "special_return");
}
