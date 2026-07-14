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
      "        - property: age\n"
      "          operator: \"<\"\n"
      "          value: 5\n"
      "    - name: friends\n"
      "      enabled: true\n"
      "      network: friendships\n"
      "      establishment: stochastic\n"
      "      span: hop\n");
  REQUIRE(cfg.follows.size() == 2);
  CHECK(cfg.follows[0].name == "infants");
  CHECK(cfg.follows[0].usesCriteria());
  CHECK(cfg.follows[0].span == FollowConfig::Span::Standing);
  REQUIRE(cfg.follows[0].follower.size() == 1);
  CHECK(cfg.follows[0].follower[0].property_path == "age");
  CHECK(cfg.follows[0].follower[0].operator_type == "<");
  CHECK(std::get<int>(cfg.follows[0].follower[0].value) == 5);
  CHECK(cfg.follows[1].name == "friends");
  CHECK(cfg.follows[1].usesNetwork());
  CHECK(cfg.follows[1].span == FollowConfig::Span::Hop);
}

TEST_CASE("eligibility written as a mapping is rejected, not read as empty") {
  // An empty criteria list matches everyone, so a mapping quietly ignored here
  // would enrol the whole world instead of the toddlers the scenario meant.
  CHECK_THROWS(
      parseCE("  follows:\n"
              "    - name: infants\n"
              "      enabled: true\n"
              "      pool_venue_type: household\n"
              "      establishment: criteria\n"
              "      eligibility:\n"
              "        max_age: 5\n"));
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

// One person criterion, built the way a config would spell it.
static SelectionCriterion ageCrit(const std::string& op, double value) {
  SelectionCriterion c;
  c.property_path = "age";
  c.operator_type = op;
  c.value = value;
  return c;
}

// A venue/criteria rule: followers are younger than max_age, hosts are at least
// min_host_age. Age is the axis these cases happen to use; the engine reads
// whatever criteria the rule carries.
static FollowConfig criteriaRule(double max_age, double min_host_age) {
  FollowConfig fc;
  fc.enabled = true;
  fc.pool_venue_type = "household";
  fc.pool_venue_type_id = 0;
  fc.establishment = FollowConfig::Establishment::Criteria;
  fc.span = FollowConfig::Span::Standing;
  fc.follower = {ageCrit("<", max_age)};
  fc.host = {ageCrit(">=", min_host_age)};
  return fc;
}

TEST_CASE("eligibility reads any person attribute, not just age") {
  // Nothing about follow is age-shaped: the same machinery binds on sex. One
  // household of four, where the rule says a female follows the lowest-id male.
  WorldState world = buildHouseholds({{40.f, 41.f, 42.f, 43.f}});
  world.people[0].sex = Sex::FEMALE;
  world.people[1].sex = Sex::MALE;
  world.people[2].sex = Sex::FEMALE;
  world.people[3].sex = Sex::MALE;

  FollowConfig fc;
  fc.enabled = true;
  fc.pool_venue_type = "household";
  fc.pool_venue_type_id = 0;
  fc.establishment = FollowConfig::Establishment::Criteria;
  fc.span = FollowConfig::Span::Standing;
  SelectionCriterion is_female;
  is_female.property_path = "sex";
  is_female.operator_type = "==";
  is_female.value = std::string("female");
  SelectionCriterion is_male;
  is_male.property_path = "sex";
  is_male.operator_type = "==";
  is_male.value = std::string("male");
  fc.follower = {is_female};
  fc.host = {is_male};
  for (auto* side : {&fc.follower, &fc.host})
    for (SelectionCriterion& c : *side) c.resolveOrThrow(world, "test");

  std::unordered_map<PersonId, PersonId> fh;
  std::unordered_set<PersonId> hosts;
  rebuildCriteriaBindings(world, fc, fh, hosts, nullptr, nullptr, {}, {});

  REQUIRE(fh.size() == 2);
  CHECK(fh[0] == 1);  // female 0 -> lowest-id male
  CHECK(fh[2] == 1);  // female 2 -> same host
  CHECK(hosts.count(1));
  CHECK(hosts.count(3) == 0);  // an eligible host nobody picked stays inactive
}

TEST_CASE("several criteria on one side are ANDed") {
  // A follower must be both under 5 and female, so the under-5 male is left
  // alone even though he passes the age bound.
  WorldState world = buildHouseholds({{40.f, 3.f, 4.f}});
  world.people[0].sex = Sex::FEMALE;
  world.people[1].sex = Sex::MALE;
  world.people[2].sex = Sex::FEMALE;

  FollowConfig fc = criteriaRule(5, 18);
  SelectionCriterion is_female;
  is_female.property_path = "sex";
  is_female.operator_type = "==";
  is_female.value = std::string("female");
  fc.follower.push_back(is_female);
  for (SelectionCriterion& c : fc.follower) c.resolveOrThrow(world, "test");

  std::unordered_map<PersonId, PersonId> fh;
  std::unordered_set<PersonId> hosts;
  rebuildCriteriaBindings(world, fc, fh, hosts, nullptr, nullptr, {}, {});

  REQUIRE(fh.size() == 1);
  CHECK(fh.count(2) == 1);  // the under-5 female follows
  CHECK(fh.count(1) == 0);  // the under-5 male does not
}

// Resolve one follow rule against a world and return the error it raised, or
// an empty string when it resolved cleanly.
static std::string resolveError(WorldState& world, const FollowConfig& fc) {
  CoordinatedEncounterConfig cfg;
  cfg.follows.push_back(fc);
  ContactMatrixConfig matrices;
  try {
    cfg.resolve(world, matrices);
  } catch (const std::exception& e) {
    return e.what();
  }
  return "";
}

TEST_CASE("a criterion this world cannot answer is rejected at resolve") {
  // Each of these would evaluate false for every person, so the rule would
  // quietly bind nobody. Resolve has to throw, and name what it choked on.
  WorldState world = buildHouseholds({{40.f, 3.f}});

  // Positive control: the well-formed rule these three are mutations of
  // resolves cleanly, so a throw below is about the criterion and nothing else.
  CHECK(resolveError(world, criteriaRule(5, 18)) == "");

  FollowConfig unknown_path = criteriaRule(5, 18);
  unknown_path.follower[0].property_path = "aeg";
  CHECK(resolveError(world, unknown_path).find("'aeg'") != std::string::npos);

  FollowConfig missing_property = criteriaRule(5, 18);
  SelectionCriterion needs_care;
  needs_care.property_path = "properties.needs_care";
  needs_care.operator_type = "==";
  needs_care.value = std::string("yes");
  missing_property.follower = {needs_care};
  CHECK(resolveError(world, missing_property).find("needs_care") !=
        std::string::npos);

  FollowConfig bad_operator = criteriaRule(5, 18);
  bad_operator.follower = {ageCrit("=<", 5)};
  CHECK(resolveError(world, bad_operator).find("'=<'") != std::string::npos);

  // The host side is resolved too, not just the follower side.
  FollowConfig bad_host = criteriaRule(5, 18);
  bad_host.host[0].property_path = "properties.rank";
  CHECK(resolveError(world, bad_host).find("rank") != std::string::npos);
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

// ===========================================================================
// Mirroring exceptions
// ===========================================================================
//
// The slot decision: given where the host went and what the follower would
// otherwise be doing, does the follower mirror the host or keep its own day?
// Activity and venue-type ids are the world's registry indices; here 1 is the
// activity a rule excepts, 7 the venue type it excepts, and -1 means "no
// activity this slot".

static FollowConfig exceptionRule(std::vector<int16_t> host_activities,
                                  std::vector<uint8_t> host_venue_types,
                                  std::vector<int16_t> follower_activities) {
  FollowConfig fc;
  fc.activity_exception_ids = std::move(host_activities);
  fc.venue_exception_type_ids = std::move(host_venue_types);
  fc.follower_activity_exception_ids = std::move(follower_activities);
  return fc;
}

TEST_CASE("with no exceptions a follower always mirrors its host") {
  FollowConfig fc = exceptionRule({}, {}, {});
  CHECK(follow_detail::mirrorSuppressed(fc, 1, 7, 1) == false);
}

TEST_CASE("an excepted host activity keeps the follower on its own schedule") {
  FollowConfig fc = exceptionRule({1}, {}, {});
  CHECK(follow_detail::mirrorSuppressed(fc, 1, 3, -1) == true);
  CHECK(follow_detail::mirrorSuppressed(fc, 2, 3, -1) == false);
  // It fires on the HOST's activity, never the follower's.
  CHECK(follow_detail::mirrorSuppressed(fc, 2, 3, 1) == false);
}

TEST_CASE(
    "an excepted host venue type keeps the follower on its own schedule") {
  FollowConfig fc = exceptionRule({}, {7}, {});
  CHECK(follow_detail::mirrorSuppressed(fc, 2, 7, -1) == true);
  CHECK(follow_detail::mirrorSuppressed(fc, 2, 3, -1) == false);
}

TEST_CASE("a follower with an excepted activity of its own keeps it") {
  // The case the host-side exceptions cannot reach: the host is somewhere
  // perfectly followable (activity 2, an ordinary venue), and the follower
  // still goes to its own activity 1.
  FollowConfig fc = exceptionRule({}, {}, {1});
  CHECK(follow_detail::mirrorSuppressed(fc, 2, 3, 1) == true);
  // A follower with nothing of its own on (-1) has nothing to protect.
  CHECK(follow_detail::mirrorSuppressed(fc, 2, 3, -1) == false);
  // A follower doing something else still follows.
  CHECK(follow_detail::mirrorSuppressed(fc, 2, 3, 4) == false);
}

TEST_CASE("the three exceptions are ORed") {
  FollowConfig fc = exceptionRule({1}, {7}, {1});
  CHECK(follow_detail::mirrorSuppressed(fc, 1, 3, -1) ==
        true);  // host activity
  CHECK(follow_detail::mirrorSuppressed(fc, 2, 7, -1) == true);  // host venue
  CHECK(follow_detail::mirrorSuppressed(fc, 2, 3, 1) == true);   // own activity
  CHECK(follow_detail::mirrorSuppressed(fc, 2, 3, 4) == false);  // none fire
}
