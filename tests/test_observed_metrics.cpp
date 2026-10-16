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

TEST_CASE("ObservedMetrics - resolve rejects unknown names and venue scope") {
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
  SUBCASE("venue scope is refused as not built") {
    ObservedMetrics m;
    m.declare("a", cfg("infection", "venue", "school"));
    CHECK(has(thrown([&] { m.resolve(world, disease); }),
              "scope 'venue' is not supported yet"));
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
  before->onDeath();
  before->onDeath();
  before->onTransition(1, 2);
  before->reduce();  // single process: local becomes the total
  const auto stored = before->activeValues();
  REQUIRE(stored.size() == 2);

  auto after = make(false);
  after->restoreActiveValues(stored);
  CHECK(after->value("deaths") == 2);
  CHECK(after->value("severe") == 1);
  // Counting continues from the restored total, not from zero.
  after->onDeath();
  after->reduce();
  CHECK(after->value("deaths") == 3);

  // A different active set is refused in both directions.
  auto more = make(true);
  CHECK(has(thrown([&] { more->restoreActiveValues(stored); }),
            "cannot be rebuilt or dropped"));
  auto fewer = make(false);
  auto three = make(true).get()->activeValues();
  CHECK(has(thrown([&] { fewer->restoreActiveValues(three); }),
            "cannot be rebuilt or dropped"));
}
