#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "activity/on_the_fly_venue_allocator.h"
#include "activity/venue_resolve_context.h"
#include "core/world_state.h"
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

TEST_CASE_FIXTURE(AllocatorFixture,
                  "resolve returns empty when no matching venues in geo_unit") {
  auto allocator = OnTheFlyVenueAllocator::fromString(kBaseYaml);
  // geo_unit 0 has no pubs
  ctx.resident_geo_unit_id = 0;

  const auto& pool = allocator.resolve("pub_visit", ctx, world);

  CHECK(pool.empty());
}
