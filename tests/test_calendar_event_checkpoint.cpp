
#include "core/types.h"
#include "core/world_state.h"
#include "doctest.h"
#include "epidemiology/calendar_event.h"
#include "test_utils.h"

using namespace june;

namespace {

CalendarEventManager makeManager(int16_t duration = 3) {
  CalendarEvent e;
  e.calendar_event_id = 42;
  e.start_day = 0;
  e.schedule_type_idx = 1;
  e.compliance_rate = 1.0f;
  e.duration_days = duration;
  e.catchment_rule_id = 0;
  e.hosting_geo_unit_id = 0;
  return CalendarEventManager({{e}});
}

}  // namespace

// Venue resolution after restore is verified via getActiveHostingGeoUnit,
// which is what the OTF allocator reads to build the VenueResolveContext.
TEST_CASE("round-trip: active event and hosting geo-unit preserved after restore") {
  WorldState world = makeCatchmentWorld();
  CalendarEventManager original = makeManager(/*duration=*/3);
  original.triggerEventsForDay(0, world, world.people, 999, {{0, {0}}});

  REQUIRE(original.hasActiveEvent(world.people[0].id));
  REQUIRE(original.getActiveHostingGeoUnit(world.people[0].id).has_value());
  REQUIRE(*original.getActiveHostingGeoUnit(world.people[0].id) == 0);

  auto snap = original.snapshot_for_checkpoint();
  REQUIRE(snap.active_event.size() == 1);

  CalendarEventManager restored = makeManager(/*duration=*/3);
  restored.restore(std::move(snap));

  REQUIRE(restored.hasActiveEvent(world.people[0].id));
  CHECK(restored.getActiveHostingGeoUnit(world.people[0].id).has_value());
  CHECK(*restored.getActiveHostingGeoUnit(world.people[0].id) == 0);
}

TEST_CASE("restore on empty snapshot leaves manager idle") {
  WorldState world = makeCatchmentWorld();
  CalendarEventManager manager = makeManager();
  CalendarEventManager::Snapshot empty_snap;
  manager.restore(std::move(empty_snap));
  CHECK(!manager.hasActiveEvent(world.people[0].id));
}

TEST_CASE("snapshot_for_checkpoint on idle manager is empty") {
  WorldState world = makeCatchmentWorld();
  CalendarEventManager manager = makeManager();
  auto snap = manager.snapshot_for_checkpoint();
  CHECK(snap.active_event.empty());
}
