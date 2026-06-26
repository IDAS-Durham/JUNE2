#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <sstream>

#include "activity/activity_manager.h"
#include "core/config.h"
#include "core/types.h"
#include "core/world_state.h"
#include "doctest.h"
#include "epidemiology/calendar_event.h"
#include "loaders/calendar_event_loader.h"
#include "test_utils.h"

using namespace june;

namespace {

// Resolve a TimeSlot's allowed_activity_indices from the world.
void resolveSlotIndices(TimeSlot& slot, const WorldState& world) {
  slot.allowed_activity_indices.clear();
  for (const auto& act : slot.allowed_activities) {
    int idx = world.getActivityIndex(act);
    if (idx >= 0)
      slot.allowed_activity_indices.push_back(static_cast<int16_t>(idx));
  }
}

// Minimal world with `num_people` in geo_unit 0 and one fair venue there.
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

CalendarEvent makeCatchmentEvent(int32_t id, int16_t sched_idx,
                                 float compliance = 1.0f,
                                 int16_t duration = 1) {
  CalendarEvent e;
  e.calendar_event_id = id;
  e.start_day = 0;
  e.schedule_type_idx = sched_idx;
  e.compliance_rate = compliance;
  e.duration_days = duration;
  e.catchment_rule_id = 0;
  e.candidate_venue_builder = [](const WorldState& w) {
    return w.getVenuesInGeoUnit(0, "fair");
  };
  return e;
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

  // Give person 0 two Fair_accommodation candidate venues (ids 1 and 2).
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

  // Catchment event: builder returns venue ids {1, 2}; custom selector pins
  // to venue 2 for deterministic CHECK in the guard test.
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

  f.world.people[0].hopped_schedule_id = 1;
  f.world.people[0].temp_slot_progress = 0;
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
  auto result = manager.resolveCalendarEventVenue(world, world.people[0], 0);
  CHECK(result.first == -1);
  CHECK(result.second == -1);
}

// =============================================================================
// Trigger: hop fields and active-event state
// =============================================================================

TEST_CASE("trigger sets hop fields for catchment-rule event") {
  WorldState world = buildCatchmentWorld();
  CalendarEventManager manager({{makeCatchmentEvent(1, 1)}});
  manager.triggerEventsForDay(0, world, world.people, 123, {{0, {0}}});

  CHECK(world.people[0].hopped_schedule_id == 1);
  CHECK(world.people[0].return_schedule_id == -1);
  CHECK(world.people[0].temp_slot_progress == 0);
  CHECK(world.people[0].hop_repeats_remaining == 1);
  CHECK(manager.hasActiveEvent(world.people[0].id));
  CHECK(manager.stats().triggered == 1);
}

TEST_CASE("trigger sets hop_repeats_remaining from duration_days") {
  WorldState world = buildCatchmentWorld();
  CalendarEventManager manager({{makeCatchmentEvent(1, 1, 1.0f, 3)}});
  manager.triggerEventsForDay(0, world, world.people, 123, {{0, {0}}});
  CHECK(world.people[0].hop_repeats_remaining == 3);
}

TEST_CASE("trigger skips a person already on a hopped schedule") {
  WorldState world = buildCatchmentWorld();
  world.people[0].hopped_schedule_id = 3;  // already mid-hop
  CalendarEventManager manager({{makeCatchmentEvent(1, 5)}});
  manager.triggerEventsForDay(0, world, world.people, 123, {{0, {0}}});

  CHECK(world.people[0].hopped_schedule_id == 3);  // unchanged
  CHECK_FALSE(manager.hasActiveEvent(world.people[0].id));
  CHECK(manager.stats().triggered == 0);
  CHECK(manager.stats().skipped_collision == 1);
}

TEST_CASE("compliance rate 1.0 always triggers, 0.0 never triggers") {
  SUBCASE("compliance 1.0 triggers") {
    WorldState world = buildCatchmentWorld();
    CalendarEventManager manager({{makeCatchmentEvent(1, 1, 1.0f)}});
    manager.triggerEventsForDay(0, world, world.people, 123, {{0, {0}}});
    CHECK(manager.stats().triggered == 1);
    CHECK(manager.stats().skipped_compliance == 0);
  }
  SUBCASE("compliance 0.0 never triggers") {
    WorldState world = buildCatchmentWorld();
    CalendarEventManager manager({{makeCatchmentEvent(1, 1, 0.0f)}});
    manager.triggerEventsForDay(0, world, world.people, 123, {{0, {0}}});
    CHECK(manager.stats().triggered == 0);
    CHECK(manager.stats().skipped_compliance == 1);
  }
}

// =============================================================================
// Cycle 7: selectVenue guard drives the assignment through ActivityManager
// =============================================================================

TEST_CASE("selectVenue guard resolves the calendar-event venue during a hop") {
  FairHopFixture f = buildFairHopFixture();
  ActivityManager manager(f.world, f.config);

  CalendarEventManager calendar_manager({{f.event}});
  calendar_manager.setActiveEvents({{f.world.people[0].id, 99}});
  calendar_manager.rebuildVenueCachesAfterRestore(f.world);
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
// Cycle 8: loader happy path
// =============================================================================

TEST_CASE("loader parses CSV into the day-indexed table") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  world.schedule_type_names = {"regular", "Fair_day_trip", "Fair_lodging"};

  std::string csv =
      "calendar_event_id,date,schedule_name,hosting_geo_unit_id,venue_type_name,"
      "catchment_rule_id,duration_days,compliance_rate,category\n"
      "42,2021-01-05,Fair_day_trip,5250,fair,7,3,0.9,fair\n";
  std::istringstream input(csv);

  auto table = CalendarEventLoader::parse(input, world, "2021-01-01", 30,
                                          "test.csv");

  REQUIRE(table.size() == 30);
  REQUIRE(table[4].size() == 1);
  const CalendarEvent& e = table[4][0];
  CHECK(e.calendar_event_id == 42);
  CHECK(e.schedule_type_idx == world.getScheduleTypeIndex("Fair_day_trip"));
  CHECK(e.hosting_geo_unit_id == 5250);
  CHECK(e.venue_type_name == "fair");
  CHECK(e.catchment_rule_id == 7);
  CHECK(e.duration_days == 3);
  CHECK(e.compliance_rate == doctest::Approx(0.9f));
  CHECK(e.category == "fair");
}

TEST_CASE("loader parses filter.* columns into attendee_filters") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  world.schedule_type_names = {"regular", "Fair_day_trip"};

  std::string csv =
      "calendar_event_id,date,schedule_name,hosting_geo_unit_id,venue_type_name,"
      "catchment_rule_id,duration_days,compliance_rate,category,filter.age\n"
      "1,2021-01-05,Fair_day_trip,0,fair,0,1,1.0,fair,>=18\n";
  std::istringstream input(csv);

  auto table = CalendarEventLoader::parse(input, world, "2021-01-01", 30,
                                          "test.csv");

  REQUIRE(table[4].size() == 1);
  REQUIRE(table[4][0].attendee_filters.size() == 1);
  CHECK(table[4][0].attendee_filters[0].property_path == "age");
}

TEST_CASE("loader treats blank duration_days as 1") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  world.schedule_type_names = {"regular", "Fair_day_trip"};

  std::string csv =
      "calendar_event_id,date,schedule_name,hosting_geo_unit_id,venue_type_name,"
      "catchment_rule_id,duration_days,compliance_rate,category\n"
      "1,2021-01-05,Fair_day_trip,0,fair,0,,1.0,fair\n";
  std::istringstream input(csv);

  auto table = CalendarEventLoader::parse(input, world, "2021-01-01", 30,
                                          "test.csv");

  REQUIRE(table[4].size() == 1);
  CHECK(table[4][0].duration_days == 1);
}

// =============================================================================
// Cycle 9: loader errors / out-of-window rows
// =============================================================================

TEST_CASE("loader throws on an unknown schedule_name") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  world.schedule_type_names = {"regular"};
  std::string csv =
      "calendar_event_id,date,schedule_name,hosting_geo_unit_id,venue_type_name,"
      "catchment_rule_id,duration_days,compliance_rate,category\n"
      "42,2021-01-05,no_such_schedule,0,fair,0,1,1.0,fair\n";
  std::istringstream input(csv);
  CHECK_THROWS_AS(
      CalendarEventLoader::parse(input, world, "2021-01-01", 30, "test.csv"),
      std::runtime_error);
}

TEST_CASE("loader throws on a malformed row (missing required columns)") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  world.schedule_type_names = {"Fair_day_trip"};
  std::string csv =
      "calendar_event_id,date,schedule_name\n"
      "42,2021-01-05,Fair_day_trip\n";
  std::istringstream input(csv);
  CHECK_THROWS_AS(
      CalendarEventLoader::parse(input, world, "2021-01-01", 30, "test.csv"),
      std::runtime_error);
}

TEST_CASE("loader skips out-of-window rows but keeps in-window ones") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  world.schedule_type_names = {"Fair_day_trip"};
  std::string csv =
      "calendar_event_id,date,schedule_name,hosting_geo_unit_id,venue_type_name,"
      "catchment_rule_id,duration_days,compliance_rate,category\n"
      "1,2021-01-05,Fair_day_trip,0,fair,0,1,1.0,fair\n"
      "2,2021-03-01,Fair_day_trip,0,fair,0,1,1.0,fair\n";
  std::istringstream input(csv);

  auto table = CalendarEventLoader::parse(input, world, "2021-01-01", 30,
                                          "test.csv");
  size_t total = 0;
  for (const auto& day : table) total += day.size();
  CHECK(total == 1);
  CHECK(table[4].size() == 1);
}

// =============================================================================
// Cycle 12: getVenuesInGeoUnit — exact match and descendant traversal
// =============================================================================

static WorldState buildGeoHierarchyWorld() {
  WorldState world;
  world.venue_type_names = {"fair", "household"};

  GeographicalUnit root; root.id = 0; root.parent_id = -1; root.level_id = 0;
  world.geo_units.push_back(root);
  GeographicalUnit child; child.id = 1; child.parent_id = 0; child.level_id = 1;
  world.geo_units.push_back(child);
  GeographicalUnit grandchild;
  grandchild.id = 2; grandchild.parent_id = 1; grandchild.level_id = 2;
  world.geo_units.push_back(grandchild);

  Venue va; va.id = 10; va.type_id = 0; va.geo_unit_id = 1;
  Venue vb; vb.id = 11; vb.type_id = 0; vb.geo_unit_id = 2;
  Venue vc; vc.id = 12; vc.type_id = 1; vc.geo_unit_id = 1;
  world.venues.push_back(va);
  world.venues.push_back(vb);
  world.venues.push_back(vc);

  world.buildIndices();
  return world;
}

TEST_CASE("getVenuesInGeoUnit returns venues in exact unit") {
  WorldState world = buildGeoHierarchyWorld();
  auto venues = world.getVenuesInGeoUnit(2, "fair");
  REQUIRE(venues.size() == 1);
  CHECK(venues[0] == 11);
}

TEST_CASE("getVenuesInGeoUnit includes descendants") {
  WorldState world = buildGeoHierarchyWorld();
  auto venues = world.getVenuesInGeoUnit(1, "fair");
  REQUIRE(venues.size() == 2);
  CHECK(venues[0] == 10);
  CHECK(venues[1] == 11);
}

TEST_CASE("getVenuesInGeoUnit from root covers all descendants") {
  WorldState world = buildGeoHierarchyWorld();
  auto fair_venues = world.getVenuesInGeoUnit(0, "fair");
  REQUIRE(fair_venues.size() == 2);
  auto hh_venues = world.getVenuesInGeoUnit(0, "household");
  REQUIRE(hh_venues.size() == 1);
  CHECK(hh_venues[0] == 12);
}

TEST_CASE("getVenuesInGeoUnit returns empty for unknown type") {
  WorldState world = buildGeoHierarchyWorld();
  auto venues = world.getVenuesInGeoUnit(0, "nonexistent");
  CHECK(venues.empty());
}

// =============================================================================
// Cycle 13: geography-driven attendee resolution via catchment rules
// =============================================================================

TEST_CASE("catchment-rule event triggers people in specified geo_units") {
  WorldState world;
  world.geo_level_names = {"sgu"};
  for (GeoUnitId gid : {10, 11, 12}) {
    GeographicalUnit gu;
    gu.id = gid; gu.parent_id = -1; gu.level_id = 0;
    world.geo_units.push_back(gu);
  }
  world.venue_type_names = {"fair"};
  Venue v; v.id = 0; v.type_id = 0; v.geo_unit_id = 10;
  world.venues.push_back(v);
  world.activity_names = {"Fair_accommodation"};
  world.schedule_type_names = {"regular", "Fair_day_trip"};

  for (int i = 0; i < 3; ++i) {
    Person& p = world.people.emplace_back();
    p.id = i; p.geo_unit_id = 10 + i; p.age = 30.0f;
  }
  world.buildIndices();

  CalendarEvent event;
  event.calendar_event_id = 7;
  event.start_day = 0;
  event.schedule_type_idx = 1;
  event.compliance_rate = 1.0f;
  event.catchment_rule_id = 7;
  event.candidate_venue_builder = [](const WorldState& w) {
    return w.getVenuesInGeoUnit(10, "fair");
  };

  CalendarEventManager manager({{event}});
  manager.triggerEventsForDay(0, world, world.people, 42, {{7, {10, 11}}});

  CHECK(manager.stats().triggered == 2);
  CHECK(world.people[0].hopped_schedule_id == 1);
  CHECK(world.people[1].hopped_schedule_id == 1);
  CHECK(world.people[2].hopped_schedule_id == -1);
}

// =============================================================================
// Cycle 14: attendee_filters trim the catchment pool
// =============================================================================

TEST_CASE("attendee_filters on catchment event exclude non-matching people") {
  WorldState world;
  world.geo_level_names = {"sgu"};
  GeographicalUnit gu; gu.id = 0; gu.parent_id = -1; gu.level_id = 0;
  world.geo_units.push_back(gu);
  world.venue_type_names = {"fair"};
  Venue v; v.id = 0; v.type_id = 0; v.geo_unit_id = 0;
  world.venues.push_back(v);
  world.activity_names = {"Fair_accommodation"};
  world.schedule_type_names = {"regular", "Fair_day_trip"};
  world.person_property_names = {"age", "sex"};

  for (int i = 0; i < 3; ++i) {
    Person& p = world.people.emplace_back();
    p.id = i; p.geo_unit_id = 0;
    p.age = 10.0f + 20.0f * i;  // 10, 30, 50
  }
  world.buildIndices();

  SelectionCriterion age_filter;
  age_filter.property_path = "age";
  age_filter.operator_type = ">=";
  age_filter.value = 20;

  CalendarEvent event;
  event.calendar_event_id = 1;
  event.start_day = 0;
  event.schedule_type_idx = 1;
  event.compliance_rate = 1.0f;
  event.catchment_rule_id = 0;
  event.candidate_venue_builder = [](const WorldState& w) {
    return w.getVenuesInGeoUnit(0, "fair");
  };
  event.attendee_filters = {age_filter};

  CalendarEventManager manager({{event}});
  manager.triggerEventsForDay(0, world, world.people, 42, {{0, {0}}});

  CHECK(manager.stats().triggered == 2);
  CHECK(world.people[0].hopped_schedule_id == -1);
  CHECK(world.people[1].hopped_schedule_id == 1);
  CHECK(world.people[2].hopped_schedule_id == 1);
}

// =============================================================================
// Cycle 15: dynamic hashed venue resolution for catchment-rule events
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

  auto v0 = manager.resolveCalendarEventVenue(world, world.people[0], 0);
  auto v1 = manager.resolveCalendarEventVenue(world, world.people[1], 0);
  CHECK((v0.first == 20 || v0.first == 21 || v0.first == 22));
  CHECK(v0.second == 0);
  CHECK((v1.first == 20 || v1.first == 21 || v1.first == 22));
  CHECK(v1.second == 0);

  // Same person always gets the same venue.
  auto v0_again = manager.resolveCalendarEventVenue(world, world.people[0], 0);
  CHECK(v0_again.first == v0.first);
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

  auto v = manager.resolveCalendarEventVenue(world, world.people[0], 0);
  CHECK(v.first == -1);
  CHECK(v.second == -1);
}

// =============================================================================
// Cycle A: event with custom candidate_venue_builder uses it to resolve venues
// =============================================================================

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

  auto v = manager.resolveCalendarEventVenue(world, world.people[0], 0);
  CHECK((v.first == 100 || v.first == 101 || v.first == 102));
  CHECK(v.second == 0);
}

// =============================================================================
// Cycle B: custom venue_selector is called at resolve time
// =============================================================================

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

  auto v = manager.resolveCalendarEventVenue(world, world.people[0], 0);
  CHECK(v.first == 102);
  CHECK(v.second == 0);
}

// =============================================================================
// Cycle C: loader sets candidate_venue_builder and venue_selector for catchment events
// =============================================================================

TEST_CASE("loader sets candidate_venue_builder for catchment event") {
  WorldState world = buildGeoHierarchyWorld();
  world.schedule_type_names = {"regular", "Fair_day_trip"};

  std::string csv =
      "calendar_event_id,date,schedule_name,hosting_geo_unit_id,venue_type_name,"
      "catchment_rule_id,duration_days,compliance_rate,category\n"
      "42,2021-01-05,Fair_day_trip,1,fair,7,1,1.0,fair\n";
  std::istringstream input(csv);
  auto table = CalendarEventLoader::parse(input, world, "2021-01-01", 30, "test.csv");
  REQUIRE(table[4].size() == 1);
  const CalendarEvent& event = table[4][0];

  REQUIRE(event.candidate_venue_builder != nullptr);
  auto candidates = event.candidate_venue_builder(world);
  REQUIRE(candidates.size() == 2);
  CHECK(candidates[0] == 10);
  CHECK(candidates[1] == 11);
}

TEST_CASE("loader sets venue_selector for catchment event") {
  WorldState world = buildGeoHierarchyWorld();
  world.schedule_type_names = {"regular", "Fair_day_trip"};

  std::string csv =
      "calendar_event_id,date,schedule_name,hosting_geo_unit_id,venue_type_name,"
      "catchment_rule_id,duration_days,compliance_rate,category\n"
      "42,2021-01-05,Fair_day_trip,1,fair,7,1,1.0,fair\n";
  std::istringstream input(csv);
  auto table = CalendarEventLoader::parse(input, world, "2021-01-01", 30, "test.csv");
  REQUIRE(table[4].size() == 1);
  const CalendarEvent& event = table[4][0];

  REQUIRE(event.venue_selector != nullptr);
  const std::vector<VenueId> candidates = {10, 11};
  auto result = event.venue_selector(candidates, 0, 12345ULL);
  CHECK((result.first == 10 || result.first == 11));
  CHECK(result.second == 0);
}

// =============================================================================
// Cycle D: rebuildVenueCachesAfterRestore fixes the restore-path bug
// =============================================================================

TEST_CASE("rebuildVenueCachesAfterRestore repopulates venue cache for catchment events") {
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

  // Restore path: setActiveEvents without prior trigger — cache is empty.
  manager.setActiveEvents({{p.id, 7}});
  auto before = manager.resolveCalendarEventVenue(world, world.people[0], 0);
  CHECK(before.first == -1);

  manager.rebuildVenueCachesAfterRestore(world);
  auto after = manager.resolveCalendarEventVenue(world, world.people[0], 0);
  CHECK((after.first == 100 || after.first == 101 || after.first == 102));
  CHECK(after.second == 0);
}

// =============================================================================
// Cycle 10: checkpoint — active-event map round-trip
// =============================================================================

TEST_CASE("active-event map round-trips through get/setActiveEvents") {
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
  original.setActiveEvents({{p.id, 42}});
  auto snapshot = original.getActiveEvents();

  CalendarEventManager restored({{event}});
  restored.setActiveEvents(snapshot);
  restored.rebuildVenueCachesAfterRestore(world);

  CHECK(restored.getActiveEvents() == original.getActiveEvents());
  auto venue = restored.resolveCalendarEventVenue(world, world.people[0], 0);
  CHECK(venue.first == 7);
  CHECK(venue.second == 0);
}
