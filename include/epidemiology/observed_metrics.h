#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/types.h"
#include "core/world_state.h"

namespace june {

class Disease;

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

// World is one total. Venue counts a person toward every venue registered to
// them for the metric's activity. ParentVenue counts them toward the parent of
// each of those venues, or the venue itself when it has none. Either way one
// observation counts a person at most once per unit, whatever the source.
enum class MetricScope : uint8_t { World, Venue, ParentVenue };

// The unit a venue counts toward under a scope: the venue itself, or its parent
// venue (the venue again when it has none). Counting and closing both key on
// it.
inline VenueId unitOf(const WorldState& world, MetricScope scope,
                      VenueId venue) {
  return scope == MetricScope::ParentVenue ? world.parentUnitOf(venue) : venue;
}

// What a checkpoint keeps of one active metric: a world scope total, or for a
// metric counted per unit, its value per unit in unit order (the running total
// of a cumulative metric, the last day's counts of a current one). The day
// after a resume reads the current counts before the next daily scan has run.
struct MetricSnapshot {
  std::string name;
  MetricScope scope = MetricScope::World;
  int64_t value = 0;
  std::vector<std::pair<VenueId, int64_t>> units;
};

// One declared metric, keyed by name in observed_metrics. Fields are the raw
// YAML strings; an empty string means the field was not given.
struct ObservedMetricConfig {
  std::string source;
  std::string scope;
  std::string activity;  // required iff scope is venue or parent_venue
  std::string stage;     // required iff the source needs a stage
};

// The declared metrics and their running counts.
//
// Nothing is evaluated for a metric until require() marks it as referenced by
// a triggered Policy. A run with no referenced metric never touches the hooks,
// the daily scan or the reduce.
class ObservedMetrics {
 public:
  // Checks that need no world: the name, the source, the scope and the fields
  // each combination requires. Throws a descriptive error naming the metric,
  // or std::logic_error when called after resolve().
  void declare(const std::string& name, const ObservedMetricConfig& cfg);

  // World dependent checks (activity, stage). Must run before require() or any
  // counting, and the world must outlive the metrics.
  void resolve(const WorldState& world, const Disease& disease);

  // Does some active metric count per parent venue? Then the world needs its
  // parent index before the first count.
  bool needsVenueParents() const;
  // Fill world.venue_parent_by_id from every rank's venues. Collective when
  // needed: every rank must call it. Does nothing otherwise.
  void buildVenueParents(WorldState& world) const;

  // Mark a declared metric as read by a Policy trigger. Throws when the name
  // was never declared, or std::logic_error before resolve().
  void require(const std::string& name);

  // Throws, with `where` in front and the declared names listed, unless the
  // name was declared. For a caller that knows which Policy asked.
  void requireDeclared(const std::string& name, const std::string& where) const;

  // Questions a Policy trigger asks of a declared metric. All throw for an
  // unknown name.
  bool isCumulative(const std::string& name) const;
  MetricScope scope(const std::string& name) const;
  const std::string& activityOf(const std::string& name) const;
  // The activity's index in the world, as resolve() found it. -1 for a world
  // scope metric, and before resolve().
  int16_t activityIndexOf(const std::string& name) const;

  bool anyActive() const { return !active_.empty(); }
  // Does the daily scan of infected people have anything to feed?
  bool tracksPresent() const { return tracks_present_; }

  // Counting hooks, called where the engine already detects the event. Call
  // them on the rank that owns the person, so each event is counted once across
  // ranks. The person's registrations for the metric's activity say which units
  // it counts toward.
  void onInfection(const Person& person, uint16_t symptom_id);
  void onTransition(const Person& person, uint16_t previous_symptom_id,
                    uint16_t symptom_id);
  void onDeath(const Person& person);
  void onPresent(const Person& person, uint16_t symptom_id);

  // Sum every active metric across ranks and fold the result in: a current
  // metric takes the sum, a cumulative one adds it. World scope metrics share
  // one integer MPI_Allreduce. Metrics counted per unit swap their sparse
  // (unit, count) lists in one more exchange for all of them, because the units
  // a rank has seen differ from rank to rank; every rank then merges the same
  // integers. Collective; every rank must call it the same days.
  void reduce();

  // Reduced value of a world scope metric as of the last reduce(). Throws for
  // an unknown name, or for a metric counted per unit (use unitValue).
  int64_t value(const std::string& name) const;

  // A metric counted per unit, as of the last reduce(): the value at one unit
  // (0 for a unit with no count), and the units the last reduce saw any count
  // for, in unit order. Throw for a world scope metric.
  int64_t unitValue(const std::string& name, VenueId unit) const;
  const std::vector<VenueId>& changedUnits(const std::string& name) const;
  // Every unit with a count as of the last reduce(), in unit order.
  const std::map<VenueId, int64_t>& unitValues(const std::string& name) const;

  // Active metrics in name order. This is what a checkpoint stores.
  std::vector<MetricSnapshot> snapshot() const;
  // Put stored values back. The stored names and scopes must be exactly the
  // active ones; a mismatch in either direction throws.
  void restoreSnapshot(const std::vector<MetricSnapshot>& stored);

 private:
  struct Entry {
    std::string name;
    ObservedMetricConfig cfg;
    // The source's rule, copied from the source table when declared.
    bool cumulative = false;
    bool (*counts)(const Observation& obs, uint16_t stage_id) = nullptr;
    MetricScope scope = MetricScope::World;
    uint16_t stage_id = 0;
    int16_t activity_index = -1;
    bool active = false;
    int64_t local = 0;
    int64_t reduced = 0;
    // Scopes counted per unit. local_units is this rank's part since the last
    // reduce; units is the reduced total (cumulative) or the last reduce's
    // counts (current); changed lists the units the last reduce saw counts for.
    std::unordered_map<VenueId, int64_t> local_units;
    std::map<VenueId, int64_t> units;
    std::vector<VenueId> changed;

    bool perUnit() const { return scope != MetricScope::World; }
  };

  Entry& find(const std::string& name);
  const Entry& find(const std::string& name) const;
  // The error for a name that was never declared, listing the ones that were.
  [[noreturn]] void throwUnknown(const std::string& name,
                                 const std::string& where) const;
  // The scope key: which units this person counts toward for this metric,
  // sorted and without repeats, so a person counts once per unit.
  void unitsOf(const Entry& entry, const Person& person,
               std::vector<VenueId>& out) const;
  void count(const Person& person, const Observation& obs);

  std::vector<Entry> entries_;  // sorted by name, so every rank agrees
  std::vector<size_t> active_;  // indices into entries_, in name order
  bool tracks_present_ = false;
  bool resolved_ = false;
  const WorldState* world_ = nullptr;
  std::vector<VenueId> scratch_units_;
};

}  // namespace june
