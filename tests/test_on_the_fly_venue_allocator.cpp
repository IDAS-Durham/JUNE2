#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "activity/activity_manager.h"
#include "activity/on_the_fly_venue_allocator.h"
#include "activity/venue_resolve_context.h"
#include "core/config.h"
#include "core/world_state.h"
#include "epidemiology/calendar_event.h"
#include "test_utils.h"

using namespace june;

static constexpr std::string_view kBaseYaml = R"(
rules:
  fair_venue_rule:
    strategy: hosting_geo_unit
    venue_type: fair
    venue_stability: daily
  fair_accommodation_rule:
    strategy: hosting_geo_unit
    venue_type: guest_house
    venue_stability: fixed
  pub_rule:
    strategy: resident_geo_unit
    venue_type: pub

activity_rules:
  fair_attendance:    fair_venue_rule
  fair_accommodation: fair_accommodation_rule
  pub_visit:          pub_rule
)";

TEST_CASE("hasRule") {
  auto allocator = OnTheFlyVenueAllocator::fromString(kBaseYaml);

  CHECK(allocator.hasRule("fair_attendance"));
  CHECK(allocator.hasRule("fair_accommodation"));
  CHECK(allocator.hasRule("pub_visit"));
  CHECK_FALSE(allocator.hasRule("unknown_activity"));
  CHECK_FALSE(allocator.hasRule(""));
}

struct AllocatorFixture {
  WorldState world;
  VenueResolveContext ctx;

  AllocatorFixture() {
    world.geo_level_names = {"county"};
    world.venue_type_names = {"fair", "guest_house", "pub"};

    GeographicalUnit gu;
    gu.id = 0; gu.level_id = 0; gu.parent_id = -1; gu.name = "TestCounty";
    world.geo_units.push_back(gu);

    // 2 fairs and 1 guest_house in geo_unit 0
    for (int i = 0; i < 2; ++i) {
      Venue v; v.id = i; v.type_id = 0; v.geo_unit_id = 0;
      world.venues.push_back(v);
    }
    Venue gh; gh.id = 2; gh.type_id = 1; gh.geo_unit_id = 0;
    world.venues.push_back(gh);

    // 3 pubs in geo_unit 1
    GeographicalUnit gu2;
    gu2.id = 1; gu2.level_id = 0; gu2.parent_id = -1; gu2.name = "OtherCounty";
    world.geo_units.push_back(gu2);
    for (int i = 0; i < 3; ++i) {
      Venue p; p.id = 10 + i; p.type_id = 2; p.geo_unit_id = 1;
      world.venues.push_back(p);
    }

    world.buildIndices();
  }
};

TEST_CASE_FIXTURE(AllocatorFixture,
                  "resolve hosting_geo_unit returns venues of correct type") {
  auto allocator = OnTheFlyVenueAllocator::fromString(kBaseYaml);
  ctx.hosting_geo_unit_id = 0;
  ctx.resident_geo_unit_id = 1;

  const auto& pool = allocator.resolve("fair_attendance", ctx, world);

  REQUIRE(pool.size() == 2);
  CHECK(pool[0] == 0);
  CHECK(pool[1] == 1);
}

TEST_CASE_FIXTURE(AllocatorFixture,
                  "resolve hosting_geo_unit returns empty when nullopt") {
  auto allocator = OnTheFlyVenueAllocator::fromString(kBaseYaml);
  ctx.hosting_geo_unit_id = std::nullopt;
  ctx.resident_geo_unit_id = 1;

  const auto& pool = allocator.resolve("fair_attendance", ctx, world);

  CHECK(pool.empty());
}

TEST_CASE_FIXTURE(AllocatorFixture,
                  "resolve resident_geo_unit returns venues of correct type") {
  auto allocator = OnTheFlyVenueAllocator::fromString(kBaseYaml);
  ctx.resident_geo_unit_id = 1;

  const auto& pool = allocator.resolve("pub_visit", ctx, world);

  REQUIRE(pool.size() == 3);
  CHECK(pool[0] == 10);
  CHECK(pool[1] == 11);
  CHECK(pool[2] == 12);
}

// =============================================================================
// geo_unit_level: ancestor-based venue resolution
// =============================================================================

struct MultiLevelFixture {
  // Hierarchy: district(10) → village1(11), village2(12)
  // Pubs: 100, 101 in village1; 200 in village2
  WorldState world;
  VenueResolveContext ctx;

  MultiLevelFixture() {
    world.geo_level_names = {"district", "village"};
    world.venue_type_names = {"pub"};

    auto add_gu = [&](GeoUnitId id, uint8_t level, GeoUnitId parent) {
      GeographicalUnit gu;
      gu.id = id; gu.level_id = level; gu.parent_id = parent;
      world.geo_units.push_back(gu);
    };
    add_gu(10, 0, -1);   // district
    add_gu(11, 1, 10);   // village1
    add_gu(12, 1, 10);   // village2

    auto add_pub = [&](VenueId id, GeoUnitId geo) {
      Venue v; v.id = id; v.type_id = 0; v.geo_unit_id = geo;
      world.venues.push_back(v);
    };
    add_pub(100, 11);
    add_pub(101, 11);
    add_pub(200, 12);

    world.buildIndices();
    ctx.resident_geo_unit_id = 11;  // person lives in village1
  }
};

static constexpr std::string_view kDistrictLevelYaml = R"(
rules:
  pub_district_rule:
    strategy: resident_geo_unit
    venue_type: pub
    geo_unit_level: district
activity_rules:
  pub_visit: pub_district_rule
)";

TEST_CASE_FIXTURE(MultiLevelFixture,
                  "resolve resident_geo_unit with geo_unit_level returns full ancestor pool") {
  auto allocator = OnTheFlyVenueAllocator::fromString(kDistrictLevelYaml);
  const auto& pool = allocator.resolve("pub_visit", ctx, world);

  REQUIRE(pool.size() == 3);
  CHECK(pool[0] == 100);
  CHECK(pool[1] == 101);
  CHECK(pool[2] == 200);
}

TEST_CASE_FIXTURE(MultiLevelFixture,
                  "resolve resident_geo_unit with geo_unit_level returns empty when ancestor not found") {
  static constexpr std::string_view kMissingLevelYaml = R"(
rules:
  pub_region_rule:
    strategy: resident_geo_unit
    venue_type: pub
    geo_unit_level: region
activity_rules:
  pub_visit: pub_region_rule
)";
  auto allocator = OnTheFlyVenueAllocator::fromString(kMissingLevelYaml);
  const auto& pool = allocator.resolve("pub_visit", ctx, world);
  CHECK(pool.empty());
}

TEST_CASE_FIXTURE(AllocatorFixture,
                  "resolve returns empty when no matching venues in geo_unit") {
  auto allocator = OnTheFlyVenueAllocator::fromString(kBaseYaml);
  // geo_unit 0 has no pubs
  ctx.resident_geo_unit_id = 0;

  const auto& pool = allocator.resolve("pub_visit", ctx, world);

  CHECK(pool.empty());
}

// =============================================================================
// checkConsistency
// =============================================================================

TEST_CASE_FIXTURE(MultiLevelFixture, "checkConsistency throws for unknown geo_unit_level") {
  auto allocator = OnTheFlyVenueAllocator::fromString(kDistrictLevelYaml);
  CHECK_NOTHROW(allocator.checkConsistency(world));

  static constexpr std::string_view kBadLevelYaml = R"(
rules:
  pub_bad_rule:
    strategy: resident_geo_unit
    venue_type: pub
    geo_unit_level: typo_level
activity_rules:
  pub_visit: pub_bad_rule
)";
  auto bad_allocator = OnTheFlyVenueAllocator::fromString(kBadLevelYaml);
  CHECK_THROWS_AS(bad_allocator.checkConsistency(world), std::runtime_error);
}

// =============================================================================
// Integration: OTF allocator drives venue assignment for a calendar-event hop
// =============================================================================

namespace {

// Helper: resolve allowed_activity_indices from allowed_activities.
void resolveSlotIndicesOtf(TimeSlot& slot, const WorldState& world) {
  slot.allowed_activity_indices.clear();
  for (const auto& act : slot.allowed_activities) {
    int idx = world.getActivityIndex(act);
    if (idx >= 0)
      slot.allowed_activity_indices.push_back(static_cast<int16_t>(idx));
  }
}

}  // namespace

TEST_CASE(
    "OTF allocator resolves venue for calendar-event hop with no pre-baked venues") {
  // World: two fair venues in geo_unit 0; person has no activity_meta for
  // fair_attendance so getActivityVenues returns empty → must go through OTF.
  WorldState world;
  world.geo_level_names = {"county"};
  world.venue_type_names = {"fair"};
  world.activity_names = {"residence", "fair_attendance", "none", "dead",
                           "no_venue"};
  world.schedule_type_names = {"regular", "fair_hop"};

  GeographicalUnit gu;
  gu.id = 0; gu.parent_id = -1; gu.level_id = 0; gu.name = "TestCounty";
  world.geo_units.push_back(gu);

  for (int i = 0; i < 2; ++i) {
    Venue v; v.id = i; v.type_id = 0; v.geo_unit_id = 0;
    world.venues.push_back(v);
  }

  Person& person = world.people.emplace_back();
  person.id = 0; person.geo_unit_id = 0;
  world.buildIndices();

  // Temporary hop schedule with a single fair_attendance slot.
  ScheduleType regular; regular.name = "regular";
  ScheduleType fair_hop; fair_hop.name = "fair_hop"; fair_hop.is_temporary = true;
  TimeSlot fair_slot; fair_slot.name = "fair_slot";
  fair_slot.allowed_activities = {"fair_attendance"};
  resolveSlotIndicesOtf(fair_slot, world);
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

  // Put person on the hop (schedule_type_idx = 1, duration 1 day).
  person.schedule_hop = ScheduleHop::begin(1, -1, 0);
  person.schedule_type_id = 0;

  // CalendarEvent: catchment_rule_id = -1 → restore() does not pre-build a
  // venue pool. hosting_geo_unit_id = 0 is read by getActiveHostingGeoUnit.
  CalendarEvent event;
  event.calendar_event_id = 1;
  event.start_day = 0;
  event.schedule_type_idx = 1;
  event.compliance_rate = 1.0f;
  event.catchment_rule_id = -1;
  event.hosting_geo_unit_id = 0;
  event.venue_type_name = "fair";

  CalendarEventManager calendar_manager({{event}});
  CalendarEventManager::Snapshot snap;
  snap.active_event[person.id] = 1;
  calendar_manager.restore(std::move(snap));

  static constexpr std::string_view kOtfYaml = R"(
rules:
  fair_rule:
    strategy: hosting_geo_unit
    venue_type: fair
activity_rules:
  fair_attendance: fair_rule
)";
  auto otf_allocator = OnTheFlyVenueAllocator::fromString(kOtfYaml);

  ActivityManager activity_manager(world, config);
  activity_manager.setCalendarEventManager(&calendar_manager);
  activity_manager.setOnTheFlyVenueAllocator(&otf_allocator);

  std::vector<PersonLocation> locations(1);
  activity_manager.assignActivitiesFromSchedule(0, 0, locations);

  const int16_t fair_idx =
      static_cast<int16_t>(world.getActivityIndex("fair_attendance"));
  CHECK(locations[0].activity_index == fair_idx);
  CHECK(locations[0].venue_id >= 0);
}
