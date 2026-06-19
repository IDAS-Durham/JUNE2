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
  CHECK(manager.hasActiveEvent(world.people[0].id));
  CHECK(manager.stats().triggered == 1);
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
// Cycle 8: loader happy path
// =============================================================================

TEST_CASE("loader parses a well-formed CSV into the day-indexed table") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  world.schedule_type_names = {"regular", "fair_1day", "fair_3day"};

  std::string csv =
      "calendar_event_id,date,schedule_name,venue_id,subset_index,compliance_rate,category\n"
      "42,2021-01-05,fair_1day,7,0,1.0,fair\n";
  std::istringstream input(csv);

  auto table = CalendarEventLoader::parse(input, world, "2021-01-01", 30,
                                          "test.csv");

  REQUIRE(table.size() == 30);
  REQUIRE(table[4].size() == 1);  // Jan 5 - Jan 1 = day 4
  CHECK(table[4][0].calendar_event_id == 42);
  CHECK(table[4][0].schedule_type_idx == world.getScheduleTypeIndex("fair_1day"));
  CHECK(table[4][0].venue_id == 7);
  CHECK(table[4][0].compliance_rate == doctest::Approx(1.0f));
  CHECK(table[4][0].category == "fair");
}

// =============================================================================
// Cycle 9: loader errors / out-of-window rows
// =============================================================================

TEST_CASE("loader throws on an unknown schedule_name") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  world.schedule_type_names = {"regular"};
  std::string csv =
      "calendar_event_id,date,schedule_name,venue_id,subset_index,compliance_rate,category\n"
      "42,2021-01-05,no_such_schedule,7,0,1.0,fair\n";
  std::istringstream input(csv);
  CHECK_THROWS_AS(
      CalendarEventLoader::parse(input, world, "2021-01-01", 30, "test.csv"),
      std::runtime_error);
}

TEST_CASE("loader throws on a malformed row (wrong column count)") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  world.schedule_type_names = {"fair_1day"};
  std::string csv = "42,2021-01-05,fair_1day,7,0\n";  // 5 columns
  std::istringstream input(csv);
  CHECK_THROWS_AS(
      CalendarEventLoader::parse(input, world, "2021-01-01", 30, "test.csv"),
      std::runtime_error);
}

TEST_CASE("loader skips out-of-window rows but keeps in-window ones") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  world.schedule_type_names = {"fair_1day"};
  std::string csv =
      "calendar_event_id,date,schedule_name,venue_id,subset_index,compliance_rate,category\n"
      "1,2021-01-05,fair_1day,7,0,1.0,fair\n"
      "2,2021-03-01,fair_1day,8,0,1.0,fair\n";  // ~day 59, outside 30-day window
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
// Cycle 10: multi-category concurrent merge
// =============================================================================

TEST_CASE("mergeEvents combines two categories into one day-indexed table") {
  CalendarEvent fair;
  fair.calendar_event_id = 1;
  fair.start_day = 0;
  fair.category = "fair";
  CalendarEvent festival;
  festival.calendar_event_id = 2;
  festival.start_day = 0;
  festival.category = "festival";

  std::vector<std::vector<CalendarEvent>> fairs = {{fair}};
  std::vector<std::vector<CalendarEvent>> festivals = {{festival}};

  CalendarEventManager manager(fairs);
  manager.mergeEvents(festivals);

  const auto& day0 = manager.eventsForDay(0);
  REQUIRE(day0.size() == 2);
  CHECK(day0[0].category == "fair");
  CHECK(day0[1].category == "festival");
}

// =============================================================================
// Cycle 11: checkpoint accessors round-trip
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
