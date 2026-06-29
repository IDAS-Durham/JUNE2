#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <sstream>

#include "core/types.h"
#include "core/world_state.h"
#include "doctest.h"
#include "epidemiology/calendar_event.h"
#include "loaders/calendar_event_loader.h"
#include "test_utils.h"

using namespace june;

namespace {

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

}  // namespace

// =============================================================================
// Loader happy path
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
// Loader errors / out-of-window rows
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
// Loader sets the candidate_venue_builder (but no venue_selector) for catchment
// events
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

TEST_CASE("loader leaves venue_selector null for catchment event") {
  // The loader deliberately installs no venue_selector: the catchment path
  // relies on the manager's default hash-select. Re-adding a loader selector
  // would duplicate that default, so guard against it here.
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

  CHECK(event.venue_selector == nullptr);
}
