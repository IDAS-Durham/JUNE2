#include "epidemiology/observed_metrics.h"

#include <algorithm>
#include <memory>
#include <stdexcept>

#include "core/world_state.h"
#include "epidemiology/disease.h"
#include "utils/mpi_utils.h"

#ifdef USE_MPI
#include <mpi.h>
#endif

namespace june {

namespace {

// Every infected person, present now.
class InfectionSource : public MetricSource {
 public:
  bool cumulative() const override { return false; }
  bool needsStage() const override { return false; }
  bool counts(const Observation& obs, uint16_t) const override {
    return obs.kind == ObservationKind::Present;
  }
};

// Infected people whose current stage is the named one.
class StageSource : public MetricSource {
 public:
  bool cumulative() const override { return false; }
  bool needsStage() const override { return true; }
  bool counts(const Observation& obs, uint16_t stage_id) const override {
    return obs.kind == ObservationKind::Present && obs.symptom_id == stage_id;
  }
};

// People marked dead, counted once at the death site.
class DeathSource : public MetricSource {
 public:
  bool cumulative() const override { return true; }
  bool needsStage() const override { return false; }
  bool counts(const Observation& obs, uint16_t) const override {
    return obs.kind == ObservationKind::Death;
  }
};

// People entering the named stage: placed in it at the moment of infection, or
// moved into it from another stage. Going from the stage to itself is not an
// admission.
class StageAdmissionSource : public MetricSource {
 public:
  bool cumulative() const override { return true; }
  bool needsStage() const override { return true; }
  bool counts(const Observation& obs, uint16_t stage_id) const override {
    if (obs.symptom_id != stage_id) return false;
    return obs.kind == ObservationKind::Start ||
           (obs.kind == ObservationKind::Transition &&
            obs.previous_symptom_id != stage_id);
  }
};

struct SourceEntry {
  const char* name;
  const MetricSource* source;
};

const std::vector<SourceEntry>& sourceTable() {
  static const InfectionSource infection;
  static const StageSource stage;
  static const DeathSource death;
  static const StageAdmissionSource stage_admission;
  static const std::vector<SourceEntry> table = {
      {"infection", &infection},
      {"stage", &stage},
      {"death", &death},
      {"stage_admission", &stage_admission},
  };
  return table;
}

std::string joinNames(const std::vector<std::string>& names) {
  std::string out;
  for (const auto& n : names) out += (out.empty() ? "" : ", ") + n;
  return out;
}

std::string metricLabel(const std::string& name) {
  return "observed_metrics '" + name + "': ";
}

}  // namespace

const MetricSource* findMetricSource(const std::string& name) {
  for (const auto& e : sourceTable())
    if (name == e.name) return e.source;
  return nullptr;
}

std::vector<std::string> metricSourceNames() {
  std::vector<std::string> names;
  for (const auto& e : sourceTable()) names.push_back(e.name);
  return names;
}

void ObservedMetrics::declare(const std::string& name,
                              const ObservedMetricConfig& cfg) {
  if (name.empty() || name.find('/') != std::string::npos) {
    throw std::runtime_error("observed_metrics: metric name '" + name +
                             "' must be non-empty and must not contain '/'.");
  }
  if (resolved_) {
    throw std::logic_error(metricLabel(name) +
                           "declared after the metrics were resolved.");
  }
  for (const auto& e : entries_) {
    if (e.name == name) {
      throw std::runtime_error(metricLabel(name) + "declared twice.");
    }
  }

  Entry entry;
  entry.name = name;
  entry.cfg = cfg;

  entry.source = findMetricSource(cfg.source);
  if (!entry.source) {
    throw std::runtime_error(
        metricLabel(name) + "unknown source '" + cfg.source +
        "'. Known sources: " + joinNames(metricSourceNames()) + ".");
  }

  if (cfg.scope == "world") {
    entry.scope = MetricScope::World;
  } else if (cfg.scope == "venue") {
    entry.scope = MetricScope::Venue;
  } else {
    throw std::runtime_error(metricLabel(name) + "unknown scope '" + cfg.scope +
                             "'. Known scopes: world, venue.");
  }

  const bool is_venue = entry.scope == MetricScope::Venue;
  if (is_venue && cfg.activity.empty()) {
    throw std::runtime_error(metricLabel(name) +
                             "scope 'venue' requires an 'activity'.");
  }
  if (!is_venue && !cfg.activity.empty()) {
    throw std::runtime_error(metricLabel(name) +
                             "'activity' is only valid "
                             "with scope 'venue', but scope is '" +
                             cfg.scope + "'.");
  }
  if (entry.source->needsStage() && cfg.stage.empty()) {
    throw std::runtime_error(metricLabel(name) + "source '" + cfg.source +
                             "' requires a 'stage'.");
  }
  if (!entry.source->needsStage() && !cfg.stage.empty()) {
    throw std::runtime_error(metricLabel(name) +
                             "'stage' is not valid with "
                             "source '" +
                             cfg.source + "'.");
  }

  entries_.push_back(std::move(entry));
  std::sort(entries_.begin(), entries_.end(),
            [](const Entry& a, const Entry& b) { return a.name < b.name; });
}

void ObservedMetrics::resolve(const WorldState& world, const Disease& disease) {
  for (auto& e : entries_) {
    if (!e.cfg.activity.empty() && world.getActivityIndex(e.cfg.activity) < 0) {
      throw std::runtime_error(metricLabel(e.name) + "unknown activity '" +
                               e.cfg.activity + "'.");
    }
    if (!e.cfg.stage.empty()) {
      // "stage" in YAML is a symptom name to the engine. Looked up here by
      // name against the disease's own list, so a misspelling throws instead
      // of falling back to symptom 0.
      const auto& names = disease.getSymptomNames();
      const auto it = std::find(names.begin(), names.end(), e.cfg.stage);
      if (it == names.end()) {
        throw std::runtime_error(metricLabel(e.name) + "unknown stage '" +
                                 e.cfg.stage +
                                 "'. Known stages: " + joinNames(names) + ".");
      }
      e.stage_id = static_cast<uint16_t>(it - names.begin());
    }
    if (e.scope == MetricScope::Venue) {
      throw std::runtime_error(
          metricLabel(e.name) +
          "scope 'venue' is not supported yet; only scope 'world' is built.");
    }
  }
  resolved_ = true;
}

ObservedMetrics::Entry& ObservedMetrics::find(const std::string& name) {
  for (auto& e : entries_)
    if (e.name == name) return e;
  throw std::runtime_error("observed_metrics: no metric named '" + name +
                           "' is declared.");
}

const ObservedMetrics::Entry& ObservedMetrics::find(
    const std::string& name) const {
  return const_cast<ObservedMetrics*>(this)->find(name);
}

std::vector<std::string> ObservedMetrics::declaredNames() const {
  std::vector<std::string> names;
  for (const auto& e : entries_) names.push_back(e.name);
  return names;
}

bool ObservedMetrics::isCumulative(const std::string& name) const {
  return find(name).source->cumulative();
}

bool ObservedMetrics::isWorldScope(const std::string& name) const {
  return find(name).scope == MetricScope::World;
}

void ObservedMetrics::require(const std::string& name) {
  if (!resolved_) {
    throw std::logic_error("observed_metrics: require('" + name +
                           "') called before the metrics were resolved.");
  }
  Entry& e = find(name);
  if (e.active) return;
  e.active = true;
  active_.clear();
  tracks_present_ = false;
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (!entries_[i].active) continue;
    active_.push_back(i);
    if (!entries_[i].source->cumulative()) tracks_present_ = true;
  }
}

void ObservedMetrics::count(const Observation& obs) {
  for (size_t i : active_) {
    Entry& e = entries_[i];
    if (e.source->counts(obs, e.stage_id)) ++e.local;
  }
}

void ObservedMetrics::onInfection(uint16_t symptom_id) {
  count({ObservationKind::Start, symptom_id, 0});
}

void ObservedMetrics::onTransition(uint16_t previous_symptom_id,
                                   uint16_t symptom_id) {
  count({ObservationKind::Transition, symptom_id, previous_symptom_id});
}

void ObservedMetrics::onDeath() { count({ObservationKind::Death, 0, 0}); }

void ObservedMetrics::onPresent(uint16_t symptom_id) {
  count({ObservationKind::Present, symptom_id, 0});
}

void ObservedMetrics::reduce() {
  if (active_.empty()) return;

  std::vector<int64_t> sums;
  sums.reserve(active_.size());
  for (size_t i : active_) sums.push_back(entries_[i].local);

  // Integer sums only, so every rank ends with the same totals whatever the
  // process count. A single process already holds the global value.
#ifdef USE_MPI
  const auto mpi = mpi_runtime::state();
  if (mpi.active && mpi.size > 1) {
    MPI_Allreduce(MPI_IN_PLACE, sums.data(), static_cast<int>(sums.size()),
                  MPI_INT64_T, MPI_SUM, MPI_COMM_WORLD);
  }
#endif

  for (size_t k = 0; k < active_.size(); ++k) {
    Entry& e = entries_[active_[k]];
    e.reduced = e.source->cumulative() ? e.reduced + sums[k] : sums[k];
    e.local = 0;
  }
}

int64_t ObservedMetrics::value(const std::string& name) const {
  return find(name).reduced;
}

int64_t ObservedMetrics::localCount(const std::string& name) const {
  return find(name).local;
}

std::vector<std::pair<std::string, int64_t>> ObservedMetrics::activeValues()
    const {
  std::vector<std::pair<std::string, int64_t>> out;
  for (size_t i : active_)
    out.emplace_back(entries_[i].name, entries_[i].reduced);
  return out;
}

void ObservedMetrics::restoreActiveValues(
    const std::vector<std::pair<std::string, int64_t>>& stored) {
  std::vector<std::string> stored_names;
  for (const auto& [n, v] : stored) stored_names.push_back(n);
  std::vector<std::string> active_names;
  for (size_t i : active_) active_names.push_back(entries_[i].name);
  std::sort(stored_names.begin(), stored_names.end());
  if (stored_names != active_names) {
    throw std::runtime_error(
        "observed_metrics: the checkpoint holds metrics [" +
        joinNames(stored_names) + "] but this run has [" +
        joinNames(active_names) +
        "] active. A cumulative counter cannot be rebuilt or dropped; resume "
        "with the same set of referenced metrics.");
  }
  for (const auto& [n, v] : stored) {
    Entry& e = find(n);
    e.reduced = v;
    e.local = 0;
  }
}

}  // namespace june
