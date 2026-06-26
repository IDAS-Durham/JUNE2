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

// Append a "calendar_event_id" membership field to the world; returns its idx.
int addCalendarEventField(WorldState& world) {
  world.membership_field_names.push_back(kCalendarEventIdField);
  world.membership_field_values.emplace_back();
  return static_cast<int>(world.membership_field_names.size() - 1);
}

// Give `person` one activity (`activity_idx`) whose candidate venues are
// `venues`. For each venue, `event_ids[i] >= 0` writes that calendar_event_id
// into the membership field at the venue's flat index; -1 leaves it absent.
// Returns the activity's venue_start (first flat index).
uint32_t giveActivityWithEventVenues(
    WorldState& world, Person& person, int16_t activity_idx,
    const std::vector<std::pair<VenueId, SubsetIndex>>& venues,
    const std::vector<int32_t>& event_ids, int cei_field) {
  person.activity_meta_start = static_cast<uint32_t>(world.activity_meta.size());
  person.activity_meta_count = 1;

  Person::ActivityMeta meta;
  meta.activity_index = activity_idx;
  meta.venue_start = static_cast<uint32_t>(world.activity_venues.size());
  meta.venue_count = static_cast<uint16_t>(venues.size());
  world.activity_meta.push_back(meta);

  for (size_t i = 0; i < venues.size(); ++i) {
    uint32_t flat_idx = meta.venue_start + static_cast<uint32_t>(i);
    world.activity_venues.push_back(venues[i]);
    if (event_ids[i] >= 0)
      world.membership_field_values[cei_field][flat_idx] =
          static_cast<float>(event_ids[i]);
  }
  return meta.venue_start;
}

// Resolve a TimeSlot's allowed_activity_indices from the world.
void resolveSlotIndices(TimeSlot& slot, const WorldState& world) {
  slot.allowed_activity_indices.clear();
  for (const auto& act : slot.allowed_activities) {
    int idx = world.getActivityIndex(act);
    if (idx >= 0)
      slot.allowed_activity_indices.push_back(static_cast<int16_t>(idx));
  }
}

}  // namespace

// =============================================================================
// Cycle 1: resolver returns the venue tagged with the active calendar_event_id
// =============================================================================

TEST_CASE("resolver returns the venue matching the active calendar event") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  world.activity_names = {"residence", "Fair_accommodation"};
  world.buildIndices();
  int cei = addCalendarEventField(world);
  const int16_t fair_idx = 1;

  giveActivityWithEventVenues(world, world.people[0], fair_idx, {{7, 3}}, {42},
                              cei);

  CalendarEventManager manager;
  manager.setActiveEvents({{world.people[0].id, 42}});

  auto venue = manager.resolveCalendarEventVenue(world, world.people[0], fair_idx);
  CHECK(venue.first == 7);
  CHECK(venue.second == 3);
}

// =============================================================================
// Cycle 2: no active event / no matching membership -> {-1,-1} (fall-through)
// =============================================================================

TEST_CASE("resolver returns no-match when person has no active event") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  world.activity_names = {"residence", "Fair_accommodation"};
  world.buildIndices();
  int cei = addCalendarEventField(world);
  const int16_t fair_idx = 1;
  giveActivityWithEventVenues(world, world.people[0], fair_idx, {{7, 3}}, {42},
                              cei);

  CalendarEventManager manager;  // no active events set

  auto venue = manager.resolveCalendarEventVenue(world, world.people[0], fair_idx);
  CHECK(venue.first == -1);
  CHECK(venue.second == -1);
}

TEST_CASE("resolver returns no-match when active id matches no candidate venue") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  world.activity_names = {"residence", "Fair_accommodation"};
  world.buildIndices();
  int cei = addCalendarEventField(world);
  const int16_t fair_idx = 1;
  giveActivityWithEventVenues(world, world.people[0], fair_idx, {{7, 3}}, {42},
                              cei);

  CalendarEventManager manager;
  manager.setActiveEvents({{world.people[0].id, 99}});  // no venue tagged 99

  auto venue = manager.resolveCalendarEventVenue(world, world.people[0], fair_idx);
  CHECK(venue.first == -1);
  CHECK(venue.second == -1);
}

// =============================================================================
// Cycle 3: triggerEventsForDay sets hop fields + active-event id for attendees
// =============================================================================

TEST_CASE("trigger sets hop fields and records active event for attendees") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  world.activity_names = {"residence", "Fair_accommodation"};
  world.buildIndices();
  int cei = addCalendarEventField(world);
  const int16_t fair_idx = 1;
  giveActivityWithEventVenues(world, world.people[0], fair_idx, {{7, 3}}, {42},
                              cei);

  CalendarEvent event;
  event.calendar_event_id = 42;
  event.start_day = 0;
  event.schedule_type_idx = 5;
  event.compliance_rate = 1.0f;
  CalendarEventManager manager({{event}});

  manager.triggerEventsForDay(0, world, world.people, /*base_seed=*/123);

  CHECK(world.people[0].hopped_schedule_id == 5);
  CHECK(world.people[0].return_schedule_id == -1);
  CHECK(world.people[0].temp_slot_progress == 0);
  CHECK(world.people[0].hop_repeats_remaining == 1);
  CHECK(manager.hasActiveEvent(world.people[0].id));
  CHECK(manager.stats().triggered == 1);
}

TEST_CASE("trigger sets hop_repeats_remaining from event duration_days") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  world.activity_names = {"residence", "Fair_accommodation"};
  world.buildIndices();
  int cei = addCalendarEventField(world);
  giveActivityWithEventVenues(world, world.people[0], 1, {{7, 3}}, {42}, cei);

  CalendarEvent event;
  event.calendar_event_id = 42;
  event.start_day = 0;
  event.schedule_type_idx = 5;
  event.compliance_rate = 1.0f;
  event.duration_days = 3;
  CalendarEventManager manager({{event}});

  manager.triggerEventsForDay(0, world, world.people, 123);

  CHECK(world.people[0].hop_repeats_remaining == 3);
}

// =============================================================================
// Cycle 4: two events / two candidate venues — disambiguation + stale-id clear
// =============================================================================

TEST_CASE("onHopCompleted clears stale id so next event resolves its own venue") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  world.activity_names = {"residence", "Fair_accommodation"};
  world.buildIndices();
  int cei = addCalendarEventField(world);
  const int16_t fair_idx = 1;
  // Two candidate fair venues, one per event id.
  giveActivityWithEventVenues(world, world.people[0], fair_idx,
                              {{7, 0}, {8, 0}}, {42, 99}, cei);

  CalendarEvent first;
  first.calendar_event_id = 42;
  first.start_day = 0;
  first.schedule_type_idx = 2;
  CalendarEvent second;
  second.calendar_event_id = 99;
  second.start_day = 1;
  second.schedule_type_idx = 2;
  CalendarEventManager manager({{first}, {second}});

  // Day 0: event 42 fires -> resolves venue 7.
  manager.triggerEventsForDay(0, world, world.people, 123);
  CHECK(manager.resolveCalendarEventVenue(world, world.people[0], fair_idx)
            .first == 7);

  // Hop finishes: simulate the auto-return that clears the id.
  world.people[0].hopped_schedule_id = -1;
  manager.onHopCompleted(world.people[0].id);
  CHECK(manager.resolveCalendarEventVenue(world, world.people[0], fair_idx)
            .first == -1);

  // Day 1: event 99 fires -> resolves the *other* venue (8), not the stale 7.
  manager.triggerEventsForDay(1, world, world.people, 123);
  CHECK(manager.resolveCalendarEventVenue(world, world.people[0], fair_idx)
            .first == 8);
}

// =============================================================================
// Cycle 5: collision — a person already mid-hop is not overridden
// =============================================================================

TEST_CASE("trigger skips a person already on a hopped schedule") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  world.activity_names = {"residence", "Fair_accommodation"};
  world.buildIndices();
  int cei = addCalendarEventField(world);
  giveActivityWithEventVenues(world, world.people[0], 1, {{7, 3}}, {42}, cei);
  world.people[0].hopped_schedule_id = 3;  // already mid-hop

  CalendarEvent event;
  event.calendar_event_id = 42;
  event.start_day = 0;
  event.schedule_type_idx = 5;
  CalendarEventManager manager({{event}});

  manager.triggerEventsForDay(0, world, world.people, 123);

  CHECK(world.people[0].hopped_schedule_id == 3);  // unchanged
  CHECK_FALSE(manager.hasActiveEvent(world.people[0].id));
  CHECK(manager.stats().triggered == 0);
  CHECK(manager.stats().skipped_collision == 1);
}

// =============================================================================
// Cycle 6: compliance 0.0 / 1.0 deterministic trigger/skip
// =============================================================================

TEST_CASE("compliance rate 1.0 always triggers, 0.0 never triggers") {
  auto buildWorld = []() {
    WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
    world.activity_names = {"residence", "Fair_accommodation"};
    world.buildIndices();
    return world;
  };

  SUBCASE("compliance 1.0 triggers") {
    WorldState world = buildWorld();
    int cei = addCalendarEventField(world);
    giveActivityWithEventVenues(world, world.people[0], 1, {{7, 3}}, {42}, cei);
    CalendarEvent event;
    event.calendar_event_id = 42;
    event.start_day = 0;
    event.schedule_type_idx = 5;
    event.compliance_rate = 1.0f;
    CalendarEventManager manager({{event}});
    manager.triggerEventsForDay(0, world, world.people, 123);
    CHECK(manager.stats().triggered == 1);
    CHECK(manager.stats().skipped_compliance == 0);
  }

  SUBCASE("compliance 0.0 never triggers") {
    WorldState world = buildWorld();
    int cei = addCalendarEventField(world);
    giveActivityWithEventVenues(world, world.people[0], 1, {{7, 3}}, {42}, cei);
    CalendarEvent event;
    event.calendar_event_id = 42;
    event.start_day = 0;
    event.schedule_type_idx = 5;
    event.compliance_rate = 0.0f;
    CalendarEventManager manager({{event}});
    manager.triggerEventsForDay(0, world, world.people, 123);
    CHECK(manager.stats().triggered == 0);
    CHECK(manager.stats().skipped_compliance == 1);
  }
}

// =============================================================================
// Cycle 7: selectVenue guard drives the assignment through ActivityManager
// =============================================================================

namespace {

// Build a world + config where person 0 is mid-hop on a temporary fair schedule
// with two candidate Fair_accommodation venues (id 1 tagged event 42, id 2
// tagged event 99). Returns the configured ActivityManager-ready Config.
struct FairHopFixture {
  WorldState world;
  Config config;
  int16_t fair_idx = 1;
};

FairHopFixture buildFairHopFixture() {
  FairHopFixture fixture;
  WorldState& world = fixture.world;
  world = TestWorldFactory::createMinimalWorld(1, 3);
  world.activity_names = {"residence", "Fair_accommodation", "none", "dead"};
  world.venue_type_names = {"home", "guesthouse"};
  world.venues[0].type_id = 0;
  world.venues[1].type_id = 1;
  world.venues[2].type_id = 1;
  world.buildIndices();

  int cei = addCalendarEventField(world);
  giveActivityWithEventVenues(world, world.people[0], fixture.fair_idx,
                              {{1, 0}, {2, 0}}, {42, 99}, cei);

  ScheduleType regular;
  regular.name = "regular";
  ScheduleType fair_temp;
  fair_temp.name = "fair_1day";
  fair_temp.is_temporary = true;
  TimeSlot fair_slot;
  fair_slot.name = "fair_day";
  fair_slot.allowed_activities = {"Fair_accommodation"};
  resolveSlotIndices(fair_slot, world);
  fair_temp.flat_slots.push_back(fair_slot);

  Config& config = fixture.config;
  config.schedule.day_type_cycle = {"day"};
  config.schedule.day_type_names = {"day"};
  config.schedule.schedule_types.push_back(regular);
  config.schedule.schedule_types.push_back(fair_temp);
  config.schedule.default_schedule_type = "regular";
  config.performance.precompute_schedules = false;
  config.resolve(world);

  world.schedule_type_names = {"regular", "fair_1day"};
  world.num_day_types = 1;

  // Put person mid-hop on the temporary fair schedule (idx 1), slot 0.
  world.people[0].hopped_schedule_id = 1;
  world.people[0].temp_slot_progress = 0;
  world.people[0].schedule_type_id = 0;
  return fixture;
}

}  // namespace

TEST_CASE("selectVenue guard resolves the calendar-event venue during a hop") {
  FairHopFixture fixture = buildFairHopFixture();
  ActivityManager manager(fixture.world, fixture.config);

  CalendarEventManager calendar_manager;
  calendar_manager.setActiveEvents({{fixture.world.people[0].id, 99}});
  manager.setCalendarEventManager(&calendar_manager);

  std::vector<PersonLocation> locations(1);
  manager.assignActivitiesFromSchedule(0, 0, locations);

  CHECK(locations[0].activity_index == fixture.fair_idx);
  CHECK(locations[0].venue_id == 2);  // event 99 -> venue 2 (resolver), not 1
  CHECK(locations[0].subset_index == 0);
}

TEST_CASE("selectVenue is unaffected when no calendar-event manager is set") {
  FairHopFixture fixture = buildFairHopFixture();
  ActivityManager manager(fixture.world, fixture.config);
  // No calendar-event manager -> normal hierarchical selection.

  std::vector<PersonLocation> locations(1);
  manager.assignActivitiesFromSchedule(0, 0, locations);

  CHECK(locations[0].activity_index == fixture.fair_idx);
  // Picks one of the candidate guesthouse venues, none excluded by the guard.
  CHECK((locations[0].venue_id == 1 || locations[0].venue_id == 2));
}

// =============================================================================
// Cycle 8: loader happy path (new FilteredTable schema)
// =============================================================================

TEST_CASE("loader parses new-schema CSV into the day-indexed table") {
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

  // duration_days column is blank
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
// Cycle 9: loader errors / out-of-window rows (new schema)
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
  // header present but data row has only 3 columns
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

TEST_CASE("trigger throws when calendar_event_id membership field is missing") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  world.activity_names = {"residence", "Fair_accommodation"};
  world.buildIndices();
  // Deliberately do NOT add the calendar_event_id membership field.

  CalendarEvent event;
  event.calendar_event_id = 42;
  event.start_day = 0;
  CalendarEventManager manager({{event}});

  CHECK_THROWS_AS(manager.triggerEventsForDay(0, world, world.people, 123),
                  std::runtime_error);
}

// =============================================================================
// Cycle 12: getVenuesInGeoUnit — exact match and descendant traversal
// =============================================================================

// Build a 3-level geo_unit hierarchy: root(0) -> child(1) -> grandchild(2).
// Venues: id=10 (type="fair", geo=1), id=11 (type="fair", geo=2),
//         id=12 (type="household", geo=1).
static WorldState buildGeoHierarchyWorld() {
  WorldState world;
  world.venue_type_names = {"fair", "household"};

  // root
  GeographicalUnit root;
  root.id = 0; root.parent_id = -1; root.level_id = 0;
  world.geo_units.push_back(root);
  // child
  GeographicalUnit child;
  child.id = 1; child.parent_id = 0; child.level_id = 1;
  world.geo_units.push_back(child);
  // grandchild
  GeographicalUnit grandchild;
  grandchild.id = 2; grandchild.parent_id = 1; grandchild.level_id = 2;
  world.geo_units.push_back(grandchild);

  Venue va; va.id = 10; va.type_id = 0; va.geo_unit_id = 1;  // fair @ child
  Venue vb; vb.id = 11; vb.type_id = 0; vb.geo_unit_id = 2;  // fair @ grandchild
  Venue vc; vc.id = 12; vc.type_id = 1; vc.geo_unit_id = 1;  // household @ child
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
  // sorted by venue_id
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
  // 3 geo_units: 10, 11, 12. People: person 0 @ 10, person 1 @ 11, person 2 @ 12.
  // Catchment rule 7: {10, 11} -> should trigger persons 0 and 1 only.
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
    p.id = i;
    p.geo_unit_id = 10 + i;
    p.age = 30.0f;
  }
  world.buildIndices();

  CalendarEvent event;
  event.calendar_event_id = 7;
  event.start_day = 0;
  event.schedule_type_idx = 1;
  event.compliance_rate = 1.0f;
  event.catchment_rule_id = 7;
  event.hosting_geo_unit_id = 10;
  event.venue_type_name = "fair";

  std::unordered_map<int32_t, std::vector<GeoUnitId>> catchment_rules;
  catchment_rules[7] = {10, 11};  // geo_units 10 and 11

  CalendarEventManager manager({{event}});
  manager.triggerEventsForDay(0, world, world.people, 42, catchment_rules);

  CHECK(manager.stats().triggered == 2);
  // person 0 (geo 10) and person 1 (geo 11) triggered; person 2 (geo 12) not
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

  // 3 people: ages 10, 30, 50 (all in geo_unit 0)
  for (int i = 0; i < 3; ++i) {
    Person& p = world.people.emplace_back();
    p.id = i;
    p.geo_unit_id = 0;
    p.age = 10.0f + 20.0f * i;  // 10, 30, 50
  }
  world.buildIndices();

  // Filter: age >= 20 (excludes person 0 aged 10)
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
  event.hosting_geo_unit_id = 0;
  event.venue_type_name = "fair";
  event.attendee_filters = {age_filter};

  std::unordered_map<int32_t, std::vector<GeoUnitId>> catchment_rules;
  catchment_rules[0] = {0};

  CalendarEventManager manager({{event}});
  manager.triggerEventsForDay(0, world, world.people, 42, catchment_rules);

  // persons 1 (age 30) and 2 (age 50) triggered; person 0 (age 10) excluded
  CHECK(manager.stats().triggered == 2);
  CHECK(world.people[0].hopped_schedule_id == -1);
  CHECK(world.people[1].hopped_schedule_id == 1);
  CHECK(world.people[2].hopped_schedule_id == 1);
}

// =============================================================================
// Cycle 15: dynamic hashed venue resolution for catchment-rule events
// =============================================================================

TEST_CASE("catchment-rule event resolves venue via hash into candidate list") {
  // 3 guest-house venues (ids 20,21,22) at geo_unit 0; 2 people in catchment.
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
  event.hosting_geo_unit_id = 0;
  event.venue_type_name = "guest_house";

  std::unordered_map<int32_t, std::vector<GeoUnitId>> catchment_rules;
  catchment_rules[0] = {0};

  CalendarEventManager manager({{event}});
  manager.triggerEventsForDay(0, world, world.people, /*base_seed=*/999,
                              catchment_rules);
  REQUIRE(manager.stats().triggered == 2);

  // Each person resolves to one of the 3 candidates with subset_index 0.
  auto v0 = manager.resolveCalendarEventVenue(world, world.people[0], 0);
  auto v1 = manager.resolveCalendarEventVenue(world, world.people[1], 0);
  CHECK((v0.first == 20 || v0.first == 21 || v0.first == 22));
  CHECK(v0.second == 0);
  CHECK((v1.first == 20 || v1.first == 21 || v1.first == 22));
  CHECK(v1.second == 0);

  // Deterministic: same person gets same venue on re-query.
  auto v0_again = manager.resolveCalendarEventVenue(world, world.people[0], 0);
  CHECK(v0_again.first == v0.first);
}

TEST_CASE("catchment-rule resolver returns no-venue when candidate list is empty") {
  WorldState world;
  world.geo_level_names = {"sgu"};
  GeographicalUnit gu; gu.id = 0; gu.parent_id = -1; gu.level_id = 0;
  world.geo_units.push_back(gu);
  world.venue_type_names = {"fair"};
  // No venues added.
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
  event.hosting_geo_unit_id = 0;
  event.venue_type_name = "fair";  // no fair venues

  std::unordered_map<int32_t, std::vector<GeoUnitId>> catchment_rules;
  catchment_rules[0] = {0};

  CalendarEventManager manager({{event}});
  manager.triggerEventsForDay(0, world, world.people, 42, catchment_rules);
  REQUIRE(manager.stats().triggered == 1);

  auto v = manager.resolveCalendarEventVenue(world, world.people[0], 0);
  CHECK(v.first == -1);
  CHECK(v.second == -1);
}

// =============================================================================
// Cycle A: event with custom candidate_venue_builder uses it to resolve venues
// =============================================================================

TEST_CASE("event with custom builder uses it to populate venue candidates") {
  // No fair venues exist in the world — proves builder is called, not
  // getVenuesInGeoUnit (which would return empty and force {-1,-1}).
  WorldState world;
  world.geo_level_names = {"sgu"};
  GeographicalUnit gu;
  gu.id = 0; gu.parent_id = -1; gu.level_id = 0;
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

  std::unordered_map<int32_t, std::vector<GeoUnitId>> catchment_rules;
  catchment_rules[7] = {0};

  CalendarEventManager manager({{event}});
  manager.triggerEventsForDay(0, world, world.people, 42, catchment_rules);
  REQUIRE(manager.stats().triggered == 1);

  // Resolver must return one of the builder's venues, not -1 (empty world).
  auto v = manager.resolveCalendarEventVenue(world, world.people[0], 0);
  CHECK((v.first == 100 || v.first == 101 || v.first == 102));
  CHECK(v.second == 0);
}

// =============================================================================
// Cycle B: custom venue_selector is called at resolve time
// =============================================================================

TEST_CASE("event with custom venue_selector uses it instead of hash-select") {
  // Builder returns {100, 101, 102}. Selector always picks the last entry.
  // If hash-select were used, the result would be non-deterministic; the custom
  // selector pins it to 102, making the test deterministic and falsifiable.
  WorldState world;
  world.geo_level_names = {"sgu"};
  GeographicalUnit gu;
  gu.id = 0; gu.parent_id = -1; gu.level_id = 0;
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
  // Always picks the last candidate — deterministic, not hash-based.
  event.venue_selector = [](const std::vector<VenueId>& candidates,
                             PersonId, uint64_t) {
    return std::make_pair(candidates.back(), SubsetIndex{0});
  };

  std::unordered_map<int32_t, std::vector<GeoUnitId>> catchment_rules;
  catchment_rules[7] = {0};

  CalendarEventManager manager({{event}});
  manager.triggerEventsForDay(0, world, world.people, 42, catchment_rules);
  REQUIRE(manager.stats().triggered == 1);

  auto v = manager.resolveCalendarEventVenue(world, world.people[0], 0);
  CHECK(v.first == 102);  // last entry, as the custom selector dictates
  CHECK(v.second == 0);
}

// =============================================================================
// Cycle C: loader sets candidate_venue_builder and venue_selector for catchment events
// =============================================================================

TEST_CASE("loader sets candidate_venue_builder for catchment event") {
  // buildGeoHierarchyWorld: hosting_geo_unit_id=1 has fair venues 10 and 11.
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

TEST_CASE("loader leaves builder and selector null for membership-field event") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  world.schedule_type_names = {"regular", "Fair_day_trip"};

  // catchment_rule_id == -1 → membership-field path; no builder/selector needed.
  std::string csv =
      "calendar_event_id,date,schedule_name,hosting_geo_unit_id,venue_type_name,"
      "catchment_rule_id,duration_days,compliance_rate,category\n"
      "42,2021-01-05,Fair_day_trip,0,fair,-1,1,1.0,fair\n";
  std::istringstream input(csv);
  auto table = CalendarEventLoader::parse(input, world, "2021-01-01", 30, "test.csv");
  REQUIRE(table[4].size() == 1);
  const CalendarEvent& event = table[4][0];

  CHECK(event.candidate_venue_builder == nullptr);
  CHECK(event.venue_selector == nullptr);
}

// =============================================================================
// Cycle D: rebuildVenueCachesAfterRestore fixes the restore-path bug
// =============================================================================

TEST_CASE("rebuildVenueCachesAfterRestore repopulates venue cache for catchment events") {
  // Simulate checkpoint restore: construct manager with a catchment event but
  // never call triggerEventsForDay. Call setActiveEvents directly (as the
  // restore code does), then call rebuildVenueCachesAfterRestore and verify
  // the resolver returns a valid venue instead of {-1,-1}.
  WorldState world;
  world.geo_level_names = {"sgu"};
  GeographicalUnit gu;
  gu.id = 0; gu.parent_id = -1; gu.level_id = 0;
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

  // After rebuilding caches the resolver must pick from the builder's pool.
  manager.rebuildVenueCachesAfterRestore(world);
  auto after = manager.resolveCalendarEventVenue(world, world.people[0], 0);
  CHECK((after.first == 100 || after.first == 101 || after.first == 102));
  CHECK(after.second == 0);
}

// =============================================================================
// Cycle 10: checkpoint accessors round-trip
// =============================================================================

TEST_CASE("active-event map round-trips through get/setActiveEvents") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  world.activity_names = {"residence", "Fair_accommodation"};
  world.buildIndices();
  int cei = addCalendarEventField(world);
  const int16_t fair_idx = 1;
  giveActivityWithEventVenues(world, world.people[0], fair_idx, {{7, 3}}, {42},
                              cei);

  CalendarEventManager original;
  original.setActiveEvents({{world.people[0].id, 42}});
  auto snapshot = original.getActiveEvents();

  CalendarEventManager restored;
  restored.setActiveEvents(snapshot);

  CHECK(restored.getActiveEvents() == original.getActiveEvents());
  auto venue = restored.resolveCalendarEventVenue(world, world.people[0], fair_idx);
  CHECK(venue.first == 7);
  CHECK(venue.second == 3);
}
