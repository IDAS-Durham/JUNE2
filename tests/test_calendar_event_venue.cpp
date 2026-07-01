#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "activity/activity_manager.h"
#include "activity/on_the_fly_venue_allocator.h"
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
  f.world.people[0].schedule_hop.hopped_schedule_id = 1;
  f.world.people[0].schedule_hop.temp_slot_progress = 0;
  f.world.people[0].schedule_type_id = 0;
  return f;
}

}  // namespace

// =============================================================================
// selectVenue behaviour with and without CalendarEventManager
// =============================================================================

TEST_CASE("selectVenue is unaffected when no calendar-event manager is set") {
  FairHopFixture f = buildFairHopFixture();
  ActivityManager manager(f.world, f.config);

  std::vector<PersonLocation> locations(1);
  manager.assignActivitiesFromSchedule(0, 0, locations);

  CHECK(locations[0].activity_index == f.fair_idx);
  CHECK((locations[0].venue_id == 1 || locations[0].venue_id == 2));
}

// =============================================================================
// CalendarEventManager state persistence
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

  CalendarEventManager original({{event}});
  original.triggerEventsForDay(0, world, world.people, 1234, {{0, {0}}});
  REQUIRE(original.stats().triggered == 1);

  CalendarEventManager restored({{event}});
  restored.restore(original.snapshot_for_checkpoint());

  CHECK(restored.hasActiveEvent(world.people[0].id));
}

// =============================================================================
// OTF fixed-stability: same venue on every day of a multi-day hop
// =============================================================================

TEST_CASE("OTF fixed-stability rule assigns same venue across days of a hop") {
  // Three guest-house venues; person on a 3-day hop with no pre-baked venues.
  // OTF allocator uses venue_stability: fixed → seed excludes sim_day →
  // same venue each day regardless of current_sim_day_.
  WorldState world;
  world.geo_level_names = {"sgu"};
  GeographicalUnit gu; gu.id = 0; gu.parent_id = -1; gu.level_id = 0;
  world.geo_units.push_back(gu);
  world.venue_type_names = {"guest_house"};
  for (VenueId vid : {10, 11, 12}) {
    Venue v; v.id = vid; v.type_id = 0; v.geo_unit_id = 0;
    world.venues.push_back(v);
  }
  world.activity_names = {"residence", "fair_lodging", "none", "dead",
                           "no_venue"};
  world.schedule_type_names = {"regular", "fair_hop"};

  Person& person = world.people.emplace_back();
  person.id = 0; person.geo_unit_id = 0;
  world.buildIndices();

  ScheduleType regular; regular.name = "regular";
  ScheduleType fair_hop; fair_hop.name = "fair_hop"; fair_hop.is_temporary = true;
  TimeSlot fair_slot; fair_slot.name = "fair_slot";
  fair_slot.allowed_activities = {"fair_lodging"};
  for (const auto& act : fair_slot.allowed_activities) {
    int idx = world.getActivityIndex(act);
    if (idx >= 0)
      fair_slot.allowed_activity_indices.push_back(static_cast<int16_t>(idx));
  }
  fair_hop.flat_slots.push_back(fair_slot);

  Config config;
  config.schedule.day_type_cycle = {"day"};
  config.schedule.day_type_names = {"day"};
  config.schedule.schedule_types.push_back(regular);
  config.schedule.schedule_types.push_back(fair_hop);
  config.schedule.default_schedule_type = "regular";
  config.performance.precompute_schedules = false;
  config.resolve(world);
  world.num_day_types = 1;

  // 3-day hop: begin with repeats=2 → 3 total slot executions.
  person.schedule_hop = ScheduleHop::begin(1, -1, 2);
  person.schedule_type_id = 0;

  CalendarEvent event;
  event.calendar_event_id = 1;
  event.start_day = 0;
  event.schedule_type_idx = 1;
  event.compliance_rate = 1.0f;
  event.catchment_rule_id = -1;
  event.hosting_geo_unit_id = 0;

  CalendarEventManager calendar_manager({{event}});
  CalendarEventManager::Snapshot snap;
  snap.active_event[person.id] = 1;
  calendar_manager.restore(std::move(snap));

  static constexpr std::string_view kFixedYaml = R"(
rules:
  lodging_rule:
    strategy: hosting_geo_unit
    venue_type: guest_house
    venue_stability: fixed
activity_rules:
  fair_lodging: lodging_rule
)";
  auto otf_allocator = OnTheFlyVenueAllocator::fromString(kFixedYaml);

  ActivityManager activity_manager(world, config);
  activity_manager.setCalendarEventManager(&calendar_manager);
  activity_manager.setOnTheFlyVenueAllocator(&otf_allocator);

  std::vector<PersonLocation> locations(1);

  activity_manager.setCurrentDay(0);
  activity_manager.assignActivitiesFromSchedule(0, 0, locations);
  const VenueId day0_venue = locations[0].venue_id;

  activity_manager.setCurrentDay(1);
  activity_manager.assignActivitiesFromSchedule(0, 0, locations);
  const VenueId day1_venue = locations[0].venue_id;

  activity_manager.setCurrentDay(2);
  activity_manager.assignActivitiesFromSchedule(0, 0, locations);
  const VenueId day2_venue = locations[0].venue_id;

  CHECK(day0_venue >= 0);
  CHECK(day1_venue == day0_venue);
  CHECK(day2_venue == day0_venue);
}

TEST_CASE("OTF daily-stability: backward re-resolution uses original logical day") {
  // Schedule: 1 flat slot (n=1), OTF daily rule, 5 guest-house venues.
  // k=0 runs on day 0 → v_forward. k=1 runs on day 1 (temp_slot_progress=2).
  // findLastNonNullVenueOnHop on day 1 scans k=0:
  //   hop_start_day = hopStartDay(1,1,1) = 0; logical_day = 0+0 = 0.
  //   With fix:    seed uses day 0 → v_forward.
  //   Without fix: seed uses current_sim_day_=1 → different venue (RED).
  WorldState world;
  world.geo_level_names = {"sgu"};
  GeographicalUnit gu; gu.id = 0; gu.parent_id = -1; gu.level_id = 0;
  world.geo_units.push_back(gu);
  world.venue_type_names = {"guest_house"};
  for (VenueId vid : {10, 11, 12, 13, 14}) {
    Venue v; v.id = vid; v.type_id = 0; v.geo_unit_id = 0;
    world.venues.push_back(v);
  }
  world.activity_names = {"residence", "fair_lodging", "none", "dead", "no_venue"};
  world.schedule_type_names = {"regular", "fair_hop"};

  Person& person = world.people.emplace_back();
  person.id = 0; person.geo_unit_id = 0;
  world.buildIndices();

  ScheduleType regular; regular.name = "regular";
  ScheduleType fair_hop; fair_hop.name = "fair_hop"; fair_hop.is_temporary = true;
  TimeSlot fair_slot; fair_slot.name = "fair_slot";
  fair_slot.allowed_activities = {"fair_lodging"};
  resolveSlotIndices(fair_slot, world);
  fair_hop.flat_slots.push_back(fair_slot);

  Config config;
  config.schedule.day_type_cycle = {"day"};
  config.schedule.day_type_names = {"day"};
  config.schedule.schedule_types.push_back(regular);
  config.schedule.schedule_types.push_back(fair_hop);
  config.schedule.default_schedule_type = "regular";
  config.performance.precompute_schedules = false;
  config.resolve(world);
  world.num_day_types = 1;

  // repeats_remaining=5: 6 cycles total; won't auto-return during test.
  person.schedule_hop = ScheduleHop::begin(1, -1, 5);
  person.schedule_type_id = 0;

  CalendarEvent event;
  event.calendar_event_id = 1; event.start_day = 0;
  event.schedule_type_idx = 1; event.compliance_rate = 1.0f;
  event.catchment_rule_id = -1; event.hosting_geo_unit_id = 0;
  CalendarEventManager calendar_manager({{event}});
  CalendarEventManager::Snapshot snap;
  snap.active_event[person.id] = 1;
  calendar_manager.restore(std::move(snap));

  static constexpr std::string_view kDailyYaml = R"(
rules:
  lodging_rule:
    strategy: hosting_geo_unit
    venue_type: guest_house
    venue_stability: daily
activity_rules:
  fair_lodging: lodging_rule
)";
  auto otf_allocator = OnTheFlyVenueAllocator::fromString(kDailyYaml);

  ActivityManager activity_manager(world, config);
  activity_manager.setCalendarEventManager(&calendar_manager);
  activity_manager.setOnTheFlyVenueAllocator(&otf_allocator);

  std::vector<PersonLocation> locations(1);

  // Forward pass day 0: k=0, logical_day=0 → v_forward.
  activity_manager.setCurrentDay(0);
  activity_manager.assignActivitiesFromSchedule(0, 0, locations);
  const VenueId v_forward = locations[0].venue_id;

  // Forward pass day 1: k=1, logical_day=1; advances temp_slot_progress to 2.
  activity_manager.setCurrentDay(1);
  activity_manager.assignActivitiesFromSchedule(0, 0, locations);

  // Backward scan on day 1, temp_slot_progress=2:
  //   hop_start_day=hopStartDay(1,1,1)=0; k=0: logical_day=0 → v_forward.
  const auto [v_back, s_back] =
      activity_manager.findLastNonNullVenueOnHop(world.people[0]);

  CHECK(v_forward >= 0);
  CHECK(v_back == v_forward);
}
