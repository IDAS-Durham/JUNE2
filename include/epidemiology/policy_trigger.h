#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/types.h"
#include "epidemiology/observed_metrics.h"

namespace june {

// The state of a trigger at one unit: whether it holds the Policy in force
// there, and one (day, value) pair. While open the pair holds the baseline
// (0 at the start, then the value at the last reopening). While closed it
// holds the anchor: the last time the metric grew beyond the tolerance, or the
// moment of closing if it has not.
struct TriggerCell {
  bool closed = false;
  int64_t anchor_day = 0;
  int64_t anchor_value = 0;

  bool operator==(const TriggerCell&) const = default;
};

// Ties a Policy's activation to an observed metric. "Closed" means the trigger
// holds the Policy in force, the way a venue closes; "open" means it does not.
//
// The state is read once per day, after the metric is reduced, so every rank
// reaches the same answer from the same integers. A world scope trigger keeps
// one cell, the inherited one. A trigger on a metric counted per unit keeps one
// cell per unit it has touched, and a bit per unit for the closed ones so the
// per slot question "is this unit closed" is a single lookup.
struct PolicyTrigger : TriggerCell {
  // Configuration. deactivate_at belongs to current metrics, calm_days and the
  // two tolerances to cumulative ones; the loader rejects the other mix.
  std::string metric;
  int64_t activate_at = 0;
  std::optional<int64_t> deactivate_at;
  int64_t calm_days = 0;
  int64_t tolerance = 0;
  int64_t tolerance_pct = 0;  // whole percent of the anchor value
  bool cumulative = false;    // taken from the metric's source at load
  MetricScope scope = MetricScope::World;  // taken from the metric at load

  bool perUnit() const { return scope != MetricScope::World; }

  // One evaluation of the world scope state. `day` is the day that just ended
  // and `value` the reduced metric at its end.
  void step(int64_t day, int64_t value) { step(*this, day, value); }
  // The same rule applied to any cell.
  void step(TriggerCell& cell, int64_t day, int64_t value) const;

  // One evaluation at one unit. A cell is kept only while it differs from the
  // untouched state, so a unit that was never close to the threshold costs
  // nothing.
  void stepUnit(VenueId unit, int64_t day, int64_t value);
  bool unitClosed(VenueId unit) const;
  // Units whose cell says closed, in unit order.
  std::vector<VenueId> closedUnits() const;
  const std::map<VenueId, TriggerCell>& unitCells() const { return cells_; }
  // Replace every unit cell, as a checkpoint restore does.
  void restoreUnits(const std::vector<std::pair<VenueId, TriggerCell>>& cells);

 private:
  void setUnitBit(VenueId unit, bool closed);

  std::map<VenueId, TriggerCell> cells_;
  std::vector<uint64_t> closed_bits_;  // indexed by unit id
};

}  // namespace june
