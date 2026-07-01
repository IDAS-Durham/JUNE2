#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <sstream>

#include "doctest.h"
#include "loaders/catchment_rule_loader.h"

using namespace june;

// =============================================================================
// Cycle 1: parse well-formed CSV -> correct map
// =============================================================================

TEST_CASE("parse returns correct rule->geo_units map for well-formed CSV") {
  std::string csv =
      "catchment_rule_id,geo_unit_id\n"
      "0,1001\n"
      "0,1002\n"
      "1,2001\n";
  std::istringstream input(csv);

  auto rules = CatchmentRuleLoader::parse(input, "test.csv");

  REQUIRE(rules.size() == 2);
  REQUIRE(rules.count(0) == 1);
  REQUIRE(rules.count(1) == 1);
  CHECK(rules.at(0).size() == 2);
  CHECK(rules.at(1).size() == 1);
  CHECK(rules.at(0)[0] == 1001);
  CHECK(rules.at(0)[1] == 1002);
  CHECK(rules.at(1)[0] == 2001);
}

// =============================================================================
// Cycle 2: malformed int throws with source location
// =============================================================================

TEST_CASE("parse throws on a non-integer geo_unit_id") {
  std::string csv =
      "catchment_rule_id,geo_unit_id\n"
      "0,not_an_int\n";
  std::istringstream input(csv);
  CHECK_THROWS_AS(CatchmentRuleLoader::parse(input, "test.csv"),
                  std::runtime_error);
}

TEST_CASE("parse throws on wrong column count") {
  std::string csv = "0\n";  // 1 column
  std::istringstream input(csv);
  CHECK_THROWS_AS(CatchmentRuleLoader::parse(input, "test.csv"),
                  std::runtime_error);
}

// =============================================================================
// Cycle 3: empty file -> empty map (no header, no data)
// =============================================================================

TEST_CASE("parse returns empty map for a header-only CSV") {
  std::string csv = "catchment_rule_id,geo_unit_id\n";
  std::istringstream input(csv);
  auto rules = CatchmentRuleLoader::parse(input, "test.csv");
  CHECK(rules.empty());
}
