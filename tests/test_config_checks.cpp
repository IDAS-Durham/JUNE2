#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/config.h"
#include "core/world_state.h"
#include "utils/config_checks.h"

using namespace june;

TEST_CASE("checkConfigConsistency - activity mask width boundary") {
  Config config;
  WorldState world;

  world.activity_names.assign(128, "activity");
  CHECK_NOTHROW(checkConfigConsistency(config, world));

  world.activity_names.assign(129, "activity");
  CHECK_THROWS_AS(checkConfigConsistency(config, world), std::runtime_error);
}

TEST_CASE("checkConfigConsistency - venue-type mask width boundary") {
  Config config;
  WorldState world;

  world.venue_type_names.assign(128, "venue_type");
  CHECK_NOTHROW(checkConfigConsistency(config, world));

  world.venue_type_names.assign(129, "venue_type");
  CHECK_THROWS_AS(checkConfigConsistency(config, world), std::runtime_error);
}
