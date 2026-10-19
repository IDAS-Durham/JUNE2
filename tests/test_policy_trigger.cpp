#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>

#include "core/world_state.h"
#include "doctest.h"
#include "epidemiology/disease.h"
#include "epidemiology/policy.h"
#include "loaders/policy_loader.h"
#include "test_utils.h"

using namespace june;

namespace {

constexpr int16_t kResidence = 0;
constexpr int16_t kLeisure = 1;
constexpr VenueId kHome = 0;
constexpr VenueId kPub = 1;

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

// healthy(0) -> mild(1) -> dead(2). Only here so the metrics can resolve.
Disease makeDisease() {
  TransmissionParams trans;
  trans.mode = InfectiousnessMode::STAGE_DRIVEN;
  auto curve = std::make_shared<ConstantCurve>(1.0);
  trans.symptom_id_curves = {nullptr, curve, curve};
  std::vector<SymptomTag> tags = {
      {"healthy", -1, 0}, {"mild", 1, 1}, {"dead", 2, 2}};
  DiseaseStageSettings settings;
  settings.fatality_stages = {"dead"};
  TrajectoryDefinition td;
  td.selection_key = "general";
  td.severity = 1.0;
  td.stages.push_back({"mild", {"constant", {{"value", 2.0}}}});
  td.stages.push_back({"dead", {"constant", {{"value", 100.0}}}});
  return Disease("TriggerDisease", tags, settings, {td}, {}, trans);
}

// One person whose residence is the home venue; a leisure closure sends them
// there.
WorldState buildWorld() {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 2);
  world.activity_names = {"residence", "leisure"};
  world.venue_type_names = {"household", "pub"};
  world.venues[0].type_id = 0;
  world.venues[1].type_id = 1;
  world.buildIndices();
  for (auto& person : world.people) {
    person.activity_meta_start =
        static_cast<uint32_t>(world.activity_meta.size());
    person.activity_meta_count = 1;
    world.activity_meta.push_back(
        {kResidence, static_cast<uint32_t>(world.activity_venues.size()), 1});
    world.activity_venues.push_back({kHome, 0});
    person.applicable_temporal_policy_mask = 1;
  }
  return world;
}

const std::string kMetrics =
    "observed_metrics:\n"
    "  deaths: {source: death, scope: world}\n"
    "  infected: {source: infection, scope: world}\n";

// Declared only where a test loads a policy file without resolving it: the
// metrics refuse to resolve a venue scope.
const std::string kVenueMetric =
    "  pub_cases: {source: infection, scope: venue, activity: leisure}\n";

// A policy file with one temporal policy carrying `trigger`. `window` is extra
// date lines for the policy, already indented.
std::string policyText(const std::string& trigger,
                       const std::string& window = "",
                       const std::string& extra_metrics = "") {
  return kMetrics + extra_metrics +
         "policies:\n"
         "  temporal_policies:\n"
         "    - name: close_leisure\n" +
         window +
         "      override_activities: [leisure]\n"
         "      replacement: residence\n"
         "      compliance_rate: 1.0\n"
         "      trigger: " +
         trigger + "\n";
}

// Loads `text` into `pm`; returns the thrown message or "".
std::string loadPolicyText(PolicyManager& pm, const std::string& text) {
  const auto path =
      std::filesystem::temp_directory_path() / "june_policy_trigger.yaml";
  {
    std::ofstream out(path);
    out << text;
  }
  const std::string msg = thrown(
      [&] { PolicyLoader::loadPolicies(pm, path.string(), "2020-02-03"); });
  std::filesystem::remove(path);
  return msg;
}

PolicyTrigger cumulativeTrigger(int64_t activate_at, int64_t calm_days,
                                int64_t tolerance = 0,
                                int64_t tolerance_pct = 0) {
  PolicyTrigger t;
  t.cumulative = true;
  t.activate_at = activate_at;
  t.calm_days = calm_days;
  t.tolerance = tolerance;
  t.tolerance_pct = tolerance_pct;
  return t;
}

// The person a world scope death is counted for; a world total ignores them.
const Person nobody{};

// A loaded world with one triggered leisure closure, driven day by day the way
// the simulator drives it: count, reduce, advance.
struct TriggeredWorld {
  WorldState world = buildWorld();
  Disease disease = makeDisease();
  PolicyManager pm{world};

  explicit TriggeredWorld(const std::string& text) {
    REQUIRE(loadPolicyText(pm, text) == "");
    pm.observedMetrics().resolve(world, disease);
    pm.requireTriggerMetrics();
    pm.resolveAll(disease);
  }

  void endDay(int day, int deaths) {
    for (int i = 0; i < deaths; ++i) pm.observedMetrics().onDeath(nobody);
    pm.observedMetrics().reduce();
    pm.advanceTriggers(day);
  }

  bool overridden(double time) {
    return pm
        .getOverride(world.people[0], kLeisure, kPub, 0,
                     SlotVenueType::fromVenue(kPub), time, 0)
        .has_value();
  }
};

const std::string kDeathsTrigger =
    "{metric: deaths, activate_at: 3, calm_days: 5}";

}  // namespace

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------

TEST_CASE("PolicyTrigger - cumulative metric follows the 100 deaths example") {
  PolicyTrigger t = cumulativeTrigger(100, 200, 5);
  t.step(49, 99);
  CHECK_FALSE(t.closed);
  t.step(50, 100);
  CHECK(t.closed);
  t.step(60, 105);  // growth of 5 is within the tolerance
  CHECK(t.closed);
  t.step(249, 105);  // 199 days since closing
  CHECK(t.closed);
  t.step(250, 105);  // 200 days
  CHECK_FALSE(t.closed);
  CHECK(t.anchor_value == 105);  // the baseline is the value at reopening
  t.step(251, 204);
  CHECK_FALSE(t.closed);
  t.step(252, 205);  // 100 more than the baseline
  CHECK(t.closed);
}

TEST_CASE("PolicyTrigger - growth beyond the tolerance restarts the clock") {
  PolicyTrigger t = cumulativeTrigger(100, 200, 5);
  t.step(50, 100);
  t.step(100, 106);  // 6 more than 5 allowed: anchor moves to (100, 106)
  CHECK(t.anchor_day == 100);
  CHECK(t.anchor_value == 106);
  t.step(299, 106);
  CHECK(t.closed);
  t.step(300, 106);
  CHECK_FALSE(t.closed);

  // Growth counts from the anchor, not from the day before, so a slow creep
  // still moves it.
  PolicyTrigger creep = cumulativeTrigger(100, 200, 5);
  creep.step(50, 100);
  creep.step(60, 104);
  CHECK(creep.anchor_day == 50);
  creep.step(70, 108);
  CHECK(creep.anchor_day == 70);
}

TEST_CASE("PolicyTrigger - the percentage tolerance is of the anchor value") {
  // The first closure already has a non zero percentage tolerance: 5 percent
  // of the 100 it closed at, not 5 percent of the baseline of 0.
  PolicyTrigger t = cumulativeTrigger(100, 10, 0, 5);
  t.step(1, 100);
  CHECK(t.closed);
  t.step(5, 105);  // exactly the 5 allowed
  CHECK(t.anchor_day == 1);
  t.step(11, 105);
  CHECK_FALSE(t.closed);

  // A rise beyond it moves the anchor, and the next tolerance is 5 percent of
  // the new anchor value, rounded down.
  PolicyTrigger moved = cumulativeTrigger(100, 10, 0, 5);
  moved.step(1, 100);
  moved.step(5, 106);
  CHECK(moved.anchor_day == 5);
  CHECK(moved.anchor_value == 106);
  moved.step(14, 111);  // 5 more than 106, and 5 percent of 106 is 5
  CHECK(moved.anchor_day == 5);
  moved.step(15, 111);
  CHECK_FALSE(moved.closed);

  // 2 percent of an anchor of 400 deaths is a tolerance of 8.
  PolicyTrigger deaths = cumulativeTrigger(400, 30, 0, 2);
  deaths.step(10, 400);
  deaths.step(20, 408);
  CHECK(deaths.anchor_day == 10);
  deaths.step(21, 409);
  CHECK(deaths.anchor_day == 21);

  // The larger of the absolute and the percentage tolerance applies.
  PolicyTrigger both = cumulativeTrigger(400, 30, 10, 2);
  both.step(10, 400);
  both.step(20, 410);
  CHECK(both.anchor_day == 10);
}

TEST_CASE("PolicyTrigger - a current metric has hysteresis") {
  PolicyTrigger t;
  t.activate_at = 10;
  t.deactivate_at = 4;
  const int64_t levels[] = {9, 10, 7, 5, 4, 9, 10};
  const bool expected[] = {false, true, true, true, false, false, true};
  for (int day = 0; day < 7; ++day) {
    t.step(day, levels[day]);
    INFO("day " << day << " level " << levels[day]);
    CHECK(t.closed == expected[day]);
  }

  // A level of 0 is reachable, so a deactivate_at of 0 is a real threshold.
  PolicyTrigger zero;
  zero.activate_at = 10;
  zero.deactivate_at = 0;
  zero.step(0, 10);
  zero.step(1, 1);
  CHECK(zero.closed);
  zero.step(2, 0);
  CHECK_FALSE(zero.closed);
}

// ---------------------------------------------------------------------------
// Loader
// ---------------------------------------------------------------------------

TEST_CASE("PolicyLoader - a good trigger loads and the policy starts off") {
  WorldState world;
  SUBCASE("cumulative metric") {
    PolicyManager pm(world);
    REQUIRE(loadPolicyText(pm, policyText("{metric: deaths, activate_at: 100, "
                                          "calm_days: 200, tolerance: 5, "
                                          "tolerance_pct: 2}")) == "");
    const auto& trigger = pm.getTemporalPolicies()[0].trigger;
    REQUIRE(trigger.has_value());
    CHECK(trigger->cumulative);
    CHECK(trigger->activate_at == 100);
    CHECK(trigger->calm_days == 200);
    CHECK(trigger->tolerance == 5);
    CHECK(trigger->tolerance_pct == 2);
    CHECK_FALSE(pm.getTemporalPolicies()[0].window.contains(5.0));
  }
  SUBCASE("current metric, deactivate_at 0 allowed") {
    PolicyManager pm(world);
    REQUIRE(loadPolicyText(pm, policyText("{metric: infected, activate_at: 10, "
                                          "deactivate_at: 0}")) == "");
    const auto& trigger = pm.getTemporalPolicies()[0].trigger;
    REQUIRE(trigger.has_value());
    CHECK_FALSE(trigger->cumulative);
    CHECK(*trigger->deactivate_at == 0);
  }
  SUBCASE("symptom policy") {
    PolicyManager pm(world);
    const std::string text =
        kMetrics +
        "policies:\n"
        "  symptom_policies:\n"
        "    - name: stay_home\n"
        "      symptoms: [mild]\n"
        "      override_activities: [leisure]\n"
        "      trigger: {metric: deaths, activate_at: 3, calm_days: 5}\n";
    REQUIRE(loadPolicyText(pm, text) == "");
    REQUIRE(pm.getSymptomPolicies()[0].trigger.has_value());
    CHECK_FALSE(pm.getSymptomPolicies()[0].window.contains(5.0));
  }
  SUBCASE("a policy with no trigger is in force as before") {
    PolicyManager pm(world);
    REQUIRE(loadPolicyText(
                pm, kMetrics + "policies:\n  temporal_policies:\n"
                               "    - name: close_leisure\n"
                               "      override_activities: [leisure]\n") == "");
    CHECK_FALSE(pm.getTemporalPolicies()[0].trigger.has_value());
    CHECK(pm.getTemporalPolicies()[0].window.contains(5.0));
    pm.requireTriggerMetrics();
    CHECK_FALSE(pm.observedMetrics().anyActive());
  }
}

TEST_CASE("PolicyLoader - every bad trigger is refused with a reason") {
  struct Bad {
    const char* trigger;
    const char* expected;
  };
  const Bad cases[] = {
      {"{metric: nope, activate_at: 5, calm_days: 3}", "unknown metric 'nope'"},
      {"{activate_at: 5, calm_days: 3}", "'metric' is required"},
      {"{metric: deaths, activate_at: 5, calm_days: 3, deactivate_at: 1}",
       "'deactivate_at' is not valid for the cumulative metric 'deaths'"},
      {"{metric: infected, activate_at: 5, deactivate_at: 5}",
       "must be below 'activate_at'"},
      {"{metric: infected, activate_at: 5, deactivate_at: 8}",
       "must be below 'activate_at'"},
      {"{metric: infected, activate_at: 5}",
       "'deactivate_at' is required for the current metric 'infected'"},
      {"{metric: deaths, activate_at: 5}",
       "'calm_days' is required for the cumulative metric 'deaths'"},
      {"{metric: infected, activate_at: 5, deactivate_at: 1, calm_days: 3}",
       "only valid for a cumulative metric"},
      {"{metric: deaths, calm_days: 3}", "'activate_at' is required"},
      {"{metric: deaths, activate_at: 5, calm_days: 3, response: []}",
       "never both"},
      {"{metric: deaths, response: []}", "'response' list is not built yet"},
      {"{metric: deaths}", "needs the threshold fields"},
      {"{metric: pub_cases, activate_at: 5, deactivate_at: 1}",
       "scope 'venue'"},
      {"{metric: pub_cases, activate_at: 5, deactivate_at: 1}",
       "is not built yet"},
      {"{metric: deaths, activate_at: 5, calm_days: 3, decision_unit: person}",
       "'decision_unit' is not built yet"},
      {"{metric: deaths, activate_at: 0, calm_days: 3}",
       "'activate_at' must be at least 1"},
      {"{metric: deaths, activate_at: -3, calm_days: 3}",
       "'activate_at' must be at least 1"},
      {"{metric: deaths, activate_at: 5, calm_days: 0}",
       "'calm_days' must be at least 1"},
      {"{metric: deaths, activate_at: 5, calm_days: -1}",
       "'calm_days' must be at least 1"},
      {"{metric: infected, activate_at: 5, deactivate_at: -1}",
       "'deactivate_at' must not be negative"},
      {"{metric: deaths, activate_at: 5, calm_days: 3, tolerance: -1}",
       "'tolerance' must not be negative"},
      {"{metric: deaths, activate_at: 5, calm_days: 3, tolerance_pct: -1}",
       "whole percent from 0 to 100"},
      {"{metric: deaths, activate_at: 5, calm_days: 3, tolerance_pct: 101}",
       "whole percent from 0 to 100"},
      {"{metric: deaths, activate_at: 2.5, calm_days: 3}",
       "'activate_at' must be a whole number"},
      {"{metric: deaths, activate_at: ten, calm_days: 3}",
       "'activate_at' must be a whole number"},
      {"{metric: deaths, activate_at: 5, calm_days: 3, colour: red}",
       "unknown field 'colour'"},
  };
  WorldState world;
  for (const auto& c : cases) {
    PolicyManager pm(world);
    const std::string msg =
        loadPolicyText(pm, policyText(c.trigger, "", kVenueMetric));
    INFO(c.trigger);
    CHECK(has(msg, c.expected));
    CHECK(has(msg, "temporal_policy 'close_leisure' trigger: "));
  }
}

// ---------------------------------------------------------------------------
// In force
// ---------------------------------------------------------------------------

TEST_CASE("ActiveWindow - a trigger holds the policy off whatever the dates") {
  ActiveWindow window{0.0, 10.0};
  CHECK(window.contains(5.0));
  window.trigger_state = false;
  CHECK_FALSE(window.contains(5.0));
}

TEST_CASE("Trigger - the effect starts at the first slot of the next day") {
  TriggeredWorld w(policyText(kDeathsTrigger));
  CHECK_FALSE(w.overridden(0.5));  // starts off

  w.endDay(0, 2);
  CHECK_FALSE(w.overridden(1.0));

  // Day 1 is still running: nothing changes until its boundary.
  CHECK_FALSE(w.overridden(1.9));
  w.endDay(1, 1);  // the third death closes the trigger
  CHECK(w.overridden(2.0));
  CHECK(w.overridden(2.9));

  // Closed on day 1, so 5 calm days reopen it at the end of day 6.
  for (int day = 2; day <= 5; ++day) w.endDay(day, 0);
  CHECK(w.overridden(6.0));
  w.endDay(6, 0);
  CHECK_FALSE(w.overridden(7.0));
}

TEST_CASE("Trigger - the state evolves while the window is not open") {
  // The window opens on day 20 (2020-02-23 against a start of 2020-02-03).
  const std::string window = "      start_date: \"2020-02-23\"\n";

  SUBCASE("a threshold crossed before the window takes effect when it opens") {
    TriggeredWorld w(
        policyText("{metric: deaths, activate_at: 3, calm_days: 100}", window));
    w.endDay(0, 3);
    CHECK_FALSE(w.overridden(5.0));  // closed, but the window is not open
    for (int day = 1; day < 20; ++day) w.endDay(day, 0);
    CHECK_FALSE(w.overridden(19.9));
    CHECK(w.overridden(20.0));
  }
  SUBCASE("a trigger that reopened before the window does not hold it") {
    TriggeredWorld w(policyText(kDeathsTrigger, window));
    w.endDay(0, 3);
    for (int day = 1; day < 19; ++day) w.endDay(day, 0);  // reopens at day 5
    w.endDay(19, 0);
    CHECK_FALSE(w.overridden(20.0));
  }
}

TEST_CASE("Trigger - a policy without one ignores the daily step") {
  WorldState world = buildWorld();
  Disease disease = makeDisease();
  PolicyManager pm(world);
  TemporalPolicy plain;
  plain.name = "plain";
  plain.action.override_activities.insert("leisure");
  plain.action.replacement_activity = "residence";
  plain.action.compliance_rate = 1.0;
  pm.addTemporalPolicy(plain);
  pm.observedMetrics().resolve(world, disease);
  pm.requireTriggerMetrics();
  pm.resolveAll(disease);
  CHECK_FALSE(pm.observedMetrics().anyActive());
  pm.advanceTriggers(3);
  CHECK(pm.getTemporalPolicies()[0].window.contains(4.0));
  CHECK(pm.triggerStates().empty());
}

// ---------------------------------------------------------------------------
// Checkpoint
// ---------------------------------------------------------------------------

TEST_CASE("Trigger - closed state survives a round trip and resumes the same") {
  const std::string text = policyText(
      "{metric: deaths, activate_at: 3, calm_days: 5, tolerance: 1}");
  TriggeredWorld before(text);
  before.endDay(0, 2);
  before.endDay(1, 1);  // closes at day 1
  before.endDay(2, 1);  // growth of 1 is within the tolerance
  REQUIRE(before.overridden(3.0));

  const auto states = before.pm.triggerStates();
  REQUIRE(states.size() == 1);
  CHECK(states[0].key == "temporal/close_leisure");
  CHECK(states[0].closed);
  CHECK(states[0].anchor_day == 1);
  CHECK(states[0].anchor_value == 3);

  // A fresh run restored the way the simulator does: metric totals, then
  // trigger state.
  TriggeredWorld after(text);
  after.pm.observedMetrics().restoreSnapshot(
      before.pm.observedMetrics().snapshot());
  after.pm.restoreTriggerStates(states);
  CHECK(after.overridden(3.0));

  // Both reopen on the same day, the end of day 6.
  for (int day = 3; day <= 5; ++day) {
    before.endDay(day, 0);
    after.endDay(day, 0);
    CHECK(before.overridden(day + 1.0));
    CHECK(after.overridden(day + 1.0));
  }
  before.endDay(6, 0);
  after.endDay(6, 0);
  CHECK_FALSE(before.overridden(7.0));
  CHECK_FALSE(after.overridden(7.0));
}

TEST_CASE("Trigger - restoring a different set of triggers is refused") {
  const std::string text = policyText(kDeathsTrigger);
  TriggeredWorld with(text);
  with.endDay(0, 3);
  const auto states = with.pm.triggerStates();

  SUBCASE("stored triggers, none in this run") {
    WorldState world;
    PolicyManager none(world);
    CHECK(has(thrown([&] { none.restoreTriggerStates(states); }),
              "cannot be rebuilt"));
  }
  SUBCASE("a trigger in this run, none stored") {
    CHECK(has(thrown([&] { with.pm.restoreTriggerStates({}); }),
              "cannot be rebuilt"));
  }
  SUBCASE("same count, different policy") {
    auto renamed = states;
    renamed[0].key = "temporal/other";
    CHECK(has(thrown([&] { with.pm.restoreTriggerStates(renamed); }),
              "temporal/other"));
  }
}
