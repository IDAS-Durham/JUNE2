#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "core/world_state.h"
#include "doctest.h"
#include "test_utils.h"

using namespace june;

TEST_CASE("WorldState basic functionality") {
  WorldState world = TestWorldFactory::createMinimalWorld(10, 5);

  SUBCASE("Building indices") {
    CHECK(world.people.size() == 10);
    CHECK(world.venues.size() == 5);
    CHECK(world.geo_units.size() == 1);

    // Check lookup
    CHECK(world.getPerson(0) != nullptr);
    CHECK(world.getPerson(9) != nullptr);
    CHECK(world.getPerson(10) == nullptr);

    CHECK(world.getVenue(0) != nullptr);
    CHECK(world.getVenue(4) != nullptr);
  }

  SUBCASE("Geographic lookup") {
    auto people_in_city = world.getPeopleInUnit(0);
    CHECK(people_in_city.size() == 10);

    auto city_by_name = world.getPeopleInUnit("city", "TestCity");
    CHECK(city_by_name.size() == 10);
  }
}

TEST_CASE("WorldState networks") {
  WorldState world = TestWorldFactory::createMinimalWorld(2, 0);
  TestWorldFactory::addNetwork(world, "friendship", {{0, 1}});

  SUBCASE("Network partners") {
    auto p0_partners =
        world.getNetworkPartners(*world.getPerson(0), "friendship");
    REQUIRE(p0_partners.size() == 1);
    CHECK(p0_partners[0] == 1);

    auto p1_partners =
        world.getNetworkPartners(*world.getPerson(1), "friendship");
    REQUIRE(p1_partners.size() == 1);
    CHECK(p1_partners[0] == 0);
  }
}

// =============================================================================
// Critical Test 7: Flat array overflow guards
// =============================================================================

TEST_CASE("WorldState flat array overflow returns empty spans") {
  WorldState world = TestWorldFactory::createMinimalWorld(2, 1);

  SUBCASE("activity_meta_start past end of array") {
    Person& p = world.people[0];
    p.activity_meta_start = 99999;
    p.activity_meta_count = 1;
    auto metas = world.getActivityMetas(p);
    CHECK(metas.empty());
  }

  SUBCASE("activity_meta start+count exceeds array size") {
    // Put one real entry then set count too high
    world.activity_meta.push_back({0, 0, 0});
    Person& p = world.people[0];
    p.activity_meta_start = 0;
    p.activity_meta_count = 100;  // Way past end
    auto metas = world.getActivityMetas(p);
    CHECK(metas.empty());
  }

  SUBCASE("network_meta_start past end") {
    Person& p = world.people[0];
    p.network_meta_start = 99999;
    p.network_meta_count = 1;
    auto metas = world.getNetworkMetas(p);
    CHECK(metas.empty());
  }

  SUBCASE("person_properties_start past end") {
    Person& p = world.people[0];
    p.properties_start = 99999;
    p.properties_count = 1;
    auto props = world.getPersonProperties(p);
    CHECK(props.empty());
  }

  SUBCASE("schedule_start past end returns empty") {
    // With the new day_type-based schedule API, an out-of-range person
    // index should return an empty schedule span.
    Person& p = world.people[0];
    p.schedule_computed = false;
    auto sched = world.getSchedule(p, 0);
    CHECK(sched.empty());
  }

  SUBCASE("venue subset_start past end") {
    Venue& v = world.venues[0];
    v.subset_start = 99999;
    v.subset_count = 1;
    auto subs = world.getSubsets(v);
    CHECK(subs.empty());
  }

  SUBCASE("venue properties_start past end") {
    Venue& v = world.venues[0];
    v.properties_start = 99999;
    v.properties_count = 1;
    auto props = world.getVenueProperties(v);
    CHECK(props.empty());
  }

  SUBCASE("zero count always returns empty span") {
    Person& p = world.people[0];
    p.activity_meta_start = 0;
    p.activity_meta_count = 0;
    CHECK(world.getActivityMetas(p).empty());

    p.network_meta_start = 0;
    p.network_meta_count = 0;
    CHECK(world.getNetworkMetas(p).empty());

    p.properties_start = 0;
    p.properties_count = 0;
    CHECK(world.getPersonProperties(p).empty());
  }
}

TEST_CASE("WorldState sentinel ID lookups") {
  WorldState world = TestWorldFactory::createMinimalWorld(3, 2);

  SUBCASE("getPerson(-1) returns nullptr") {
    CHECK(world.getPerson(-1) == nullptr);
  }

  SUBCASE("getVenue(-1) returns nullptr") {
    CHECK(world.getVenue(-1) == nullptr);
  }

  SUBCASE("getGeoUnit(-1) returns nullptr") {
    CHECK(world.getGeoUnit(-1) == nullptr);
  }

  SUBCASE("getPeopleInUnit(-1) returns empty") {
    auto people = world.getPeopleInUnit(-1);
    CHECK(people.empty());
  }

  SUBCASE("getActivityVenues with negative activity index returns empty") {
    auto venues = world.getActivityVenues(world.people[0], (int16_t)-1);
    CHECK(venues.empty());
  }

  SUBCASE("getNetworkPartners with negative type returns empty") {
    auto partners = world.getNetworkPartners(world.people[0], -1);
    CHECK(partners.empty());
  }

  SUBCASE("duplicate person IDs: second overwrites first in index") {
    WorldState w;
    w.venue_type_names = {"office"};
    w.geo_level_names = {"city"};
    auto& p1 = w.people.emplace_back();
    p1.id = 42;
    p1.age = 20.0f;
    auto& p2 = w.people.emplace_back();
    p2.id = 42;  // Duplicate
    p2.age = 30.0f;
    w.buildIndices();
    // The second person (index 1) wins
    const Person* found = w.getPerson(42);
    REQUIRE(found != nullptr);
    CHECK(found->age == doctest::Approx(30.0f));
  }
}
