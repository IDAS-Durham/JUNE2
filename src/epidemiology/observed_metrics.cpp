#include "epidemiology/observed_metrics.h"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>

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

#ifdef USE_MPI
// Every rank's integers, joined in rank order. Collective.
std::vector<int64_t> allGatherInt64(const std::vector<int64_t>& mine,
                                    int ranks) {
  const int my_count = static_cast<int>(mine.size());
  std::vector<int> counts(static_cast<size_t>(ranks));
  MPI_Allgather(&my_count, 1, MPI_INT, counts.data(), 1, MPI_INT,
                MPI_COMM_WORLD);
  std::vector<int> displs(static_cast<size_t>(ranks), 0);
  for (int r = 1; r < ranks; ++r) displs[r] = displs[r - 1] + counts[r - 1];
  std::vector<int64_t> all(static_cast<size_t>(displs.back() + counts.back()));
  MPI_Allgatherv(mine.data(), my_count, MPI_INT64_T, all.data(), counts.data(),
                 displs.data(), MPI_INT64_T, MPI_COMM_WORLD);
  return all;
}
#endif

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
  } else if (cfg.scope == "parent_venue") {
    entry.scope = MetricScope::ParentVenue;
  } else {
    throw std::runtime_error(metricLabel(name) + "unknown scope '" + cfg.scope +
                             "'. Known scopes: world, venue, parent_venue.");
  }

  if (entry.perUnit() && cfg.activity.empty()) {
    throw std::runtime_error(metricLabel(name) + "scope '" + cfg.scope +
                             "' requires an 'activity'.");
  }
  if (!entry.perUnit() && !cfg.activity.empty()) {
    throw std::runtime_error(metricLabel(name) +
                             "'activity' is only valid with scope 'venue' or "
                             "'parent_venue', but scope is '" +
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
  world_ = &world;
  for (auto& e : entries_) {
    if (!e.cfg.activity.empty()) {
      const int activity = world.getActivityIndex(e.cfg.activity);
      if (activity < 0) {
        throw std::runtime_error(metricLabel(e.name) + "unknown activity '" +
                                 e.cfg.activity + "'.");
      }
      e.activity_index = static_cast<int16_t>(activity);
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
  }
  resolved_ = true;
}

bool ObservedMetrics::needsVenueParents() const {
  for (size_t i : active_)
    if (entries_[i].scope == MetricScope::ParentVenue) return true;
  return false;
}

void ObservedMetrics::buildVenueParents(WorldState& world) const {
  if (!needsVenueParents()) return;

  // A venue and its parent live on the same rank, so each rank lists the
  // parents of its own venues. Together the ranks hold every venue, and each
  // rank ends with the whole index.
  std::vector<int64_t> pairs;
  int64_t max_id = static_cast<int64_t>(world.venue_type_by_id.size()) - 1;
  for (const Venue& venue : world.venues) {
    max_id = std::max<int64_t>(max_id, venue.id);
    if (venue.parent_id >= 0) {
      pairs.push_back(venue.id);
      pairs.push_back(venue.parent_id);
    }
  }

#ifdef USE_MPI
  const auto mpi = mpi_runtime::state();
  if (mpi.active && mpi.size > 1) {
    MPI_Allreduce(MPI_IN_PLACE, &max_id, 1, MPI_INT64_T, MPI_MAX,
                  MPI_COMM_WORLD);
    pairs = allGatherInt64(pairs, mpi.size);
  }
#endif

  world.venue_parent_by_id.assign(static_cast<size_t>(max_id + 1), -1);
  for (size_t k = 0; k + 1 < pairs.size(); k += 2) {
    world.venue_parent_by_id[static_cast<size_t>(pairs[k])] =
        static_cast<VenueId>(pairs[k + 1]);
  }
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

MetricScope ObservedMetrics::scope(const std::string& name) const {
  return find(name).scope;
}

const std::string& ObservedMetrics::activityOf(const std::string& name) const {
  return find(name).cfg.activity;
}

int16_t ObservedMetrics::activityIndexOf(const std::string& name) const {
  return find(name).activity_index;
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

void ObservedMetrics::unitsOf(const Entry& entry, const Person& person,
                              std::vector<VenueId>& out) const {
  out.clear();
  // Registrations are static world data, so a dead person still has theirs.
  for (const auto& registration :
       world_->getActivityVenues(person, entry.activity_index)) {
    const VenueId venue = registration.first;
    out.push_back(entry.scope == MetricScope::ParentVenue
                      ? world_->parentUnitOf(venue)
                      : venue);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
}

void ObservedMetrics::count(const Person& person, const Observation& obs) {
  for (size_t i : active_) {
    Entry& e = entries_[i];
    if (!e.source->counts(obs, e.stage_id)) continue;
    if (!e.perUnit()) {
      ++e.local;
      continue;
    }
    unitsOf(e, person, scratch_units_);
    for (VenueId unit : scratch_units_) ++e.local_units[unit];
  }
}

void ObservedMetrics::onInfection(const Person& person, uint16_t symptom_id) {
  count(person, {ObservationKind::Start, symptom_id, 0});
}

void ObservedMetrics::onTransition(const Person& person,
                                   uint16_t previous_symptom_id,
                                   uint16_t symptom_id) {
  count(person, {ObservationKind::Transition, symptom_id, previous_symptom_id});
}

void ObservedMetrics::onDeath(const Person& person) {
  count(person, {ObservationKind::Death, 0, 0});
}

void ObservedMetrics::onPresent(const Person& person, uint16_t symptom_id) {
  count(person, {ObservationKind::Present, symptom_id, 0});
}

void ObservedMetrics::reduce() {
  if (active_.empty()) return;

  std::vector<size_t> world_entries;
  std::vector<int64_t> sums;
  for (size_t i : active_) {
    if (entries_[i].perUnit()) continue;
    world_entries.push_back(i);
    sums.push_back(entries_[i].local);
  }

  // Integer sums only, so every rank ends with the same totals whatever the
  // process count. A single process already holds the global value.
#ifdef USE_MPI
  const auto mpi = mpi_runtime::state();
  const bool parallel = mpi.active && mpi.size > 1;
  if (parallel && !sums.empty()) {
    MPI_Allreduce(MPI_IN_PLACE, sums.data(), static_cast<int>(sums.size()),
                  MPI_INT64_T, MPI_SUM, MPI_COMM_WORLD);
  }
#endif
  for (size_t k = 0; k < world_entries.size(); ++k) {
    Entry& e = entries_[world_entries[k]];
    e.reduced = e.source->cumulative() ? e.reduced + sums[k] : sums[k];
    e.local = 0;
  }

  // Metrics counted per unit. Each rank lists (metric, unit, count) for the
  // units it has seen; the lists are gathered whole, so every rank adds up the
  // same integers and ends with the same per unit totals.
  std::vector<int64_t> triples;
  bool any_unit_metric = false;
  for (size_t k = 0; k < active_.size(); ++k) {
    const Entry& e = entries_[active_[k]];
    if (!e.perUnit()) continue;
    any_unit_metric = true;
    for (const auto& [unit, count] : e.local_units) {
      triples.insert(triples.end(), {static_cast<int64_t>(k),
                                     static_cast<int64_t>(unit), count});
    }
  }
  if (!any_unit_metric) return;

#ifdef USE_MPI
  if (parallel) triples = allGatherInt64(triples, mpi.size);
#endif

  std::vector<std::map<VenueId, int64_t>> today(active_.size());
  for (size_t t = 0; t + 2 < triples.size(); t += 3) {
    today[static_cast<size_t>(triples[t])]
         [static_cast<VenueId>(triples[t + 1])] += triples[t + 2];
  }
  for (size_t k = 0; k < active_.size(); ++k) {
    Entry& e = entries_[active_[k]];
    if (!e.perUnit()) continue;
    e.changed.clear();
    if (!e.source->cumulative()) e.units.clear();
    for (const auto& [unit, count] : today[k]) {
      e.changed.push_back(unit);
      e.units[unit] += count;
    }
    e.local_units.clear();
  }
}

int64_t ObservedMetrics::value(const std::string& name) const {
  const Entry& e = find(name);
  if (e.perUnit()) {
    throw std::runtime_error(metricLabel(name) +
                             "is counted per unit; read it with unitValue.");
  }
  return e.reduced;
}

int64_t ObservedMetrics::localCount(const std::string& name) const {
  return find(name).local;
}

int64_t ObservedMetrics::unitValue(const std::string& name,
                                   VenueId unit) const {
  const Entry& e = find(name);
  if (!e.perUnit()) {
    throw std::runtime_error(metricLabel(name) +
                             "has scope 'world'; read it with value.");
  }
  const auto it = e.units.find(unit);
  return it == e.units.end() ? 0 : it->second;
}

const std::vector<VenueId>& ObservedMetrics::changedUnits(
    const std::string& name) const {
  const Entry& e = find(name);
  if (!e.perUnit()) {
    throw std::runtime_error(metricLabel(name) +
                             "has scope 'world'; it has no units.");
  }
  return e.changed;
}

const std::map<VenueId, int64_t>& ObservedMetrics::unitValues(
    const std::string& name) const {
  const Entry& e = find(name);
  if (!e.perUnit()) {
    throw std::runtime_error(metricLabel(name) +
                             "has scope 'world'; it has no units.");
  }
  return e.units;
}

std::vector<MetricSnapshot> ObservedMetrics::snapshot() const {
  std::vector<MetricSnapshot> out;
  for (size_t i : active_) {
    const Entry& e = entries_[i];
    MetricSnapshot s;
    s.name = e.name;
    s.scope = e.scope;
    if (!e.perUnit()) {
      s.value = e.reduced;
    } else {
      s.units.assign(e.units.begin(), e.units.end());
    }
    out.push_back(std::move(s));
  }
  return out;
}

void ObservedMetrics::restoreSnapshot(
    const std::vector<MetricSnapshot>& stored) {
  std::vector<std::string> stored_names;
  for (const auto& s : stored) stored_names.push_back(s.name);
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
  for (const auto& s : stored) {
    Entry& e = find(s.name);
    if (e.scope != s.scope) {
      throw std::runtime_error(metricLabel(s.name) +
                               "the checkpoint counted it with a different "
                               "scope than this run; a counter cannot be "
                               "rebuilt under another scope.");
    }
    e.reduced = s.value;
    e.local = 0;
    e.local_units.clear();
    e.units.clear();
    e.changed.clear();
    for (const auto& [unit, count] : s.units) e.units[unit] = count;
  }
}

}  // namespace june
