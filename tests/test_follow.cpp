// Follow subsystem tests.
//
// Two layers, both on synthetic worlds with anonymous ids (physics/contract,
// never named after real places — see
// feedback_tests_name_physics_not_scenarios):
//   1. Config contract: the follows: list, the follow: sugar, and the schema
//      guards that keep a scenario honest.
//   2. Binding behaviour: the committed-set exclusion that makes several rules
//      coexist — a follower belongs to one rule, a host may recur, no chains.
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/config.h"
#include "core/world_state.h"
#include "doctest.h"
#include "epidemiology/disease.h"
#include "loaders/config_loader.h"
#include "simulation/follow_bindings.h"
#include "test_utils.h"
#include "transmission_fixtures.h"

using namespace june;
using follow_detail::buildFollowPeople;
using follow_detail::FollowPeople;

// The follow helpers take a table of who may follow and host. On one rank the
// table comes straight from the world, with nothing to exchange.
static void rebuildCriteriaBindings(
    WorldState& world, const FollowConfig& fc, uint64_t seed,
    std::unordered_map<PersonId, PersonId>& fh,
    std::unordered_set<PersonId>& hosts,
    const std::unordered_set<PersonId>& follower_excl = {}) {
  FollowPeople people =
      buildFollowPeople(world, fc, false, follower_excl, false);
  follow_detail::rebuildCriteriaBindings(world, fc, seed, people, fh, hosts,
                                         nullptr, nullptr, false);
}

static void enrolFollowHosts(
    WorldState& world, const FollowConfig& fc, const ScheduleConfig& sched,
    uint64_t seed, bool standing, std::unordered_set<PersonId>& hosts,
    std::unordered_map<PersonId, PersonId>& fh, int day,
    const std::unordered_set<PersonId>& follower_excl = {}) {
  FollowPeople people =
      buildFollowPeople(world, fc, true, follower_excl, false);
  follow_detail::enrolFollowHosts(world, fc, sched, seed, standing, people,
                                  hosts, fh, day, nullptr, nullptr, false);
}

// ===========================================================================
// Config contract
// ===========================================================================

// Write a coordinated_encounters YAML body to a temp file and parse it.
static CoordinatedEncounterConfig parseCE(const std::string& body) {
  ScopedTestFiles files{"follow_test_ce"};
  const auto path = files.write("config.yaml",
                                "coordinated_encounters:\n"
                                "  enabled: false\n"
                                "  encounters: []\n" +
                                    body);
  return ConfigLoader::loadCoordinatedEncounters(path.string());
}

TEST_CASE("a one-rule follows list parses its fields") {
  auto cfg = parseCE(
      "  follows:\n"
      "    - enabled: true\n"
      "      pool:\n"
      "        venue_types: [household]\n"
      "      establishment: stochastic\n"
      "      probability: 0.5\n");
  REQUIRE(cfg.follows.size() == 1);
  CHECK(cfg.follows[0].enabled);
  CHECK(cfg.follows[0].pool.venue_types ==
        std::vector<std::string>{"household"});
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
      "      pool:\n"
      "        venue_types: [household]\n"
      "      establishment: criteria\n"
      "      span: standing\n"
      "      eligibility:\n"
      "        - property: age\n"
      "          operator: \"<\"\n"
      "          value: 5\n"
      "    - name: friends\n"
      "      enabled: true\n"
      "      pool:\n"
      "        network: friendships\n"
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
              "      pool:\n"
              "        venue_types: [household]\n"
              "      establishment: criteria\n"
              "      eligibility:\n"
              "        max_age: 5\n"));
}

TEST_CASE("an un-named rule defaults its name to its list index") {
  auto cfg = parseCE(
      "  follows:\n"
      "    - enabled: true\n"
      "      pool:\n"
      "        venue_types: [household]\n"
      "    - enabled: true\n"
      "      pool:\n"
      "        network: friendships\n");
  REQUIRE(cfg.follows.size() == 2);
  CHECK(cfg.follows[0].name == "0");
  CHECK(cfg.follows[1].name == "1");
}

TEST_CASE("the singular follow key is rejected, follows is the only spelling") {
  CHECK_THROWS_WITH(parseCE("  follow:\n"
                            "    enabled: true\n"
                            "    pool:\n"
                            "      venue_types: [household]\n"),
                    doctest::Contains("unknown field 'follow'"));
}

TEST_CASE("duplicate rule names are rejected") {
  CHECK_THROWS(
      parseCE("  follows:\n"
              "    - name: dup\n"
              "      enabled: true\n"
              "      pool:\n"
              "        venue_types: [household]\n"
              "    - name: dup\n"
              "      enabled: true\n"
              "      pool:\n"
              "        network: friendships\n"));
}

TEST_CASE("a rule name with a slash is rejected (would break its shard path)") {
  CHECK_THROWS(
      parseCE("  follows:\n"
              "    - name: a/b\n"
              "      enabled: true\n"
              "      pool:\n"
              "        venue_types: [household]\n"));
}

// ===========================================================================
// Binding behaviour — synthetic households
// ===========================================================================

// A world of venues that people can share. venue_members[v] lists the members
// of venue v (all of type "household", pool venue type id 0); a person is
// assigned to every venue that lists them, as one activity with several venues.
// ages[i] is person i's age.
static WorldState buildSharedVenues(
    const std::vector<float>& ages,
    const std::vector<std::vector<PersonId>>& venue_members) {
  WorldState world;
  world.venue_type_names = {"household"};
  world.activity_names = {"residence"};
  world.geo_level_names = {"city"};
  world.person_property_names = {"age"};
  GeographicalUnit gu;
  gu.id = 0;
  gu.name = "g";
  gu.level_id = 0;
  gu.parent_id = -1;
  world.geo_units.push_back(gu);

  for (size_t i = 0; i < ages.size(); ++i) {
    Person& p = world.people.emplace_back();
    p.id = static_cast<PersonId>(i);
    p.age = ages[i];
    p.geo_unit_id = 0;
    p.activity_meta_start = static_cast<uint32_t>(world.activity_meta.size());
    p.activity_meta_count = 1;
    Person::ActivityMeta meta;
    meta.activity_index = 0;
    meta.venue_start = static_cast<uint32_t>(world.activity_venues.size());
    meta.venue_count = 0;
    for (size_t v = 0; v < venue_members.size(); ++v) {
      for (PersonId m : venue_members[v]) {
        if (m != p.id) continue;
        world.activity_venues.push_back({static_cast<VenueId>(v), 0});
        ++meta.venue_count;
      }
    }
    world.activity_meta.push_back(meta);
  }
  for (size_t v = 0; v < venue_members.size(); ++v) {
    Venue venue;
    venue.id = static_cast<VenueId>(v);
    venue.type_id = 0;
    venue.geo_unit_id = 0;
    venue.parent_id = -1;
    venue.is_residence = true;
    venue.subset_start = static_cast<uint32_t>(world.subsets.size());
    venue.subset_count = 1;
    world.venues.push_back(venue);
    Subset s;
    s.venue_id = venue.id;
    s.subset_index = 0;
    s.subset_type_id = 0;
    s.member_start = static_cast<uint32_t>(world.subset_members.size());
    s.member_count = static_cast<uint32_t>(venue_members[v].size());
    world.subsets.push_back(s);
    world.subset_members.insert(world.subset_members.end(),
                                venue_members[v].begin(),
                                venue_members[v].end());
  }
  world.buildIndices();
  return world;
}

// Households: each inner vector is one household's member ages; ids run
// 0,1,2,... across households in order, and everyone lives in their own
// household venue only.
static WorldState buildHouseholds(
    const std::vector<std::vector<float>>& households) {
  std::vector<float> ages;
  std::vector<std::vector<PersonId>> venues;
  for (const auto& household : households) {
    venues.emplace_back();
    for (float age : household) {
      venues.back().push_back(static_cast<PersonId>(ages.size()));
      ages.push_back(age);
    }
  }
  return buildSharedVenues(ages, venues);
}

// One person criterion, built the way a config would spell it.
static SelectionCriterion ageCrit(const std::string& op, double value) {
  SelectionCriterion c;
  c.property_path = "age";
  c.operator_type = op;
  c.value = value;
  return c;
}

// Resolve a rule's follower and host criteria against the world, as the config
// resolve step does.
static void resolveCriteria(WorldState& world, FollowConfig& fc) {
  for (auto* side : {&fc.follower, &fc.host})
    for (SelectionCriterion& c : *side) c.resolveOrThrow(world, "test");
}

// A venue/criteria rule: followers are younger than max_age, hosts are at least
// min_host_age. Age is the axis these cases happen to use; the engine reads
// whatever criteria the rule carries.
static FollowConfig criteriaRule(double max_age, double min_host_age) {
  FollowConfig fc;
  fc.enabled = true;
  fc.pool.venue_types = {"household"};
  fc.pool_venue_type_ids = {0};
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
  fc.pool.venue_types = {"household"};
  fc.pool_venue_type_ids = {0};
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
  resolveCriteria(world, fc);

  std::unordered_map<PersonId, PersonId> fh;
  std::unordered_set<PersonId> hosts;
  rebuildCriteriaBindings(world, fc, 1, fh, hosts);

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
  rebuildCriteriaBindings(world, fc, 1, fh, hosts);

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
  rebuildCriteriaBindings(world, fc, 1, fh, hosts);

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
  rebuildCriteriaBindings(world, fc, 1, fh, hosts);

  REQUIRE(fh.count(2));
  CHECK(fh[2] == 0);  // lowest-id adult, order-independent
}

TEST_CASE("host_eligibility keeps a second child from being a host") {
  // A lone child with no adult in the household has no eligible host.
  WorldState world = buildHouseholds({{3.f, 4.f}});
  FollowConfig fc = criteriaRule(5, 18);
  std::unordered_map<PersonId, PersonId> fh;
  std::unordered_set<PersonId> hosts;
  rebuildCriteriaBindings(world, fc, 1, fh, hosts);
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
  rebuildCriteriaBindings(world, fc, 1, fh, hosts, follower_excl);
  CHECK(fh.count(1) == 0);  // yielded to the earlier rule
  CHECK(fh.count(3) == 1);  // the other child still binds here
}

TEST_CASE("a person following under another rule can still host here") {
  // The only adult (id 0) already follows under an earlier rule. Child 1 still
  // binds to them here; the chain is settled once every rule has run.
  WorldState world = buildHouseholds({{40.f, 3.f}});
  FollowConfig fc = criteriaRule(5, 18);
  std::unordered_map<PersonId, PersonId> fh;
  std::unordered_set<PersonId> hosts;
  rebuildCriteriaBindings(world, fc, 1, fh, hosts, /*follower_excl=*/{0});
  CHECK(fh[1] == 0);
}

TEST_CASE(
    "stochastic enrol: every eligible person can host, followers or not") {
  // probability 1 => every person rolls as a host. Nobody is shut out of
  // hosting because they also follow someone.
  WorldState world = buildHouseholds({{30.f, 31.f, 32.f}});
  FollowConfig fc;
  fc.enabled = true;
  fc.pool.venue_types = {"household"};
  fc.pool_venue_type_ids = {0};
  fc.establishment = FollowConfig::Establishment::Stochastic;
  fc.span = FollowConfig::Span::Standing;
  fc.probability = 1.0;

  ScheduleConfig sched;  // unused for a standing span
  std::unordered_map<PersonId, PersonId> fh;
  std::unordered_set<PersonId> hosts;
  enrolFollowHosts(world, fc, sched, /*seed=*/123, /*standing=*/true, hosts, fh,
                   /*current_day=*/0);
  CHECK(hosts.size() == 3);
  CHECK(!fh.empty());
}

TEST_CASE("a follower of a follower ends up at the end of the chain") {
  std::unordered_map<PersonId, PersonId> parent = {{1, 2}, {2, 3}};
  CHECK(follow_detail::followRoot(parent, 1) == 3);
  CHECK(follow_detail::followRoot(parent, 3) == 3);  // follows nobody
}

TEST_CASE(
    "a bond that would close a loop is refused, the same way every time") {
  // A follows B and B follows A. The edge by the lower host id is taken first;
  // the one that would close the loop is refused, whatever order they arrive
  // in.
  std::vector<follow_detail::FollowEdge> edges = {{0, 5, 9}, {0, 9, 5}};
  for (int flip = 0; flip < 2; ++flip) {
    std::vector<follow_detail::FollowEdge> refused;
    auto parent = follow_detail::flattenFollowEdges(edges, refused);
    REQUIRE(refused.size() == 1);
    CHECK(refused[0].follower == 5);  // 5 would follow 9, who follows 5
    CHECK(parent.count(9) == 1);
    CHECK(parent.count(5) == 0);
    std::reverse(edges.begin(), edges.end());
  }
}

TEST_CASE("a long loop is cut once and the rest of the chain is kept") {
  // 1->2->3->4->1: the last edge to be added closes the loop.
  std::vector<follow_detail::FollowEdge> refused;
  auto parent = follow_detail::flattenFollowEdges(
      {{0, 1, 2}, {0, 2, 3}, {0, 3, 4}, {0, 4, 1}}, refused);
  CHECK(refused.size() == 1);
  CHECK(parent.size() == 3);
  for (int p = 1; p <= 4; ++p) {  // every chain ends
    PersonId r = follow_detail::followRoot(parent, p);
    CHECK(parent.count(r) == 0);
  }
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

TEST_CASE("an unresolvable host venue type is not silently followable") {
  // An untypeable host venue must never reach the gate: answering "not
  // excepted" would fail open and mirror the follower into a venue whose type
  // nobody knows. Every rank types every Venue in the world, so a 255 here
  // names no Venue at all — a defect, not an outcome.
  FollowConfig fc = exceptionRule({}, {7}, {});
  CHECK_THROWS_AS(
      follow_detail::mirrorSuppressed(fc, 2, kUnknownVenueTypeId, -1),
      std::runtime_error);
}

TEST_CASE("the three exceptions are ORed") {
  FollowConfig fc = exceptionRule({1}, {7}, {1});
  CHECK(follow_detail::mirrorSuppressed(fc, 1, 3, -1) ==
        true);  // host activity
  CHECK(follow_detail::mirrorSuppressed(fc, 2, 7, -1) == true);  // host venue
  CHECK(follow_detail::mirrorSuppressed(fc, 2, 3, 1) == true);   // own activity
  CHECK(follow_detail::mirrorSuppressed(fc, 2, 3, 4) == false);  // none fire
}

// ===========================================================================
// Policy against the mirror
// ===========================================================================
//
// The mirror moves the follower to the host's venue, so the policy question is
// about that venue, not the one the follower is scheduled to. A venue-gated
// policy (close_pubs: leisure at pub) asked about the follower's own grocery
// would wave them through, straight into the closed pub.

namespace {

constexpr int16_t kResidenceActivity = 0;
constexpr int16_t kLeisureActivity = 1;

constexpr VenueId kHomeVenue = 0;
constexpr VenueId kPubVenue = 1;
constexpr VenueId kGroceryVenue = 2;

constexpr uint8_t kPubVenueType = 1;
constexpr uint8_t kGroceryVenueType = 2;

// One person: residence at home, leisure reachable at the grocery. Venue types
// household=0, pub=1, grocery=2, so a gate naming "pub" bites on kPubVenue
// alone.
WorldState buildGateWorld() {
  WorldState world;
  world.venue_type_names = {"household", "pub", "grocery"};
  world.activity_names = {"residence", "leisure"};
  world.geo_level_names = {"city"};
  world.person_property_names = {"age"};
  GeographicalUnit gu;
  gu.id = 0;
  gu.name = "g";
  gu.level_id = 0;
  gu.parent_id = -1;
  world.geo_units.push_back(gu);

  for (int i = 0; i < 3; ++i) {
    Venue v;
    v.id = i;
    v.type_id = static_cast<uint8_t>(i);
    v.geo_unit_id = 0;
    v.parent_id = -1;
    v.is_residence = (i == kHomeVenue);
    world.venues.push_back(v);
  }

  Person& follower = world.people.emplace_back();
  follower.id = 0;
  follower.age = 30.f;
  follower.geo_unit_id = 0;
  follower.activity_meta_start = 0;
  follower.activity_meta_count = 2;
  for (VenueId venue : {kHomeVenue, kGroceryVenue}) {
    Person::ActivityMeta meta;
    meta.activity_index =
        (venue == kHomeVenue) ? kResidenceActivity : kLeisureActivity;
    meta.venue_start = static_cast<uint32_t>(world.activity_venues.size());
    meta.venue_count = 1;
    world.activity_venues.push_back({venue, /*subset=*/0});
    world.activity_meta.push_back(meta);
  }
  // Normally set by precomputePolicyApplicability.
  follower.applicable_temporal_policy_mask = 1;

  world.buildIndices();
  return world;
}

// A policy sending the named activities home, gated to the named venue types
// (empty = ungated, the activity-only behaviour that predates the gate).
void addClosurePolicy(PolicyManager& policy_manager, WorldState& world,
                      const std::vector<std::string>& activities,
                      const std::vector<std::string>& venue_types) {
  TemporalPolicy policy;
  policy.name = "close_pubs";
  policy.window.start_time = 0.0;
  policy.window.end_time = 10.0;
  policy.action.override_activities.insert(activities.begin(),
                                           activities.end());
  policy.action.override_venue_types.insert(venue_types.begin(),
                                            venue_types.end());
  policy.action.replacement_activity = "residence";
  policy.action.compliance_rate = 1.0;
  policy.resolve(world);
  policy_manager.addTemporalPolicy(policy);
}

}  // namespace

TEST_CASE("a venue-gated policy on the host's venue stops the mirror") {
  WorldState world = buildGateWorld();
  PolicyManager policy_manager(world);
  addClosurePolicy(policy_manager, world, {"leisure"}, {"pub"});

  // REGRESSION: asked about the follower's own grocery the gate does not
  // match, and the follower is mirrored into the closed pub.
  CHECK(follow_detail::policySuppressesMirror(&policy_manager, world.people[0],
                                              kLeisureActivity, kPubVenueType,
                                              kPubVenue, 5.0) == true);

  // Same policy, host somewhere the gate does not name: the mirror stands.
  CHECK(follow_detail::policySuppressesMirror(
            &policy_manager, world.people[0], kLeisureActivity,
            kGroceryVenueType, kGroceryVenue, 5.0) == false);
}

TEST_CASE("the mirror gate keys on the host's venue type, not the follower's") {
  WorldState world = buildGateWorld();
  PolicyManager policy_manager(world);

  // The follower's own leisure venue is the grocery. Gating on the grocery
  // must not fire: the key is where the mirror would put them, and a
  // cross-rank host's venue cannot be typed on this rank at all, so the type
  // that travelled with the host is the only one the gate may use.
  addClosurePolicy(policy_manager, world, {"leisure"}, {"grocery"});
  CHECK(follow_detail::policySuppressesMirror(&policy_manager, world.people[0],
                                              kLeisureActivity, kPubVenueType,
                                              kPubVenue, 5.0) == false);
}

TEST_CASE("an activity-only policy stops the mirror wherever the host is") {
  WorldState world = buildGateWorld();
  PolicyManager policy_manager(world);
  addClosurePolicy(policy_manager, world, {"leisure"}, {});

  CHECK(follow_detail::policySuppressesMirror(&policy_manager, world.people[0],
                                              kLeisureActivity, kPubVenueType,
                                              kPubVenue, 5.0) == true);
  CHECK(follow_detail::policySuppressesMirror(
            &policy_manager, world.people[0], kLeisureActivity,
            kGroceryVenueType, kGroceryVenue, 5.0) == true);
}

TEST_CASE("with no policy manager the mirror always stands") {
  WorldState world = buildGateWorld();
  CHECK(follow_detail::policySuppressesMirror(nullptr, world.people[0],
                                              kLeisureActivity, kPubVenueType,
                                              kPubVenue, 5.0) == false);
}

namespace {

// A Disease whose only stage is "sick", lasting 100 days, so the symptom the
// policy triggers on is the same at every time the test asks about.
Disease buildAlwaysSickDisease() {
  TransmissionParams transmission;
  transmission.mode = InfectiousnessMode::STAGE_DRIVEN;
  auto curve = std::make_shared<ConstantCurve>(1.0);
  transmission.stage_curves["sick"] = curve;
  transmission.symptom_id_curves = {nullptr, curve};

  std::vector<SymptomTag> symptom_tags = {{"healthy", -1, 0}, {"sick", 1, 1}};
  DiseaseStageSettings stage_settings;
  stage_settings.recovered_stages = {"healthy"};

  TrajectoryDefinition trajectory;
  trajectory.selection_key = "general";
  trajectory.severity = 1.0;
  trajectory.stages.push_back({"sick", {"constant", {{"value", 100.0}}}});

  return Disease("TestDisease", symptom_tags, stage_settings, {trajectory}, {},
                 transmission);
}

}  // namespace

TEST_CASE("the mirror gate pins nobody at the host's venue") {
  // A hopped follower, sick, under a freeze_in_place policy. The gate must
  // answer "a policy outranks the mirror" and stop there. The venue it is
  // asked about is the host's, so a pin written here anchors the follower at
  // the host's venue for the whole freeze — and every later slot
  // ActivityManager finds that entry and teleports them back to it, long
  // after this slot's mirror was declined.
  WorldState world = buildGateWorld();
  Disease disease = buildAlwaysSickDisease();

  PolicyManager policy_manager(world);
  SymptomPolicy freeze_when_sick;
  freeze_when_sick.name = "freeze_traveller_when_sick";
  freeze_when_sick.trigger_symptoms = {"sick"};
  freeze_when_sick.action.override_activities = {"leisure"};
  freeze_when_sick.action.replacement_activity = "residence";
  freeze_when_sick.action.compliance_rate = 1.0;
  // Set before adding: resolve() only fills this in from a named schedule.
  freeze_when_sick.action.replacement_schedule_idx = 0;
  policy_manager.addSymptomPolicy(freeze_when_sick);
  policy_manager.resolveAll(disease);

  Person& follower = world.people[0];
  follower.infection = std::make_unique<Infection>(&disease, 0.0, &follower, 42,
                                                   kNoTransmissionContext,
                                                   nullptr, "household", 0);
  follower.applicable_symptom_policy_mask = 1;
  constexpr int16_t kHoppedSchedule = 3;
  constexpr int16_t kReturnSchedule = 1;
  follower.schedule_hop = ScheduleHop::begin(kHoppedSchedule, kReturnSchedule);

  CHECK(follow_detail::policySuppressesMirror(&policy_manager, follower,
                                              kLeisureActivity, kPubVenueType,
                                              kPubVenue, 5.0) == true);

  CHECK(policy_manager.getFrozenStates().empty());
  CHECK(follower.schedule_hop.hopped_schedule_id == kHoppedSchedule);
  CHECK(follower.schedule_hop.return_schedule_id == kReturnSchedule);
}

// ===========================================================================
// Invite distribution parameters
// ===========================================================================

static void loadWithInviteDistribution(const std::string& distribution) {
  ScopedTestFiles files{"invite_distribution_ce"};
  const auto path = files.write("config.yaml",
                                "coordinated_encounters:\n"
                                "  enabled: true\n"
                                "  encounters:\n"
                                "    - name: meet\n"
                                "      network: friends\n"
                                "      trigger_slots: [leisure]\n"
                                "      allowed_venues: [pub]\n"
                                "      proposal_probability: 0.5\n"
                                "      invite_distribution: " +
                                    distribution + "\n");
  ConfigLoader::loadCoordinatedEncounters(path.string());
}

TEST_CASE("invite distribution parameters are checked when the file loads") {
  CHECK_NOTHROW(loadWithInviteDistribution("{type: poisson, mean: 1.5}"));
  CHECK_NOTHROW(loadWithInviteDistribution("{type: binomial, p: 1.0}"));
  CHECK_NOTHROW(loadWithInviteDistribution("{type: fixed, count: 0}"));
  CHECK_THROWS_WITH(loadWithInviteDistribution("{type: poisson, mean: 501}"),
                    doctest::Contains("'meet' mean must be in [0, 500"));
  CHECK_THROWS_WITH(loadWithInviteDistribution("{type: poisson, mean: -1}"),
                    doctest::Contains("mean must be"));
  CHECK_THROWS_WITH(loadWithInviteDistribution("{type: binomial, p: 1.2}"),
                    doctest::Contains("p must be in [0, 1]"));
  CHECK_THROWS_WITH(loadWithInviteDistribution("{type: fixed, count: -2}"),
                    doctest::Contains("count must be 0 or more"));
}

// ===========================================================================
// Pools: venue choice, eligibility on every establishment, caps
// ===========================================================================

static FollowConfig venueRule(FollowPool::VenueChoice choice) {
  FollowConfig fc;
  fc.enabled = true;
  fc.name = "rule";
  fc.pool.venue_types = {"household"};
  fc.pool.venue_choice = choice;
  fc.pool_venue_type_ids = {0};
  return fc;
}

TEST_CASE("venue choice all feeds the pool from every shared venue") {
  // Person 0 is in venues 0 and 1; person 2 is in both.
  WorldState world = buildSharedVenues({30.f, 30.f, 30.f, 30.f, 30.f},
                                       {{0, 1, 2}, {0, 2, 3}, {4}});
  FollowConfig fc = venueRule(FollowPool::VenueChoice::All);
  auto venues = follow_detail::poolVenues(world, fc, 1, world.people[0], 0);
  CHECK(venues == std::vector<VenueId>{0, 1});
}

TEST_CASE("venue choice lowest_id feeds the pool from the lowest-id venue") {
  WorldState world =
      buildSharedVenues({30.f, 30.f, 30.f, 30.f}, {{0, 1}, {0, 2}, {0, 3}});
  FollowConfig fc = venueRule(FollowPool::VenueChoice::LowestId);
  auto venues = follow_detail::poolVenues(world, fc, 1, world.people[0], 0);
  CHECK(venues == std::vector<VenueId>{0});
}

TEST_CASE("venue choice random holds its draw and uses every venue overall") {
  // 200 people, each in venues 0 and 1.
  std::vector<PersonId> everyone;
  for (PersonId i = 0; i < 200; ++i) everyone.push_back(i);
  WorldState world =
      buildSharedVenues(std::vector<float>(200, 30.f), {everyone, everyone});
  FollowConfig fc = venueRule(FollowPool::VenueChoice::Random);

  int in_zero = 0, changed_by_day = 0;
  for (const Person& p : world.people) {
    auto a = follow_detail::poolVenues(world, fc, 7, p, 0);
    auto again = follow_detail::poolVenues(world, fc, 7, p, 0);
    auto other_day = follow_detail::poolVenues(world, fc, 7, p, 3);
    REQUIRE(a.size() == 1);
    CHECK(a == again);  // the same key gives the same venue
    if (a[0] == 0) ++in_zero;
    if (a != other_day) ++changed_by_day;
  }
  CHECK(in_zero > 60);  // both venues get drawn, roughly half each
  CHECK(in_zero < 140);
  CHECK(changed_by_day > 0);  // a different key day re-draws
}

TEST_CASE("a criteria follower takes the lowest eligible host across venues") {
  // Follower 0 (child) shares venue 0 with adult 5 and venue 1 with adult 2.
  WorldState world =
      buildSharedVenues({5.f, 5.f, 40.f, 5.f, 5.f, 40.f}, {{0, 5}, {0, 2}});
  FollowConfig fc = venueRule(FollowPool::VenueChoice::All);
  fc.establishment = FollowConfig::Establishment::Criteria;
  fc.span = FollowConfig::Span::Standing;
  fc.follower = {ageCrit("<", 18)};
  fc.host = {ageCrit(">=", 18)};
  resolveCriteria(world, fc);

  std::unordered_map<PersonId, PersonId> fh;
  std::unordered_set<PersonId> hosts;
  rebuildCriteriaBindings(world, fc, 1, fh, hosts);
  REQUIRE(fh.count(0) == 1);
  CHECK(fh[0] == 2);
}

TEST_CASE(
    "a person who is follower and host eligible never follows themselves") {
  // Everyone may follow and everyone may host: each follows the lowest other.
  WorldState world = buildSharedVenues({30.f, 30.f, 30.f}, {{0, 1, 2}});
  FollowConfig fc = venueRule(FollowPool::VenueChoice::All);
  fc.establishment = FollowConfig::Establishment::Criteria;
  fc.span = FollowConfig::Span::Standing;
  std::unordered_map<PersonId, PersonId> fh;
  std::unordered_set<PersonId> hosts;
  rebuildCriteriaBindings(world, fc, 1, fh, hosts);
  CHECK(fh[0] == 1);
  CHECK(fh[1] == 0);
  CHECK(fh[2] == 0);
}

TEST_CASE("a stochastic rule binds only people who meet eligibility") {
  // 2,000 households of two adults and two children. Followers and hosts must
  // both be adults; a child is never either.
  std::vector<float> ages;
  std::vector<std::vector<PersonId>> venues;
  for (int h = 0; h < 2000; ++h) {
    PersonId b = static_cast<PersonId>(ages.size());
    for (float a : {40.f, 38.f, 9.f, 5.f}) ages.push_back(a);
    venues.push_back({b, b + 1, b + 2, b + 3});
  }
  WorldState world = buildSharedVenues(ages, venues);
  FollowConfig fc = venueRule(FollowPool::VenueChoice::All);
  fc.establishment = FollowConfig::Establishment::Stochastic;
  fc.span = FollowConfig::Span::Standing;
  fc.probability = 0.3;
  fc.follower = {ageCrit(">=", 18)};
  fc.host = {ageCrit(">=", 18)};
  resolveCriteria(world, fc);

  ScheduleConfig sched;
  std::unordered_map<PersonId, PersonId> fh;
  std::unordered_set<PersonId> hosts;
  enrolFollowHosts(world, fc, sched, 123, true, hosts, fh, 0);

  CHECK(!fh.empty());
  for (const auto& [f, h] : fh) {
    CHECK(world.people[world.person_index.at(f)].age >= 18.f);
    CHECK(world.people[world.person_index.at(h)].age >= 18.f);
  }
  for (PersonId h : hosts)
    CHECK(world.people[world.person_index.at(h)].age >= 18.f);
}

TEST_CASE("a cap keeps the same followers whatever order people are stored") {
  // One adult and six children; the rule caps the adult at two followers.
  auto bind = [](bool reversed) {
    WorldState world = buildSharedVenues({40.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f},
                                         {{0, 1, 2, 3, 4, 5, 6}});
    if (reversed) {
      std::reverse(world.people.begin(), world.people.end());
      world.buildIndices();
    }
    FollowConfig fc = venueRule(FollowPool::VenueChoice::All);
    fc.establishment = FollowConfig::Establishment::Criteria;
    fc.span = FollowConfig::Span::Standing;
    fc.follower = {ageCrit("<", 18)};
    fc.host = {ageCrit(">=", 18)};
    fc.max_followers = 2;
    resolveCriteria(world, fc);
    std::unordered_map<PersonId, PersonId> fh;
    std::unordered_set<PersonId> hosts;
    rebuildCriteriaBindings(world, fc, 9, fh, hosts);
    return fh;
  };
  auto forward = bind(false);
  CHECK(forward.size() == 2);  // no more than the cap
  CHECK(forward == bind(true));
}

TEST_CASE("a stochastic cap holds per host and survives a reordered world") {
  auto bind = [](bool reversed) {
    WorldState world = buildSharedVenues(
        {40.f, 41.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f}, {{0, 1, 2, 3, 4, 5, 6, 7}});
    if (reversed) {
      std::reverse(world.people.begin(), world.people.end());
      world.buildIndices();
    }
    FollowConfig fc = venueRule(FollowPool::VenueChoice::All);
    fc.establishment = FollowConfig::Establishment::Stochastic;
    fc.span = FollowConfig::Span::Standing;
    fc.probability = 0.5;
    fc.follower = {ageCrit("<", 18)};
    fc.host = {ageCrit(">=", 18)};
    fc.max_followers = 2;
    resolveCriteria(world, fc);
    ScheduleConfig sched;
    std::unordered_map<PersonId, PersonId> fh;
    std::unordered_set<PersonId> hosts;
    enrolFollowHosts(world, fc, sched, 11, true, hosts, fh, 0);
    std::unordered_map<PersonId, int> per_host;
    for (const auto& [f, h] : fh) ++per_host[h];
    for (const auto& [h, n] : per_host) CHECK(n <= 2);
    return fh;
  };
  CHECK(bind(false) == bind(true));
}

TEST_CASE("a pool venue that no rank owns is refused, not skipped") {
  // Person 1 is also assigned to venue 7, which exists in the venue registry
  // but is listed by no rank.
  WorldState world = buildHouseholds({{40.f, 3.f}});
  world.activity_venues.push_back({7, 0});
  world.activity_meta[world.people[1].activity_meta_start].venue_count = 2;
  world.setGlobalVenueType(7, 0);
  FollowConfig fc = criteriaRule(5, 18);
  std::unordered_map<PersonId, PersonId> fh;
  std::unordered_set<PersonId> hosts;
  CHECK_THROWS_WITH(rebuildCriteriaBindings(world, fc, 1, fh, hosts),
                    doctest::Contains("no owning rank"));
}

TEST_CASE("pool settings that cannot mean anything are rejected at resolve") {
  WorldState world = buildHouseholds({{40.f, 3.f}});

  FollowConfig neither = criteriaRule(5, 18);
  neither.pool.venue_types.clear();
  CHECK(resolveError(world, neither).find("exactly one source") !=
        std::string::npos);

  FollowConfig both = criteriaRule(5, 18);
  both.pool.network = "friendships";
  CHECK(resolveError(world, both).find("exactly one source") !=
        std::string::npos);

  FollowConfig unknown_type = criteriaRule(5, 18);
  unknown_type.pool.venue_types = {"nowhere"};
  CHECK(resolveError(world, unknown_type).find("'nowhere'") !=
        std::string::npos);

  FollowConfig network_choice;
  network_choice.enabled = true;
  network_choice.name = "net";
  network_choice.pool.network = "friendships";
  network_choice.pool.venue_choice = FollowPool::VenueChoice::All;
  CHECK(resolveError(world, network_choice).find("venue_choice") !=
        std::string::npos);

  FollowConfig zero_cap = criteriaRule(5, 18);
  zero_cap.max_followers = 0;
  CHECK(resolveError(world, zero_cap).find("at least 1") != std::string::npos);
}

TEST_CASE("the pool block rejects keys it does not know and bad venue_choice") {
  CHECK_THROWS(
      parseCE("  follows:\n"
              "    - name: a\n"
              "      pool:\n"
              "        venue_type: household\n"));  // singular: a typo
  CHECK_THROWS(
      parseCE("  follows:\n"
              "    - name: a\n"
              "      pool:\n"
              "        venue_types: [household]\n"
              "        venue_choice: first\n"));
  CHECK_THROWS(
      parseCE("  follows:\n"
              "    - name: a\n"
              "      pool_venue_type: household\n"));  // the old key is gone
  auto cfg = parseCE(
      "  follows:\n"
      "    - name: a\n"
      "      pool:\n"
      "        venue_types: [household]\n"
      "        venue_choice: lowest_id\n"
      "      max_followers: 3\n");
  CHECK(cfg.follows[0].pool.venue_choice == FollowPool::VenueChoice::LowestId);
  CHECK(cfg.follows[0].max_followers.value() == 3);
}

// ===========================================================================
// Network pools take host_eligibility and a cap
// ===========================================================================

static FollowConfig networkRule(WorldState& world, double max_follower_age) {
  FollowConfig fc;
  fc.enabled = true;
  fc.name = "net";
  fc.pool.network = "friends";
  fc.network_idx = world.getNetworkTypeIndex("friends");
  fc.establishment = FollowConfig::Establishment::Criteria;
  fc.span = FollowConfig::Span::Standing;
  fc.follower = {ageCrit("<", max_follower_age)};
  fc.host = {ageCrit(">=", 18)};
  resolveCriteria(world, fc);
  return fc;
}

TEST_CASE("a network partner who is not host eligible is never the host") {
  // Child 0 has a child partner (1) and an adult partner (2).
  WorldState world = buildHouseholds({{5.f, 8.f, 40.f}});
  TestWorldFactory::addNetwork(world, "friends", {{0, 1}, {0, 2}});
  FollowConfig fc = networkRule(world, 10);
  CHECK(resolveError(world, fc) ==
        "");  // host_eligibility on a network resolves

  std::unordered_map<PersonId, PersonId> fh;
  std::unordered_set<PersonId> hosts;
  rebuildCriteriaBindings(world, fc, 1, fh, hosts);
  CHECK(fh[0] == 2);        // the adult, though the child has the lower id
  CHECK(fh.count(1) == 0);  // child 1's only partner is a child
}

TEST_CASE("a cap limits a network host to its lowest-hash followers") {
  WorldState world = buildHouseholds({{40.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f}});
  TestWorldFactory::addNetwork(
      world, "friends", {{0, 1}, {0, 2}, {0, 3}, {0, 4}, {0, 5}, {0, 6}});
  FollowConfig fc = networkRule(world, 10);
  fc.max_followers = 2;
  CHECK(resolveError(world, fc) == "");

  std::unordered_map<PersonId, PersonId> fh;
  std::unordered_set<PersonId> hosts;
  rebuildCriteriaBindings(world, fc, 5, fh, hosts);
  CHECK(fh.size() == 2);
  for (const auto& [f, h] : fh) CHECK(h == 0);
}
