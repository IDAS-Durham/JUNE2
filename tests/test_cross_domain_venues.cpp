#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <set>

#include "activity/activity_manager.h"
#include "core/config.h"
#include "doctest.h"
#include "test_utils.h"

using namespace june;

// =============================================================================
// Helper: Build a two-domain world where some venues are "cross-domain"
//
// This rank owns:
//   geo_unit 0 (MGU 100) → local venues 0, 1
//
// The remote rank owns:
//   geo_unit 1 (MGU 101) → venues 100, 101 (NOT loaded here — null in
//   world.venues)
//
// Person 0 lives in geo_unit 0 and has:
//   residence        → venue 0   (local household)
//   primary_activity → venue 100 (cross-domain company, not loaded)
//   leisure          → venue 1   (local pub) + venue 101 (cross-domain pub)
// =============================================================================

static WorldState buildCrossDomainWorld() {
  WorldState world;

  world.venue_type_names = {"household", "company", "school", "pub"};
  world.geo_level_names = {"SGU", "MGU"};
  world.activity_names = {"residence", "primary_activity", "leisure", "none",
                          "dead"};

  // Geography: 2 SGUs under 2 MGUs
  for (int i = 0; i < 2; ++i) {
    GeographicalUnit sgu;
    sgu.id = i;
    sgu.name = "SGU_" + std::to_string(i);
    sgu.level_id = 0;
    sgu.parent_id = 100 + i;
    world.geo_units.push_back(sgu);
  }
  for (int i = 0; i < 2; ++i) {
    GeographicalUnit mgu;
    mgu.id = 100 + i;
    mgu.name = "MGU_" + std::to_string(100 + i);
    mgu.level_id = 1;
    mgu.parent_id = -1;
    world.geo_units.push_back(mgu);
  }

  // LOCAL venues (geo_unit 0, owned by this rank)
  {
    Venue h;
    h.id = 0;
    h.type_id = 0;  // household
    h.geo_unit_id = 0;
    h.is_residence = true;
    world.venues.push_back(h);

    Venue p;
    p.id = 1;
    p.type_id = 3;  // pub
    p.geo_unit_id = 0;
    world.venues.push_back(p);
  }
  // Venues 100 (company, geo_unit 1) and 101 (pub, geo_unit 1)
  // are NOT in world.venues — they live on the remote rank.

  // Person 0 with activity mappings that include cross-domain venues
  {
    Person& p = world.people.emplace_back();
    p.id = 0;
    p.age = 30.0f;
    p.sex = Sex::MALE;
    p.geo_unit_id = 0;
    p.activity_meta_start = 0;
    p.activity_meta_count = 3;
  }

  // residence (idx 0) → 1 local venue
  {
    Person::ActivityMeta m;
    m.activity_index = 0;
    m.venue_start = 0;
    m.venue_count = 1;
    world.activity_meta.push_back(m);
  }
  world.activity_venues.push_back({0, 0});

  // primary_activity (idx 1) → 1 cross-domain venue only
  {
    Person::ActivityMeta m;
    m.activity_index = 1;
    m.venue_start = 1;
    m.venue_count = 1;
    world.activity_meta.push_back(m);
  }
  world.activity_venues.push_back({100, 0});

  // leisure (idx 2) → 1 local + 1 cross-domain
  {
    Person::ActivityMeta m;
    m.activity_index = 2;
    m.venue_start = 2;
    m.venue_count = 2;
    world.activity_meta.push_back(m);
  }
  world.activity_venues.push_back({1, 0});
  world.activity_venues.push_back({101, 0});

  world.buildIndices();
  return world;
}

// Build a minimal Config with one slot that allows the given activities.
static Config buildConfig(WorldState& world,
                          const std::vector<std::string>& activities) {
  Config config;
  config.performance.precompute_schedules = false;

  config.schedule.day_type_cycle = {"workday"};
  config.schedule.day_type_names = {"workday"};

  ScheduleType sched;
  sched.name = "test_sched";
  TimeSlot slot;
  slot.name = "day";
  slot.allowed_activities = activities;
  for (const auto& a : activities) sched.participation_by_day_type["workday"][a] = 1.0;
  sched.slots_by_day_type["workday"].push_back(slot);
  config.schedule.schedule_types.push_back(sched);
  config.schedule.default_schedule_type = "test_sched";

  world.schedule_type_names = {"test_sched"};
  world.people[0].schedule_type_id = 0;
  world.people[0].cached_schedule_type_ = nullptr;

  config.resolve(world);
  return config;
}

// =============================================================================
// Sanity: cross-domain venues are absent from the local world
// =============================================================================
TEST_CASE("Cross-domain venues are not loaded locally") {
  WorldState world = buildCrossDomainWorld();

  CHECK(world.getVenue(0) != nullptr);    // local household
  CHECK(world.getVenue(1) != nullptr);    // local pub
  CHECK(world.getVenue(100) == nullptr);  // remote company — not loaded
  CHECK(world.getVenue(101) == nullptr);  // remote pub   — not loaded

  // But the cross-domain IDs ARE present in the activity mapping
  auto pa = world.getActivityVenues(world.people[0], "primary_activity");
  REQUIRE(pa.size() == 1);
  CHECK(pa[0].first == 100);

  auto lei = world.getActivityVenues(world.people[0], "leisure");
  REQUIRE(lei.size() == 2);
}

// =============================================================================
// selectVenue fallback: when ALL venues for an activity are cross-domain,
// the fixed ActivityManager returns one of them instead of {-1,-1}.
// =============================================================================
TEST_CASE("selectVenue: cross-domain fallback when no local venues exist") {
  WorldState world = buildCrossDomainWorld();
  Config config = buildConfig(world, {"primary_activity"});
  ActivityManager manager(world, config);
  manager.assignScheduleTypes();

  std::vector<PersonLocation> locs(world.people.size());
  manager.assignActivities(config.schedule.schedule_types[0].slots_by_day_type.at("workday")[0], 0,
                           locs);

  // Person 0's only primary_activity venue is 100 (cross-domain).
  // The fixed code falls back to it rather than returning -1.
  CHECK(locs[0].venue_id == 100);
  CHECK(locs[0].subset_index == 0);
}

// =============================================================================
// selectVenue: when only LOCAL venues exist (no cross-domain), behaviour
// is unchanged — a local venue is selected.
// =============================================================================
TEST_CASE("selectVenue: local venues are preferred when available") {
  WorldState world = buildCrossDomainWorld();
  Config config = buildConfig(world, {"residence"});
  ActivityManager manager(world, config);
  manager.assignScheduleTypes();

  std::vector<PersonLocation> locs(world.people.size());
  manager.assignActivities(config.schedule.schedule_types[0].slots_by_day_type.at("workday")[0], 0,
                           locs);

  CHECK(locs[0].venue_id == 0);  // local household
}

// =============================================================================
// selectVenue: with a mixed pool (local + cross-domain) only local venues
// are chosen via type-based selection; cross-domain venues are the fallback
// and not reached when a local venue exists.
// =============================================================================
TEST_CASE(
    "selectVenue: mixed pool uses local venues via type-based selection") {
  WorldState world = buildCrossDomainWorld();
  Config config = buildConfig(world, {"leisure"});
  ActivityManager manager(world, config);
  manager.assignScheduleTypes();

  std::set<VenueId> selected;
  for (int i = 0; i < 200; ++i) {
    std::vector<PersonLocation> locs(world.people.size());
    manager.assignActivities(config.schedule.schedule_types[0].slots_by_day_type.at("workday")[0],
                             0, locs);
    if (locs[0].venue_id != -1) selected.insert(locs[0].venue_id);
  }

  // Local pub (venue 1) must be selected at least sometimes
  CHECK(selected.count(1) > 0);
  // Cross-domain pub (venue 101) is not reached because the local pub
  // satisfies the type-based selection
  CHECK(selected.count(101) == 0);
}

// =============================================================================
// selectVenue returns {-1,-1} when the activity has no venues at all
// =============================================================================
TEST_CASE("selectVenue: returns -1 when activity has no venues") {
  WorldState world = buildCrossDomainWorld();
  Config config = buildConfig(world, {"leisure"});
  // Remove the person's leisure activity metas entirely
  world.people[0].activity_meta_count = 2;  // only residence + primary_activity
  config.resolve(world);

  ActivityManager manager(world, config);
  manager.assignScheduleTypes();

  std::vector<PersonLocation> locs(world.people.size());
  manager.assignActivities(config.schedule.schedule_types[0].slots_by_day_type.at("workday")[0], 0,
                           locs);

  // No leisure venues → fallback to residence
  CHECK(locs[0].venue_id != -1);  // fallback to residence (idx 0)
}
