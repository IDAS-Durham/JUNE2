#define DOCTEST_CONFIG_IMPLEMENT
#include "core/types.h"
#include "core/world_state.h"
#include "doctest.h"
#include "loaders/domain_loader_internals.h"
#include "test_utils.h"

using namespace june;
using namespace june::detail;

int main(int argc, char** argv) {
  doctest::Context context;
  context.applyCommandLine(argc, argv);
  return context.run();
}

// =============================================================================
// matchMembershipRowToFlatIndex: matches a membership-metadata side-table
// row to its flat index in WorldState::activity_venues for one person.
//
// Property under test: when a person holds several Subsets at the same
// Venue (e.g. two Feast accommodation memberships sharing one guest house),
// matching on (venue_id, subset_index) resolves each to its own distinct
// flat index, whereas venue_id alone cannot.
// =============================================================================

namespace {

// Give `person` one ActivityMeta whose candidate venues are exactly
// `venues`, appended to world.activity_venues. Returns the meta's
// venue_start so tests can assert against absolute flat indices.
uint32_t giveOneActivityWithVenues(
    WorldState& world, Person& person,
    const std::vector<std::pair<VenueId, SubsetIndex>>& venues) {
  person.activity_meta_start = static_cast<uint32_t>(world.activity_meta.size());
  person.activity_meta_count = 1;

  Person::ActivityMeta meta;
  meta.activity_index = 0;
  meta.venue_start = static_cast<uint32_t>(world.activity_venues.size());
  meta.venue_count = static_cast<uint16_t>(venues.size());
  world.activity_meta.push_back(meta);

  for (const auto& venue : venues) world.activity_venues.push_back(venue);
  return meta.venue_start;
}

}  // namespace

TEST_CASE("single subset at a venue matches by venue_id alone") {
  WorldState world = TestWorldFactory::createMinimalWorld();
  Person& person = world.people[0];
  uint32_t venue_start = giveOneActivityWithVenues(world, person, {{5, 2}});

  uint32_t result =
      matchMembershipRowToFlatIndex(world, person, /*venue_id=*/5, nullptr);

  CHECK(result == venue_start);
}

TEST_CASE("single subset at a venue matches when subset_index also given") {
  WorldState world = TestWorldFactory::createMinimalWorld();
  Person& person = world.people[0];
  uint32_t venue_start = giveOneActivityWithVenues(world, person, {{5, 2}});

  SubsetIndex subset_index = 2;
  uint32_t result = matchMembershipRowToFlatIndex(world, person,
                                                   /*venue_id=*/5, &subset_index);

  CHECK(result == venue_start);
}

TEST_CASE(
    "two subsets at the same venue are disambiguated by subset_index") {
  WorldState world = TestWorldFactory::createMinimalWorld();
  Person& person = world.people[0];
  uint32_t venue_start =
      giveOneActivityWithVenues(world, person, {{5, 0}, {5, 1}});

  SubsetIndex first = 0;
  SubsetIndex second = 1;
  uint32_t first_result =
      matchMembershipRowToFlatIndex(world, person, /*venue_id=*/5, &first);
  uint32_t second_result =
      matchMembershipRowToFlatIndex(world, person, /*venue_id=*/5, &second);

  CHECK(first_result == venue_start);
  CHECK(second_result == venue_start + 1);
  CHECK(first_result != second_result);
}

TEST_CASE(
    "two subsets at the same venue collide on venue_id alone "
    "(documents the pre-fix fallback behaviour for old worlds)") {
  WorldState world = TestWorldFactory::createMinimalWorld();
  Person& person = world.people[0];
  uint32_t venue_start =
      giveOneActivityWithVenues(world, person, {{5, 0}, {5, 1}});

  uint32_t result =
      matchMembershipRowToFlatIndex(world, person, /*venue_id=*/5, nullptr);

  CHECK(result == venue_start);
}

TEST_CASE("no candidate venue matches returns the absent sentinel") {
  WorldState world = TestWorldFactory::createMinimalWorld();
  Person& person = world.people[0];
  giveOneActivityWithVenues(world, person, {{5, 0}});

  uint32_t result =
      matchMembershipRowToFlatIndex(world, person, /*venue_id=*/9, nullptr);

  CHECK(result == kAbsentFlatIndex);
}
