#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace june {

class Disease;
class WorldState;

// What happened to an infected person that a metric source may want to count.
// Present is the daily look at who is infected now, Start is the stage a person
// is placed in at the moment of infection, Transition is one replayed stage
// change, Death is the moment a person is marked dead.
enum class ObservationKind : uint8_t { Present, Start, Transition, Death };

struct Observation {
  ObservationKind kind = ObservationKind::Present;
  uint16_t symptom_id = 0;  // Present: current. Start, Transition: entered.
  uint16_t previous_symptom_id = 0;  // Transition only
};

// One way of turning observations into a count. A source is either current (a
// function of present state, read from Present observations) or cumulative (an
// event counter that only rises). That comes from the source, never from YAML.
class MetricSource {
 public:
  virtual ~MetricSource() = default;
  virtual bool cumulative() const = 0;
  virtual bool needsStage() const = 0;
  virtual bool counts(const Observation& obs, uint16_t stage_id) const = 0;
};

// Registry lookup by source name. Null when the name is not registered.
const MetricSource* findMetricSource(const std::string& name);
std::vector<std::string> metricSourceNames();

enum class MetricScope : uint8_t { World, Venue };

// One declared metric, keyed by name in observed_metrics. Fields are the raw
// YAML strings; an empty string means the field was not given.
struct ObservedMetricConfig {
  std::string source;
  std::string scope;
  std::string activity;  // required iff scope is venue
  std::string stage;     // required iff the source needs a stage
};

// The declared metrics and their running counts.
//
// Nothing is evaluated for a metric until require() marks it as referenced by
// a triggered Policy. A run with no referenced metric never touches the hooks,
// the daily scan or the reduce.
class ObservedMetrics {
 public:
  // Syntactic checks only: the source, scope and conditional fields. Throws a
  // descriptive error naming the metric.
  void declare(const std::string& name, const ObservedMetricConfig& cfg);

  // World dependent checks (activity, stage), then rejects scopes that are not
  // built yet. Must run before require() or any counting.
  void resolve(const WorldState& world, const Disease& disease);

  // Mark a declared metric as read by a Policy trigger. Throws when the name
  // was never declared.
  void require(const std::string& name);

  bool anyDeclared() const { return !entries_.empty(); }
  bool anyActive() const { return !active_.empty(); }
  // Does the daily scan of infected people have anything to feed?
  bool tracksPresent() const { return tracks_present_; }

  // Counting hooks, called where the engine already detects the event.
  void onInfection(uint16_t symptom_id);
  void onTransition(uint16_t previous_symptom_id, uint16_t symptom_id);
  void onDeath();
  void onPresent(uint16_t symptom_id);

  // Sum every active metric across ranks (one integer MPI_Allreduce) and fold
  // the result in: a current metric takes the sum, a cumulative one adds it.
  // Collective; every rank must call it the same days.
  void reduce();

  // Reduced value as of the last reduce(). Throws for an unknown name.
  int64_t value(const std::string& name) const;
  // The rank local part not yet reduced (for tests and diagnostics).
  int64_t localCount(const std::string& name) const;

  // Active metrics in name order, with their reduced values. This is what a
  // checkpoint stores.
  std::vector<std::pair<std::string, int64_t>> activeValues() const;
  // Put stored values back. The stored names must be exactly the active
  // names; a mismatch in either direction throws.
  void restoreActiveValues(
      const std::vector<std::pair<std::string, int64_t>>& stored);

 private:
  struct Entry {
    std::string name;
    ObservedMetricConfig cfg;
    const MetricSource* source = nullptr;
    MetricScope scope = MetricScope::World;
    uint16_t stage_id = 0;
    bool active = false;
    int64_t local = 0;
    int64_t reduced = 0;
  };

  Entry& find(const std::string& name);
  const Entry& find(const std::string& name) const;
  // World scope is one total, so there is one counter per metric. A level
  // scope would key the counter by the unit a person counts toward, here.
  void count(const Observation& obs);

  std::vector<Entry> entries_;  // sorted by name, so every rank agrees
  std::vector<size_t> active_;  // indices into entries_, in name order
  bool tracks_present_ = false;
  bool resolved_ = false;
};

}  // namespace june
