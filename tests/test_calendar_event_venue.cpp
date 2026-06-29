#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "activity/activity_manager.h"
#include "core/config.h"
#include "core/types.h"
#include "core/world_state.h"
#include "doctest.h"
#include "epidemiology/calendar_event.h"
#include "test_utils.h"

using namespace june;

namespace {

void resolveSlotIndices(TimeSlot& slot, const WorldState& world) {
  slot.allowed_activity_indices.clear();
  for (const auto& act : slot.allowed_activities) {
    int idx = world.getActivityIndex(act);
    if (idx >= 0)
      slot.allowed_activity_indices.push_back(static_cast<int16_t>(idx));
  }
}

WorldState buildCatchmentWorld(int num_people = 1) {
  WorldState world;
  world.geo_level_names = {"sgu"};
  GeographicalUnit gu;
  gu.id = 0; gu.parent_id = -1; gu.level_id = 0;
  world.geo_units.push_back(gu);
  world.venue_type_names = {"fair"};
  Venue v; v.id = 0; v.type_id = 0; v.geo_unit_id = 0;
  world.venues.push_back(v);
  world.activity_names = {"Fair_accommodation"};
  world.schedule_type_names = {"regular", "Fair_day_trip"};
  for (int i = 0; i < num_people; ++i) {
    Person& p = world.people.emplace_back();
    p.id = i; p.geo_unit_id = 0;
  }
  world.buildIndices();
  return world;
}

struct FairHopFixture {
  WorldState world;
  Config config;
  CalendarEvent event;
  int16_t fair_idx = 1;
};

FairHopFixture buildFairHopFixture() {
  FairHopFixture f;
  f.world = TestWorldFactory::createMinimalWorld(1, 3);
  f.world.activity_names = {"residence", "Fair_accommodation", "none", "dead"};
  f.world.venue_type_names = {"home", "guesthouse"};
  f.world.venues[0].type_id = 0;
  f.world.venues[1].type_id = 1;
  f.world.venues[2].type_id = 1;
  f.world.geo_level_names = {"sgu"};
  GeographicalUnit gu; gu.id = 0; gu.parent_id = -1; gu.level_id = 0;
  f.world.geo_units.push_back(gu);
  f.world.people[0].geo_unit_id = 0;
  f.world.buildIndices();

  Person::ActivityMeta meta;
  meta.activity_index = f.fair_idx;
  meta.venue_start = static_cast<uint32_t>(f.world.activity_venues.size());
  meta.venue_count = 2;
  f.world.people[0].activity_meta_start =
      static_cast<uint32_t>(f.world.activity_meta.size());
  f.world.people[0].activity_meta_count = 1;
  f.world.activity_meta.push_back(meta);
  f.world.activity_venues.push_back({1, 0});
  f.world.activity_venues.push_back({2, 0});

  ScheduleType regular; regular.name = "regular";
  ScheduleType fair_temp;
  fair_temp.name = "fair_1day"; fair_temp.is_temporary = true;
  TimeSlot fair_slot; fair_slot.name = "fair_day";
  fair_slot.allowed_activities = {"Fair_accommodation"};
  resolveSlotIndices(fair_slot, f.world);
  fair_temp.flat_slots.push_back(fair_slot);

  f.config.schedule.day_type_cycle = {"day"};
  f.config.schedule.day_type_names = {"day"};
  f.config.schedule.schedule_types.push_back(regular);
  f.config.schedule.schedule_types.push_back(fair_temp);
  f.config.schedule.default_schedule_type = "regular";
  f.config.performance.precompute_schedules = false;
  f.config.resolve(f.world);
  f.world.schedule_type_names = {"regular", "fair_1day"};
  f.world.num_day_types = 1;

  f.event.calendar_event_id = 99;
  f.event.start_day = 0;
  f.event.schedule_type_idx = 1;
  f.event.compliance_rate = 1.0f;
  f.event.catchment_rule_id = 0;
  f.event.candidate_venue_builder = [](const WorldState&) -> std::vector<VenueId> {
    return {1, 2};
  };
  f.event.venue_selector = [](const std::vector<VenueId>& c, PersonId, uint64_t) {
    return std::make_pair(c.back(), SubsetIndex{0});
  };

  f.world.people[0].schedule_hop.hopped_schedule_id = 1;
  f.world.people[0].schedule_hop.temp_slot_progress = 0;
  f.world.people[0].schedule_type_id = 0;
  return f;
}

}  // namespace

// =============================================================================
// Venue resolver basics
// =============================================================================

TEST_CASE("resolver returns {-1,-1} when person has no active event") {
  WorldState world = buildCatchmentWorld();
  CalendarEventManager manager;
  auto result = manager.resolveCalendarEventVenue(world.people[0]);
  CHECK(result.first == -1);
  CHECK(result.second == -1);
}

// =============================================================================
// selectVenue guard drives the assignment through ActivityManager
// =============================================================================

TEST_CASE("selectVenue guard resolves the calendar-event venue during a hop") {
  FairHopFixture f = buildFairHopFixture();
  ActivityManager manager(f.world, f.config);

  CalendarEventManager calendar_manager({{f.event}});
  CalendarEventManager::Snapshot snap;
  snap.active_event[f.world.people[0].id] = 99;
  calendar_manager.restore(std::move(snap), f.world);
  manager.setCalendarEventManager(&calendar_manager);

  std::vector<PersonLocation> locations(1);
  manager.assignActivitiesFromSchedule(0, 0, locations);

  CHECK(locations[0].activity_index == f.fair_idx);
  CHECK(locations[0].venue_id == 2);  // custom selector always picks back() = 2
  CHECK(locations[0].subset_index == 0);
}

TEST_CASE("selectVenue is unaffected when no calendar-event manager is set") {
  FairHopFixture f = buildFairHopFixture();
  ActivityManager manager(f.world, f.config);

  std::vector<PersonLocation> locations(1);
  manager.assignActivitiesFromSchedule(0, 0, locations);

  CHECK(locations[0].activity_index == f.fair_idx);
  CHECK((locations[0].venue_id == 1 || locations[0].venue_id == 2));
}

// =============================================================================
// Dynamic hashed venue resolution for catchment-rule events
// =============================================================================

TEST_CASE("catchment-rule event resolves venue via hash into candidate list") {
  WorldState world;
  world.geo_level_names = {"sgu"};
  GeographicalUnit gu; gu.id = 0; gu.parent_id = -1; gu.level_id = 0;
  world.geo_units.push_back(gu);
  world.venue_type_names = {"guest_house"};
  for (VenueId vid : {20, 21, 22}) {
    Venue v; v.id = vid; v.type_id = 0; v.geo_unit_id = 0;
    world.venues.push_back(v);
  }
  world.activity_names = {"Fair_lodging"};
  world.schedule_type_names = {"regular", "Fair_lodging"};
  for (PersonId pid : {0, 1}) {
    Person& p = world.people.emplace_back();
    p.id = pid; p.geo_unit_id = 0;
  }
  world.buildIndices();

  CalendarEvent event;
  event.calendar_event_id = 5;
  event.start_day = 0;
  event.schedule_type_idx = 1;
  event.compliance_rate = 1.0f;
  event.catchment_rule_id = 0;
  event.candidate_venue_builder = [](const WorldState& w) {
    return w.getVenuesInGeoUnit(0, "guest_house");
  };

  CalendarEventManager manager({{event}});
  manager.triggerEventsForDay(0, world, world.people, 999, {{0, {0}}});
  REQUIRE(manager.stats().triggered == 2);

  auto v0 = manager.resolveCalendarEventVenue(world.people[0]);
  auto v1 = manager.resolveCalendarEventVenue(world.people[1]);
  CHECK((v0.first == 20 || v0.first == 21 || v0.first == 22));
  CHECK(v0.second == 0);
  CHECK((v1.first == 20 || v1.first == 21 || v1.first == 22));
  CHECK(v1.second == 0);

  auto v0_again = manager.resolveCalendarEventVenue(world.people[0]);
  CHECK(v0_again.first == v0.first);
}

TEST_CASE("catchment-rule event with no builder uses manager default pool") {
  WorldState world;
  world.geo_level_names = {"sgu"};
  GeographicalUnit gu; gu.id = 0; gu.parent_id = -1; gu.level_id = 0;
  world.geo_units.push_back(gu);
  world.venue_type_names = {"guest_house"};
  for (VenueId vid : {20, 21, 22}) {
    Venue v; v.id = vid; v.type_id = 0; v.geo_unit_id = 0;
    world.venues.push_back(v);
  }
  world.activity_names = {"Fair_lodging"};
  world.schedule_type_names = {"regular", "Fair_lodging"};
  Person& p = world.people.emplace_back();
  p.id = 0; p.geo_unit_id = 0;
  world.buildIndices();

  CalendarEvent event;
  event.calendar_event_id = 5;
  event.start_day = 0;
  event.schedule_type_idx = 1;
  event.compliance_rate = 1.0f;
  event.catchment_rule_id = 0;
  event.hosting_geo_unit_id = 0;
  event.venue_type_name = "guest_house";
  // No candidate_venue_builder: the manager derives the pool from the struct
  // fields via getVenuesInGeoUnit.

  CalendarEventManager manager({{event}});
  manager.triggerEventsForDay(0, world, world.people, 999, {{0, {0}}});
  REQUIRE(manager.stats().triggered == 1);

  auto v = manager.resolveCalendarEventVenue(world.people[0]);
  CHECK((v.first == 20 || v.first == 21 || v.first == 22));
  CHECK(v.second == 0);
}

TEST_CASE("catchment-rule resolver returns {-1,-1} when candidate list is empty") {
  WorldState world;
  world.geo_level_names = {"sgu"};
  GeographicalUnit gu; gu.id = 0; gu.parent_id = -1; gu.level_id = 0;
  world.geo_units.push_back(gu);
  world.venue_type_names = {"fair"};
  // No venues — builder returns empty list.
  world.activity_names = {"Fair_accommodation"};
  world.schedule_type_names = {"regular", "Fair_day_trip"};
  Person& p = world.people.emplace_back();
  p.id = 0; p.geo_unit_id = 0;
  world.buildIndices();

  CalendarEvent event;
  event.calendar_event_id = 1;
  event.start_day = 0;
  event.schedule_type_idx = 1;
  event.compliance_rate = 1.0f;
  event.catchment_rule_id = 0;
  event.candidate_venue_builder = [](const WorldState& w) {
    return w.getVenuesInGeoUnit(0, "fair");
  };

  CalendarEventManager manager({{event}});
  manager.triggerEventsForDay(0, world, world.people, 42, {{0, {0}}});
  REQUIRE(manager.stats().triggered == 1);

  auto v = manager.resolveCalendarEventVenue(world.people[0]);
  CHECK(v.first == -1);
  CHECK(v.second == -1);
}

TEST_CASE("event with custom builder uses it to populate venue candidates") {
  WorldState world;
  world.geo_level_names = {"sgu"};
  GeographicalUnit gu; gu.id = 0; gu.parent_id = -1; gu.level_id = 0;
  world.geo_units.push_back(gu);
  world.venue_type_names = {"fair"};
  world.activity_names = {"Fair_accommodation"};
  world.schedule_type_names = {"regular", "fair_sched"};
  Person& p = world.people.emplace_back();
  p.id = 0; p.geo_unit_id = 0;
  world.buildIndices();

  const std::vector<VenueId> custom_venues = {100, 101, 102};

  CalendarEvent event;
  event.calendar_event_id = 7;
  event.start_day = 0;
  event.schedule_type_idx = 1;
  event.compliance_rate = 1.0f;
  event.catchment_rule_id = 7;
  event.candidate_venue_builder = [&custom_venues](const WorldState&) {
    return custom_venues;
  };

  CalendarEventManager manager({{event}});
  manager.triggerEventsForDay(0, world, world.people, 42, {{7, {0}}});
  REQUIRE(manager.stats().triggered == 1);

  auto v = manager.resolveCalendarEventVenue(world.people[0]);
  CHECK((v.first == 100 || v.first == 101 || v.first == 102));
  CHECK(v.second == 0);
}

TEST_CASE("event with custom venue_selector uses it instead of hash-select") {
  WorldState world;
  world.geo_level_names = {"sgu"};
  GeographicalUnit gu; gu.id = 0; gu.parent_id = -1; gu.level_id = 0;
  world.geo_units.push_back(gu);
  world.venue_type_names = {"fair"};
  world.activity_names = {"Fair_accommodation"};
  world.schedule_type_names = {"regular", "fair_sched"};
  Person& p = world.people.emplace_back();
  p.id = 0; p.geo_unit_id = 0;
  world.buildIndices();

  const std::vector<VenueId> custom_venues = {100, 101, 102};

  CalendarEvent event;
  event.calendar_event_id = 7;
  event.start_day = 0;
  event.schedule_type_idx = 1;
  event.compliance_rate = 1.0f;
  event.catchment_rule_id = 7;
  event.candidate_venue_builder = [&custom_venues](const WorldState&) {
    return custom_venues;
  };
  event.venue_selector = [](const std::vector<VenueId>& candidates,
                             PersonId, uint64_t) {
    return std::make_pair(candidates.back(), SubsetIndex{0});
  };

  CalendarEventManager manager({{event}});
  manager.triggerEventsForDay(0, world, world.people, 42, {{7, {0}}});
  REQUIRE(manager.stats().triggered == 1);

  auto v = manager.resolveCalendarEventVenue(world.people[0]);
  CHECK(v.first == 102);
  CHECK(v.second == 0);
}

// =============================================================================
// restore() repopulates venue cache (ordering enforced structurally)
// =============================================================================

TEST_CASE("restore repopulates venue cache for catchment-path persons") {
  WorldState world;
  world.geo_level_names = {"sgu"};
  GeographicalUnit gu; gu.id = 0; gu.parent_id = -1; gu.level_id = 0;
  world.geo_units.push_back(gu);
  world.venue_type_names = {"fair"};
  world.activity_names = {"Fair_accommodation"};
  world.schedule_type_names = {"regular", "fair_sched"};
  Person& p = world.people.emplace_back();
  p.id = 0; p.geo_unit_id = 0;
  world.buildIndices();

  const std::vector<VenueId> custom_venues = {100, 101, 102};

  CalendarEvent event;
  event.calendar_event_id = 7;
  event.start_day = 0;
  event.schedule_type_idx = 1;
  event.compliance_rate = 1.0f;
  event.catchment_rule_id = 7;
  event.candidate_venue_builder = [&custom_venues](const WorldState&) {
    return custom_venues;
  };

  CalendarEventManager manager({{event}});
  CalendarEventManager::Snapshot snap;
  snap.active_event[p.id] = 7;
  snap.event_trigger_seed[7] = 42;
  manager.restore(std::move(snap), world);

  auto after = manager.resolveCalendarEventVenue(world.people[0]);
  CHECK((after.first == 100 || after.first == 101 || after.first == 102));
  CHECK(after.second == 0);
}

// =============================================================================
// Checkpoint — active-event map round-trip
// =============================================================================

TEST_CASE("active-event map round-trips through snapshot_for_checkpoint/restore") {
  WorldState world;
  world.geo_level_names = {"sgu"};
  GeographicalUnit gu; gu.id = 0; gu.parent_id = -1; gu.level_id = 0;
  world.geo_units.push_back(gu);
  world.venue_type_names = {"fair"};
  world.activity_names = {"Fair_accommodation"};
  world.schedule_type_names = {"regular", "Fair_day_trip"};
  Person& p = world.people.emplace_back();
  p.id = 0; p.geo_unit_id = 0;
  world.buildIndices();

  CalendarEvent event;
  event.calendar_event_id = 42;
  event.start_day = 0;
  event.schedule_type_idx = 1;
  event.compliance_rate = 1.0f;
  event.catchment_rule_id = 0;
  event.candidate_venue_builder = [](const WorldState&) -> std::vector<VenueId> {
    return {7};
  };

  CalendarEventManager original({{event}});
  original.triggerEventsForDay(0, world, world.people, 1234, {{0, {0}}});
  REQUIRE(original.stats().triggered == 1);

  CalendarEventManager restored({{event}});
  restored.restore(original.snapshot_for_checkpoint(), world);

  CHECK(restored.hasActiveEvent(world.people[0].id));
  auto venue = restored.resolveCalendarEventVenue(world.people[0]);
  CHECK(venue.first == 7);
  CHECK(venue.second == 0);
}

TEST_CASE("multi-day Fair assigns the same venue on every day of the hop") {
  // Regression for base_seed_ stomping: a different event fires on day 1 with a
  // different base_seed. The person hopped by the Fair event must resolve to the
  // same guest-house on days 1 and 2 as on day 0.
  WorldState world;
  world.geo_level_names = {"sgu"};
  GeographicalUnit gu; gu.id = 0; gu.parent_id = -1; gu.level_id = 0;
  world.geo_units.push_back(gu);
  world.venue_type_names = {"guest_house"};
  for (VenueId vid : {10, 11, 12}) {
    Venue v; v.id = vid; v.type_id = 0; v.geo_unit_id = 0;
    world.venues.push_back(v);
  }
  world.activity_names = {"Fair_lodging"};
  world.schedule_type_names = {"regular", "Fair_lodging"};
  Person& person = world.people.emplace_back();
  person.id = 0; person.geo_unit_id = 0;
  world.buildIndices();

  std::vector<uint64_t> seeds_at_resolve;

  CalendarEvent fair_event;
  fair_event.calendar_event_id = 1;
  fair_event.start_day = 0;
  fair_event.schedule_type_idx = 1;
  fair_event.compliance_rate = 1.0f;
  fair_event.duration_days = 3;
  fair_event.catchment_rule_id = 0;
  fair_event.candidate_venue_builder = [](const WorldState& w) {
    return w.getVenuesInGeoUnit(0, "guest_house");
  };
  fair_event.venue_selector = [&seeds_at_resolve](
      const std::vector<VenueId>& candidates, PersonId, uint64_t seed) {
    seeds_at_resolve.push_back(seed);
    return std::make_pair(candidates[0], SubsetIndex{0});
  };

  CalendarEvent day1_event;
  day1_event.calendar_event_id = 2;
  day1_event.start_day = 1;
  day1_event.schedule_type_idx = 1;
  day1_event.compliance_rate = 0.0f;  // never triggers anyone
  day1_event.duration_days = 1;
  day1_event.catchment_rule_id = 0;
  day1_event.candidate_venue_builder = [](const WorldState& w) {
    return w.getVenuesInGeoUnit(0, "guest_house");
  };

  CalendarEventManager manager({{fair_event}, {day1_event}, {}});

  manager.triggerEventsForDay(0, world, world.people, /*seed=*/100, {{0, {0}}});
  REQUIRE(manager.stats().triggered == 1);
  manager.resolveCalendarEventVenue(world.people[0]);

  manager.triggerEventsForDay(1, world, world.people, /*seed=*/999999, {{0, {0}}});
  manager.resolveCalendarEventVenue(world.people[0]);

  manager.triggerEventsForDay(2, world, world.people, /*seed=*/777, {{0, {0}}});
  manager.resolveCalendarEventVenue(world.people[0]);

  REQUIRE(seeds_at_resolve.size() == 3);
  CHECK(seeds_at_resolve[1] == seeds_at_resolve[0]);
  CHECK(seeds_at_resolve[2] == seeds_at_resolve[0]);
}
