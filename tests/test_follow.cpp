// Follow subsystem tests.
//
// Two layers, both on synthetic worlds with anonymous ids (physics/contract,
// never named after real places — see
// feedback_tests_name_physics_not_scenarios):
//   1. Config contract: the follows: list, the follow: sugar, and the schema
//      guards that keep a scenario honest.
//   2. Binding behaviour: the committed-set exclusion that makes several rules
//      coexist — a follower belongs to one rule, a host may recur, no chains.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/config.h"
#include "core/world_state.h"
#include "doctest.h"
#include "loaders/config_loader.h"
#include "simulation/follow_bindings.h"

using namespace june;
using follow_detail::enrolFollowHosts;
using follow_detail::rebuildCriteriaBindings;

// ===========================================================================
// Config contract
// ===========================================================================

// Write a coordinated_encounters YAML body to a temp file and parse it.
static CoordinatedEncounterConfig parseCE(const std::string& body) {
  static int counter = 0;
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("follow_test_ce_" + std::to_string(counter++) + ".yaml");
  std::ofstream(path) << "coordinated_encounters:\n"
                         "  enabled: false\n"
                         "  encounters: []\n"
                      << body;
  auto cfg = ConfigLoader::loadCoordinatedEncounters(path.string());
  std::filesystem::remove(path);
  return cfg;
}

TEST_CASE("follow: singular block is sugar for a one-element list") {
  auto cfg = parseCE(
      "  follow:\n"
      "    enabled: true\n"
      "    pool_venue_type: household\n"
      "    establishment: stochastic\n"
      "    probability: 0.5\n");
  REQUIRE(cfg.follows.size() == 1);
  CHECK(cfg.follows[0].enabled);
  CHECK(cfg.follows[0].pool_venue_type == "household");
  CHECK(cfg.follows[0].establishment ==
        FollowConfig::Establishment::Stochastic);
  CHECK(cfg.follows[0].probability == doctest::Approx(0.5));
  // An un-named rule takes its list position as its name.
  CHECK(cfg.follows[0].name == "0");
}

TEST_CASE("follows: parses a list of rules in order with their fields") {
  auto cfg = parseCE(
      "  follows:\n"
      "    - name: infants\n"
      "      enabled: true\n"
      "      pool_venue_type: household\n"
      "      establishment: criteria\n"
      "      span: standing\n"
      "      eligibility:\n"
      "        max_age: 5\n"
      "    - name: friends\n"
      "      enabled: true\n"
      "      network: friendships\n"
      "      establishment: stochastic\n"
      "      span: hop\n");
  REQUIRE(cfg.follows.size() == 2);
  CHECK(cfg.follows[0].name == "infants");
  CHECK(cfg.follows[0].usesCriteria());
  CHECK(cfg.follows[0].span == FollowConfig::Span::Standing);
  CHECK(cfg.follows[0].follower.max_age == doctest::Approx(5));
  CHECK(cfg.follows[1].name == "friends");
  CHECK(cfg.follows[1].usesNetwork());
  CHECK(cfg.follows[1].span == FollowConfig::Span::Hop);
}

TEST_CASE("an un-named rule defaults its name to its list index") {
  auto cfg = parseCE(
      "  follows:\n"
      "    - enabled: true\n"
      "      pool_venue_type: household\n"
      "    - enabled: true\n"
      "      network: friendships\n");
  REQUIRE(cfg.follows.size() == 2);
  CHECK(cfg.follows[0].name == "0");
  CHECK(cfg.follows[1].name == "1");
}

TEST_CASE("setting both follow and follows is rejected") {
  CHECK_THROWS(
      parseCE("  follow:\n"
              "    enabled: true\n"
              "    pool_venue_type: household\n"
              "  follows:\n"
              "    - enabled: true\n"
              "      network: friendships\n"));
}

TEST_CASE("duplicate rule names are rejected") {
  CHECK_THROWS(
      parseCE("  follows:\n"
              "    - name: dup\n"
              "      enabled: true\n"
              "      pool_venue_type: household\n"
              "    - name: dup\n"
              "      enabled: true\n"
              "      network: friendships\n"));
}

TEST_CASE("a rule name with a slash is rejected (would break its shard path)") {
  CHECK_THROWS(
      parseCE("  follows:\n"
              "    - name: a/b\n"
              "      enabled: true\n"
              "      pool_venue_type: household\n"));
}

// ===========================================================================
// Binding behaviour — synthetic households
// ===========================================================================

// Build a world of households. Each inner vector is one household's member
// ages; ids are assigned 0,1,2,... across households in order. Every person
// gets a residence activity at their household venue, and the venue carries one
// subset holding all its members (the pool the follow logic reads).
static WorldState buildHouseholds(
    const std::vector<std::vector<float>>& households) {
  WorldState world;
  world.venue_type_names = {"household"};  // pool_venue_type_id 0
  world.activity_names = {"residence"};
  world.geo_level_names = {"city"};
  world.person_property_names = {"age"};
  GeographicalUnit gu;
  gu.id = 0;
  gu.name = "g";
  gu.level_id = 0;
  gu.parent_id = -1;
  world.geo_units.push_back(gu);

  PersonId next_id = 0;
  for (const auto& ages : households) {
    VenueId vid = static_cast<VenueId>(world.venues.size());
    std::vector<PersonId> members;
    for (float age : ages) {
      Person& p = world.people.emplace_back();
      p.id = next_id++;
      p.age = age;
      p.geo_unit_id = 0;
      // one residence activity pointing at this household venue
      p.activity_meta_start = static_cast<uint32_t>(world.activity_meta.size());
      p.activity_meta_count = 1;
      Person::ActivityMeta meta;
      meta.activity_index = 0;
      meta.venue_start = static_cast<uint32_t>(world.activity_venues.size());
      meta.venue_count = 1;
      world.activity_venues.push_back({vid, /*subset=*/0});
      world.activity_meta.push_back(meta);
      members.push_back(p.id);
    }
    Venue v;
    v.id = vid;
    v.type_id = 0;
    v.geo_unit_id = 0;
    v.parent_id = -1;
    v.is_residence = true;
    v.subset_start = static_cast<uint32_t>(world.subsets.size());
    v.subset_count = 1;
    world.venues.push_back(v);
    Subset s;
    s.venue_id = vid;
    s.subset_index = 0;
    s.subset_type_id = 0;
    s.member_start = static_cast<uint32_t>(world.subset_members.size());
    s.member_count = static_cast<uint32_t>(members.size());
    world.subsets.push_back(s);
    world.subset_members.insert(world.subset_members.end(), members.begin(),
                                members.end());
  }
  world.buildIndices();
  return world;
}

// A venue/criteria rule: followers are age < max_age, hosts are age >= min_age.
static FollowConfig criteriaRule(double max_age, double min_host_age) {
  FollowConfig fc;
  fc.enabled = true;
  fc.pool_venue_type = "household";
  fc.pool_venue_type_id = 0;
  fc.establishment = FollowConfig::Establishment::Criteria;
  fc.span = FollowConfig::Span::Standing;
  fc.follower.max_age = max_age;
  fc.host.min_age = min_host_age;
  return fc;
}

TEST_CASE(
    "criteria binds each under-age follower to the lowest-id adult host") {
  // Two households, each an adult + a child.
  WorldState world = buildHouseholds({{40.f, 3.f}, {35.f, 2.f}});
  FollowConfig fc = criteriaRule(5, 18);
  std::unordered_map<PersonId, PersonId> fh;
  std::unordered_set<PersonId> hosts;
  rebuildCriteriaBindings(world, fc, fh, hosts, nullptr, nullptr, {}, {});

  REQUIRE(fh.size() == 2);
  CHECK(fh[1] == 0);  // child 1 -> adult 0
  CHECK(fh[3] == 2);  // child 3 -> adult 2
  CHECK(hosts.count(0));
  CHECK(hosts.count(2));
  // Invariant: nobody is both a host and a follower.
  for (const auto& [f, h] : fh) CHECK(hosts.count(f) == 0);
}

TEST_CASE("min-id tiebreak: a child follows the lowest-id eligible adult") {
  // One household with two adults (ids 0,1) and a child (id 2).
  WorldState world = buildHouseholds({{40.f, 41.f, 3.f}});
  FollowConfig fc = criteriaRule(5, 18);
  std::unordered_map<PersonId, PersonId> fh;
  std::unordered_set<PersonId> hosts;
  rebuildCriteriaBindings(world, fc, fh, hosts, nullptr, nullptr, {}, {});

  REQUIRE(fh.count(2));
  CHECK(fh[2] == 0);  // lowest-id adult, order-independent
}

TEST_CASE("host_eligibility keeps a second child from being a host") {
  // A lone child with no adult in the household has no eligible host.
  WorldState world = buildHouseholds({{3.f, 4.f}});
  FollowConfig fc = criteriaRule(5, 18);
  std::unordered_map<PersonId, PersonId> fh;
  std::unordered_set<PersonId> hosts;
  rebuildCriteriaBindings(world, fc, fh, hosts, nullptr, nullptr, {}, {});
  CHECK(fh.empty());  // neither child can host the other
  CHECK(hosts.empty());
}

TEST_CASE("follower_excl: a follower claimed by an earlier rule is skipped") {
  WorldState world = buildHouseholds({{40.f, 3.f}, {35.f, 2.f}});
  FollowConfig fc = criteriaRule(5, 18);
  std::unordered_map<PersonId, PersonId> fh;
  std::unordered_set<PersonId> hosts;
  // child 1 already follows under an earlier rule.
  std::unordered_set<PersonId> follower_excl = {1};
  rebuildCriteriaBindings(world, fc, fh, hosts, nullptr, nullptr, follower_excl,
                          {});
  CHECK(fh.count(1) == 0);  // yielded to the earlier rule
  CHECK(fh.count(3) == 1);  // the other child still binds here
}

TEST_CASE("host_excl: a person following elsewhere cannot host (no chain)") {
  WorldState world = buildHouseholds({{40.f, 3.f}});
  FollowConfig fc = criteriaRule(5, 18);
  std::unordered_map<PersonId, PersonId> fh;
  std::unordered_set<PersonId> hosts;
  // the only adult (id 0) already follows under an earlier rule, so it may not
  // host here — otherwise child 1 -> adult 0 -> (adult's host) would chain.
  std::unordered_set<PersonId> host_excl = {0};
  rebuildCriteriaBindings(world, fc, fh, hosts, nullptr, nullptr, {},
                          host_excl);
  CHECK(fh.empty());
  CHECK(hosts.empty());
}

TEST_CASE(
    "stochastic enrol: hosts and followers stay disjoint, host_excl held") {
  // probability 1 => every eligible person rolls as a host, so with the
  // committed-exclusivity rule none of them can also be a follower.
  WorldState world = buildHouseholds({{30.f, 31.f, 32.f}});
  FollowConfig fc;
  fc.enabled = true;
  fc.pool_venue_type = "household";
  fc.pool_venue_type_id = 0;
  fc.establishment = FollowConfig::Establishment::Stochastic;
  fc.span = FollowConfig::Span::Standing;
  fc.probability = 1.0;

  ScheduleConfig sched;  // unused for a standing span
  std::unordered_map<PersonId, PersonId> fh;
  std::unordered_set<PersonId> hosts;
  // person 0 already follows elsewhere, so it must not be enrolled as a host.
  std::unordered_set<PersonId> host_excl = {0};
  enrolFollowHosts(world, fc, sched, /*seed=*/123, /*standing=*/true, hosts, fh,
                   /*current_day=*/0, nullptr, nullptr, {}, host_excl);

  CHECK(hosts.count(0) == 0);    // barred by host_excl
  for (const auto& [f, h] : fh)  // no host is also a follower
    CHECK(hosts.count(f) == 0);
}
