#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>

#include "core/config.h"
#include "core/world_state.h"
#include "doctest.h"
#include "epidemiology/disease.h"
#include "epidemiology/epidemiology.h"
#include "epidemiology/observed_metrics.h"
#include "epidemiology/policy.h"
#include "loaders/policy_loader.h"
#include "transmission_fixtures.h"
#include "venue_scope_fixtures.h"

using namespace june;

namespace {

// Message of the exception a call throws, or "" if it does not throw.
std::string thrown(const std::function<void()>& f) {
  try {
    f();
  } catch (const std::exception& e) {
    return e.what();
  }
  return "";
}

bool has(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

// healthy(0) -> mild(1) -> severe(2) -> dead(3), mild 2 days, severe 1 day.
Disease makeMetricDisease() {
  TransmissionParams trans;
  trans.mode = InfectiousnessMode::STAGE_DRIVEN;
  auto curve = std::make_shared<ConstantCurve>(1.0);
  trans.symptom_id_curves = {nullptr, curve, curve, curve};

  std::vector<SymptomTag> tags = {
      {"healthy", -1, 0}, {"mild", 1, 1}, {"severe", 2, 2}, {"dead", 3, 3}};
  DiseaseStageSettings settings;
  settings.fatality_stages = {"dead"};

  TrajectoryDefinition td;
  td.selection_key = "general";
  td.severity = 1.0;
  td.stages.push_back({"mild", {"constant", {{"value", 2.0}}}});
  td.stages.push_back({"severe", {"constant", {{"value", 1.0}}}});
  td.stages.push_back({"dead", {"constant", {{"value", 100.0}}}});
  return Disease("MetricDisease", tags, settings, {td}, {}, trans);
}

ObservedMetricConfig cfg(const std::string& source, const std::string& scope,
                         const std::string& activity = "",
                         const std::string& stage = "") {
  return {source, scope, activity, stage};
}

}  // namespace

TEST_CASE("ObservedMetrics - declaration rejects each malformed case") {
  ObservedMetrics m;
  CHECK(has(thrown([&] { m.declare("a", cfg("tests", "world")); }),
            "unknown source 'tests'"));
  CHECK(has(thrown([&] { m.declare("a", cfg("death", "region")); }),
            "unknown scope 'region'"));
  CHECK(has(thrown([&] { m.declare("a", cfg("death", "venue")); }),
            "requires an 'activity'"));
  CHECK(has(thrown([&] { m.declare("a", cfg("death", "parent_venue")); }),
            "scope 'parent_venue' requires an 'activity'"));
  CHECK(has(thrown([&] { m.declare("a", cfg("death", "region")); }),
            "Known scopes: world, venue, parent_venue"));
  CHECK(has(thrown([&] { m.declare("a", cfg("death", "world", "school")); }),
            "only valid with scope 'venue' or 'parent_venue'"));
  CHECK(has(thrown([&] { m.declare("a", cfg("death", "world", "school")); }),
            "'activity' is only valid with scope 'venue'"));
  CHECK(has(thrown([&] { m.declare("a", cfg("stage", "world")); }),
            "requires a 'stage'"));
  CHECK(has(thrown([&] { m.declare("a", cfg("death", "world", "", "mild")); }),
            "'stage' is not valid with source 'death'"));
  CHECK(has(thrown([&] { m.declare("a/b", cfg("death", "world")); }),
            "must not contain '/'"));
  CHECK_FALSE(m.anyDeclared());
}

TEST_CASE("ObservedMetrics - resolve rejects unknown names") {
  WorldState world;
  world.activity_names = {"residence", "school"};
  const Disease disease = makeMetricDisease();

  SUBCASE("unknown stage") {
    ObservedMetrics m;
    m.declare("a", cfg("stage", "world", "", "icu"));
    const std::string msg = thrown([&] { m.resolve(world, disease); });
    CHECK(has(msg, "unknown stage 'icu'"));
    CHECK(has(msg, "severe"));
  }
  SUBCASE("unknown activity") {
    ObservedMetrics m;
    m.declare("a", cfg("infection", "venue", "casino"));
    CHECK(has(thrown([&] { m.resolve(world, disease); }),
              "unknown activity 'casino'"));
  }
  SUBCASE("venue and parent venue scopes resolve against a known activity") {
    ObservedMetrics m;
    m.declare("a", cfg("infection", "venue", "school"));
    m.declare("b", cfg("death", "parent_venue", "school"));
    CHECK_NOTHROW(m.resolve(world, disease));
    CHECK(m.scope("a") == MetricScope::Venue);
    CHECK(m.scope("b") == MetricScope::ParentVenue);
    CHECK(m.activityOf("b") == "school");
  }
  SUBCASE("a valid world metric resolves and can be required") {
    ObservedMetrics m;
    m.declare("a", cfg("stage_admission", "world", "", "severe"));
    CHECK_NOTHROW(m.resolve(world, disease));
    CHECK(has(thrown([&] { m.require("nope"); }), "no metric named 'nope'"));
    CHECK_NOTHROW(m.require("a"));
    CHECK(m.anyActive());
  }
  SUBCASE("require before resolve is refused") {
    ObservedMetrics m;
    m.declare("a", cfg("death", "world"));
    CHECK(has(thrown([&] { m.require("a"); }), "before the metrics were"));
  }
}

TEST_CASE("PolicyLoader - observed_metrics alone is parsed, not ignored") {
  const auto path =
      std::filesystem::temp_directory_path() / "june_observed_metrics.yaml";
  {
    std::ofstream out(path);
    out << "observed_metrics:\n"
           "  deaths_so_far: {source: death, scope: world}\n"
           "  severe_now: {source: stage, scope: world, stage: severe}\n";
  }
  WorldState world;
  PolicyManager pm(world);
  PolicyLoader::loadPolicies(pm, path.string(), "2020-02-03");
  CHECK(pm.observedMetrics().anyDeclared());
  CHECK_FALSE(pm.observedMetrics().anyActive());

  {
    std::ofstream out(path);
    out << "observed_metrics:\n"
           "  bad: {source: death, scope: world, colour: red}\n";
  }
  WorldState world2;
  PolicyManager pm2(world2);
  CHECK(has(thrown([&] {
              PolicyLoader::loadPolicies(pm2, path.string(), "2020-02-03");
            }),
            "unknown field 'colour'"));
  std::filesystem::remove(path);
}

TEST_CASE("Epidemiology - cumulative and current metric counts") {
  WorldState world;
  world.activity_names = {"residence"};
  const Disease disease = makeMetricDisease();
  Epidemiology epi(world, &disease);

  constexpr int kPeople = 5;
  for (int i = 0; i < kPeople; ++i) {
    auto& p = world.people.emplace_back();
    p.id = i;
    p.age = 30.0f;
    p.geo_unit_id = -1;
  }
  world.buildIndices();

  ObservedMetrics m;
  m.declare("deaths", cfg("death", "world"));
  m.declare("entered_severe", cfg("stage_admission", "world", "", "severe"));
  m.declare("now_mild", cfg("stage", "world", "", "mild"));
  m.declare("infected", cfg("infection", "world"));
  m.declare("unread", cfg("stage", "world", "", "severe"));
  m.resolve(world, disease);

  SUBCASE("a metric nothing requires is never evaluated") {
    epi.setObservedMetrics(&m);  // as Simulator does only when anyActive()
    for (int i = 0; i < kPeople; ++i) {
      world.people[i].infection = std::make_unique<Infection>(
          &disease, 0.0, &world.people[i], 100 + i, kNoTransmissionContext,
          nullptr, "office", 0);
      epi.trackInfection(i);
    }
    CHECK_FALSE(m.anyActive());
    epi.updateInfectionStates(10.0, {});
    epi.reduceObservedMetrics();
    CHECK(m.value("deaths") == 0);
    CHECK(m.value("infected") == 0);
  }

  SUBCASE("deaths and admissions follow the trajectories, in any slicing") {
    for (const char* n : {"deaths", "entered_severe", "now_mild", "infected"})
      m.require(n);
    epi.setObservedMetrics(&m);

    // Person i is infected at t = -i/2: mild for 2 days, severe for 1, then
    // dead at t = 3 - i/2, so deaths land at 3, 2.5, 2, 1.5 and 1.
    for (int i = 0; i < kPeople; ++i) {
      world.people[i].infection = std::make_unique<Infection>(
          &disease, -0.5 * i, &world.people[i], 100 + i, kNoTransmissionContext,
          nullptr, "office", 0);
      epi.trackInfection(i);
    }

    int stats_deaths = 0;
    int64_t last_deaths = 0;
    int64_t last_severe = 0;
    for (double t = 0.5; t <= 12.0; t += 0.5) {
      stats_deaths += epi.updateInfectionStates(t, {}).deaths;
      epi.reduceObservedMetrics();
      // Cumulative counters only rise.
      CHECK(m.value("deaths") >= last_deaths);
      CHECK(m.value("entered_severe") >= last_severe);
      last_deaths = m.value("deaths");
      last_severe = m.value("entered_severe");
      if (t == 1.5) {
        // Persons 3 and 4 are dead; only person 0 is still mild; persons 1
        // to 4 have each entered severe.
        CHECK(m.value("deaths") == 2);
        CHECK(m.value("infected") == 3);
        CHECK(m.value("now_mild") == 1);
        CHECK(m.value("entered_severe") == 4);
      }
    }

    int is_dead = 0;
    for (const auto& p : world.people) is_dead += p.is_dead ? 1 : 0;
    CHECK(is_dead == kPeople);
    CHECK(stats_deaths == kPeople);
    CHECK(m.value("deaths") == kPeople);
    CHECK(m.value("entered_severe") == kPeople);
    CHECK(m.value("infected") == 0);
    CHECK(m.value("now_mild") == 0);
    CHECK(m.localCount("deaths") == 0);  // folded into the total
  }
}

TEST_CASE(
    "Epidemiology - stage_admission counts the stage placed in at infection") {
  WorldState world;
  world.activity_names = {"residence"};
  const Disease disease = makeMetricDisease();
  Epidemiology epi(world, &disease);

  constexpr int kPeople = 4;
  for (int i = 0; i < kPeople; ++i) {
    auto& p = world.people.emplace_back();
    p.id = i;
    p.age = 30.0f;
    p.geo_unit_id = -1;
  }
  world.buildIndices();

  ObservedMetrics m;
  m.declare("entered_mild", cfg("stage_admission", "world", "", "mild"));
  m.declare("entered_severe", cfg("stage_admission", "world", "", "severe"));
  m.declare("now_mild", cfg("stage", "world", "", "mild"));
  m.resolve(world, disease);
  for (const char* n : {"entered_mild", "entered_severe", "now_mild"})
    m.require(n);
  epi.setObservedMetrics(&m);

  auto infect = [&](int i, double t, const std::string& start = "") {
    world.people[i].infection = std::make_unique<Infection>(
        &disease, t, &world.people[i], 100 + i, kNoTransmissionContext, nullptr,
        "office", 0, "", start);
    epi.trackInfection(i);
  };
  auto day_boundary = [&](double t) {
    epi.updateInfectionStates(t, {});
    epi.reduceObservedMetrics();
  };

  SUBCASE("every newly infected person is an admission to the starting stage") {
    infect(0, 0.0);
    infect(1, 0.0);
    day_boundary(0.5);
    // Both are still in their starting stage, and both entered it.
    CHECK(m.value("entered_mild") == 2);
    CHECK(m.value("now_mild") == 2);
    CHECK(m.value("entered_severe") == 0);

    // Counted once: later checks do not count the starting stage again.
    day_boundary(1.0);
    day_boundary(2.5);
    CHECK(m.value("entered_mild") == 2);
    CHECK(m.value("entered_severe") == 2);
  }

  SUBCASE("a person is counted at their own infection, not before") {
    infect(0, 0.0);
    day_boundary(0.5);
    CHECK(m.value("entered_mild") == 1);
    infect(1, 1.0);
    day_boundary(1.0);
    CHECK(m.value("entered_mild") == 2);
    day_boundary(1.5);
    CHECK(m.value("entered_mild") == 2);
  }

  SUBCASE("totals do not depend on how the days are sliced") {
    infect(0, 0.0);
    infect(1, 0.0);
    infect(2, 0.0);
    // One check long after infection replays the whole trajectory at once.
    day_boundary(12.0);
    CHECK(m.value("entered_mild") == 3);
    CHECK(m.value("entered_severe") == 3);
    CHECK(m.value("now_mild") == 0);
  }

  SUBCASE(
      "a trajectory that starts in a later stage is admitted to that stage") {
    infect(0, 0.0, "severe");
    day_boundary(0.5);
    CHECK(m.value("entered_severe") == 1);
    CHECK(m.value("entered_mild") == 0);
  }
}

TEST_CASE("ObservedMetrics - stored totals restore exactly or throw") {
  WorldState world;
  const Disease disease = makeMetricDisease();

  auto make = [&](bool with_extra) {
    auto m = std::make_unique<ObservedMetrics>();
    m->declare("deaths", cfg("death", "world"));
    m->declare("severe", cfg("stage_admission", "world", "", "severe"));
    m->declare("other", cfg("infection", "world"));
    m->resolve(world, disease);
    m->require("deaths");
    m->require("severe");
    if (with_extra) m->require("other");
    return m;
  };

  auto before = make(false);
  const Person nobody{};
  before->onDeath(nobody);
  before->onDeath(nobody);
  before->onTransition(nobody, 1, 2);
  before->reduce();  // single process: local becomes the total
  const auto stored = before->snapshot();
  REQUIRE(stored.size() == 2);

  auto after = make(false);
  after->restoreSnapshot(stored);
  CHECK(after->value("deaths") == 2);
  CHECK(after->value("severe") == 1);
  // Counting continues from the restored total, not from zero.
  after->onDeath(nobody);
  after->reduce();
  CHECK(after->value("deaths") == 3);

  // A different active set is refused in both directions.
  auto more = make(true);
  CHECK(has(thrown([&] { more->restoreSnapshot(stored); }),
            "cannot be rebuilt or dropped"));
  auto fewer = make(false);
  auto three = make(true).get()->snapshot();
  CHECK(has(thrown([&] { fewer->restoreSnapshot(three); }),
            "cannot be rebuilt or dropped"));
}

// ---------------------------------------------------------------------------
// Metrics counted per venue and per parent venue
// ---------------------------------------------------------------------------

namespace {

using namespace june::venue_scope_fixture;

// A metric set over the fixture world, all four active.
struct UnitMetrics {
  WorldState world = makeWorld();
  Disease disease = makeMetricDisease();
  ObservedMetrics m;

  UnitMetrics() {
    m.declare("class_now", cfg("infection", "venue", "primary_activity"));
    m.declare("school_now",
              cfg("infection", "parent_venue", "primary_activity"));
    m.declare("class_deaths", cfg("death", "venue", "primary_activity"));
    m.declare("school_deaths",
              cfg("death", "parent_venue", "primary_activity"));
    m.declare("pub_now", cfg("infection", "venue", "leisure"));
    m.declare("world_total", cfg("death", "world"));
    m.resolve(world, disease);
    for (const char* n : {"class_now", "school_now", "class_deaths",
                          "school_deaths", "pub_now"})
      m.require(n);
    m.buildVenueParents(world);
  }

  const Person& person(PersonId id) const { return world.people[id]; }
};

}  // namespace

TEST_CASE("Metrics per venue - a person counts at every registered venue") {
  UnitMetrics u;
  for (PersonId id = 0; id < kPeople; ++id) u.m.onPresent(u.person(id), 1);
  u.m.reduce();

  // Class A: pupil A and the person registered to both classrooms.
  CHECK(u.m.unitValue("class_now", kClassA) == 2);
  CHECK(u.m.unitValue("class_now", kClassB) == 2);
  CHECK(u.m.unitValue("class_now", kClassC) == 1);
  // The metric counts every registered venue of the activity whatever its
  // type: the pub worker's primary venue is a pub, the staff member's the
  // school.
  CHECK(u.m.unitValue("class_now", kPub) == 1);
  CHECK(u.m.unitValue("class_now", kSchool) == 1);
  // Only the named activity counts, so nobody is counted at the household.
  CHECK(u.m.unitValue("class_now", kHome) == 0);

  // A person with two leisure venues counts at both of them.
  CHECK(u.m.unitValue("pub_now", kPub) == 2);
  CHECK(u.m.unitValue("pub_now", kPubTwo) == 1);
  const auto& seen = u.m.changedUnits("pub_now");
  CHECK(seen == std::vector<VenueId>{kPub, kPubTwo});
}

TEST_CASE("Metrics per parent venue - one unit per parent, once per person") {
  UnitMetrics u;
  for (PersonId id = 0; id < kPeople; ++id) u.m.onPresent(u.person(id), 1);
  u.m.reduce();

  // School 3 holds class A, class B and the school itself. The person
  // registered to both classrooms counts once, not twice: pupil A, pupil B,
  // that person and the staff member registered to the school are four.
  CHECK(u.m.unitValue("school_now", kSchool) == 4);
  // A venue with no parent is its own unit.
  CHECK(u.m.unitValue("school_now", kPub) == 1);
  // The other school holds class C only.
  CHECK(u.m.unitValue("school_now", kSchoolTwo) == 1);
  // Nothing is counted at a child of a parent.
  CHECK(u.m.unitValue("school_now", kClassA) == 0);
  CHECK(u.m.unitValue("school_now", kClassC) == 0);
  CHECK(u.m.changedUnits("school_now") ==
        std::vector<VenueId>{kPub, kSchool, kSchoolTwo});
}

TEST_CASE(
    "Metrics per unit - a dead person is counted where they were registered") {
  UnitMetrics u;
  Person& dead = u.world.people[kTwoClasses];
  dead.is_dead = true;  // as the engine sets it just before the death hook
  u.m.onDeath(dead);
  u.m.reduce();

  CHECK(u.m.unitValue("class_deaths", kClassA) == 1);
  CHECK(u.m.unitValue("class_deaths", kClassB) == 1);
  CHECK(u.m.unitValue("school_deaths", kSchool) ==
        1);  // once for two classrooms

  // Cumulative: another death adds, and a day with none leaves the total.
  u.m.onDeath(u.person(kPupilA));
  u.m.reduce();
  CHECK(u.m.unitValue("class_deaths", kClassA) == 2);
  CHECK(u.m.unitValue("school_deaths", kSchool) == 2);
  u.m.reduce();
  CHECK(u.m.unitValue("class_deaths", kClassA) == 2);
  CHECK(u.m.changedUnits("class_deaths").empty());
}

TEST_CASE(
    "Metrics per unit - a current metric forgets units it no longer sees") {
  UnitMetrics u;
  u.m.onPresent(u.person(kPupilA), 1);
  u.m.onPresent(u.person(kPupilB), 1);
  u.m.reduce();
  CHECK(u.m.unitValue("class_now", kClassA) == 1);
  CHECK(u.m.unitValue("class_now", kClassB) == 1);

  u.m.onPresent(u.person(kPupilB), 1);
  u.m.reduce();
  CHECK(u.m.unitValue("class_now", kClassA) == 0);
  CHECK(u.m.unitValue("class_now", kClassB) == 1);
  CHECK(u.m.changedUnits("class_now") == std::vector<VenueId>{kClassB});
}

TEST_CASE("Metrics per unit - reading the wrong kind of metric is refused") {
  UnitMetrics u;
  CHECK(has(thrown([&] { u.m.value("class_now"); }), "read it with unitValue"));
  CHECK(has(thrown([&] { u.m.unitValue("world_total", kPub); }),
            "has scope 'world'"));
  CHECK(has(thrown([&] { u.m.changedUnits("world_total"); }),
            "has scope 'world'"));
}

TEST_CASE("Metrics per unit - the parent index is built only when needed") {
  SUBCASE("a parent venue metric fills it from the venues") {
    UnitMetrics u;
    REQUIRE(u.world.venue_parent_by_id.size() == kVenueCount);
    CHECK(u.world.parentUnitOf(kClassA) == kSchool);
    CHECK(u.world.parentUnitOf(kClassC) == kSchoolTwo);
    CHECK(u.world.parentUnitOf(kPub) == kPub);        // no parent: itself
    CHECK(u.world.parentUnitOf(kSchool) == kSchool);  // the parent itself
    CHECK(has(thrown([&] { u.world.parentUnitOf(99); }),
              "outside the parent index"));
  }
  SUBCASE("venue and world metrics leave it empty") {
    WorldState world = makeWorld();
    const Disease disease = makeMetricDisease();
    ObservedMetrics m;
    m.declare("a", cfg("infection", "venue", "primary_activity"));
    m.declare("b", cfg("death", "world"));
    m.resolve(world, disease);
    m.require("a");
    m.require("b");
    CHECK_FALSE(m.needsVenueParents());
    m.buildVenueParents(world);
    CHECK(world.venue_parent_by_id.empty());
  }
  SUBCASE("a declared but unreferenced parent venue metric costs nothing") {
    WorldState world = makeWorld();
    const Disease disease = makeMetricDisease();
    ObservedMetrics m;
    m.declare("a", cfg("infection", "parent_venue", "primary_activity"));
    m.resolve(world, disease);
    CHECK_FALSE(m.needsVenueParents());
    m.buildVenueParents(world);
    CHECK(world.venue_parent_by_id.empty());
  }
}

TEST_CASE("Metrics per unit - stored totals restore exactly or throw") {
  UnitMetrics before;
  before.m.onDeath(before.person(kTwoClasses));
  before.m.onDeath(before.person(kPupilA));
  before.m.onPresent(before.person(kPupilA), 1);
  before.m.reduce();
  const auto stored = before.m.snapshot();

  UnitMetrics after;
  after.m.restoreSnapshot(stored);
  CHECK(after.m.unitValue("class_deaths", kClassA) == 2);
  CHECK(after.m.unitValue("class_deaths", kClassB) == 1);
  CHECK(after.m.unitValue("school_deaths", kSchool) == 2);
  // Counting goes on from the restored totals.
  after.m.onDeath(after.person(kPupilB));
  after.m.reduce();
  CHECK(after.m.unitValue("class_deaths", kClassB) == 2);
  CHECK(after.m.unitValue("school_deaths", kSchool) == 3);
  // A current metric is not stored: the next daily scan rebuilds it.
  CHECK(after.m.unitValue("class_now", kClassA) == 0);

  // The same name under another scope is refused.
  WorldState world = makeWorld();
  const Disease disease = makeMetricDisease();
  ObservedMetrics other;
  other.declare("class_deaths", cfg("death", "world"));
  other.declare("school_deaths",
                cfg("death", "parent_venue", "primary_activity"));
  other.declare("class_now", cfg("infection", "venue", "primary_activity"));
  other.declare("school_now",
                cfg("infection", "parent_venue", "primary_activity"));
  other.declare("pub_now", cfg("infection", "venue", "leisure"));
  other.resolve(world, disease);
  for (const char* n :
       {"class_now", "school_now", "class_deaths", "school_deaths", "pub_now"})
    other.require(n);
  CHECK(has(thrown([&] { other.restoreSnapshot(stored); }),
            "different scope than this run"));
}

TEST_CASE(
    "Epidemiology - hooks count a real trajectory at the person's venues") {
  UnitMetrics u;
  Epidemiology epi(u.world, &u.disease);
  epi.setObservedMetrics(&u.m);

  // Two people infected at t = 0: mild until 2, severe until 3, dead after.
  for (PersonId id : {kPupilA, kTwoClasses}) {
    u.world.people[id].infection = std::make_unique<Infection>(
        &u.disease, 0.0, &u.world.people[id], 100 + id, kNoTransmissionContext,
        nullptr, "office", 0);
    epi.trackInfection(id);
  }

  epi.updateInfectionStates(1.0, {});
  epi.reduceObservedMetrics();
  CHECK(u.m.unitValue("class_now", kClassA) == 2);
  CHECK(u.m.unitValue("class_now", kClassB) == 1);
  CHECK(u.m.unitValue("school_now", kSchool) == 2);

  epi.updateInfectionStates(5.0, {});
  epi.reduceObservedMetrics();
  CHECK(u.m.unitValue("class_now", kClassA) == 0);  // nobody is infected now
  CHECK(u.m.unitValue("class_deaths", kClassA) == 2);
  CHECK(u.m.unitValue("class_deaths", kClassB) == 1);
  CHECK(u.m.unitValue("school_deaths", kSchool) == 2);
}
