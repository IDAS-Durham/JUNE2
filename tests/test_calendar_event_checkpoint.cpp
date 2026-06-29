#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "core/types.h"
#include "core/world_state.h"
#include "doctest.h"
#include "epidemiology/calendar_event.h"
#include "test_utils.h"

using namespace june;

namespace {

WorldState buildCatchmentWorld() {
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
  Person& p = world.people.emplace_back();
  p.id = 0; p.geo_unit_id = 0;
  world.buildIndices();
  return world;
}

CalendarEventManager makeManager(int16_t duration = 3) {
  CalendarEvent e;
  e.calendar_event_id = 42;
  e.start_day = 0;
  e.schedule_type_idx = 1;
  e.compliance_rate = 1.0f;
  e.duration_days = duration;
  e.catchment_rule_id = 0;
  e.candidate_venue_builder = [](const WorldState& w) {
    return w.getVenuesInGeoUnit(0, "fair");
  };
  return CalendarEventManager({{e}});
}

}  // namespace

TEST_CASE("round-trip: resolveCalendarEventVenue matches after restore") {
  WorldState world = buildCatchmentWorld();
  CalendarEventManager original = makeManager(/*duration=*/3);
  original.triggerEventsForDay(0, world, world.people, 999, {{0, {0}}});

  REQUIRE(original.hasActiveEvent(world.people[0].id));
  const auto venue_before =
      original.resolveCalendarEventVenue(world.people[0]);
  REQUIRE(venue_before.first != -1);

  auto snap = original.snapshot_for_checkpoint();
  REQUIRE(snap.active_event.size() == 1);
  REQUIRE(snap.event_trigger_seed.size() == 1);

  CalendarEventManager restored = makeManager(/*duration=*/3);
  restored.restore(std::move(snap), world);

  REQUIRE(restored.hasActiveEvent(world.people[0].id));
  const auto venue_after = restored.resolveCalendarEventVenue(world.people[0]);
  CHECK(venue_after == venue_before);
}

TEST_CASE("restore on empty snapshot leaves manager idle") {
  WorldState world = buildCatchmentWorld();
  CalendarEventManager manager = makeManager();
  CalendarEventManager::Snapshot empty_snap;
  manager.restore(std::move(empty_snap), world);
  CHECK(!manager.hasActiveEvent(world.people[0].id));
}

TEST_CASE("snapshot_for_checkpoint on idle manager is empty") {
  WorldState world = buildCatchmentWorld();
  CalendarEventManager manager = makeManager();
  auto snap = manager.snapshot_for_checkpoint();
  CHECK(snap.active_event.empty());
  CHECK(snap.event_trigger_seed.empty());
}
