#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "core/types.h"
#include "core/world_state.h"
#include "doctest.h"
#include "epidemiology/calendar_event.h"
#include "test_utils.h"

using namespace june;

namespace {

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

}  // namespace

// =============================================================================
// Trigger: hop fields and active-event state
// =============================================================================

TEST_CASE("trigger sets hop fields for catchment-rule event") {
  WorldState world = buildCatchmentWorld();
  CalendarEventManager manager({{makeCatchmentEvent(1, 1)}});
  manager.triggerEventsForDay(0, world, world.people, 123, {{0, {0}}});

  CHECK(world.people[0].schedule_hop.hopped_schedule_id == 1);
  CHECK(world.people[0].schedule_hop.return_schedule_id == -1);
  CHECK(world.people[0].schedule_hop.temp_slot_progress == 0);
  CHECK(world.people[0].schedule_hop.repeats_remaining == 0);
  CHECK(manager.hasActiveEvent(world.people[0].id));
  CHECK(manager.stats().triggered == 1);
}

TEST_CASE("trigger sets hop_repeats_remaining from duration_days") {
  WorldState world = buildCatchmentWorld();
  CalendarEventManager manager({{makeCatchmentEvent(1, 1, 1.0f, 3)}});
  manager.triggerEventsForDay(0, world, world.people, 123, {{0, {0}}});
  CHECK(world.people[0].schedule_hop.repeats_remaining == 2);
}

TEST_CASE("trigger skips a person already on a hopped schedule") {
  WorldState world = buildCatchmentWorld();
  world.people[0].schedule_hop.hopped_schedule_id = 3;  // already mid-hop
  CalendarEventManager manager({{makeCatchmentEvent(1, 5)}});
  manager.triggerEventsForDay(0, world, world.people, 123, {{0, {0}}});

  CHECK(world.people[0].schedule_hop.hopped_schedule_id == 3);  // unchanged
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
// Catchment-rule attendee resolution
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
  CHECK(world.people[0].schedule_hop.hopped_schedule_id == 1);
  CHECK(world.people[1].schedule_hop.hopped_schedule_id == 1);
  CHECK(world.people[2].schedule_hop.hopped_schedule_id == -1);
}

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
  CHECK(world.people[0].schedule_hop.hopped_schedule_id == -1);
  CHECK(world.people[1].schedule_hop.hopped_schedule_id == 1);
  CHECK(world.people[2].schedule_hop.hopped_schedule_id == 1);
}
