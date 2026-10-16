#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace june {

// Ties a Policy's activation to an observed metric. "Closed" means the trigger
// holds the Policy in force, the way a venue closes; "open" means it does not.
//
// The state is read once per day, after the metric is reduced, so every rank
// reaches the same answer from the same integers.
struct PolicyTrigger {
  // Configuration. deactivate_at belongs to current metrics, calm_days and the
  // two tolerances to cumulative ones; the loader rejects the other mix.
  std::string metric;
  int64_t activate_at = 0;
  std::optional<int64_t> deactivate_at;
  int64_t calm_days = 0;
  int64_t tolerance = 0;
  int64_t tolerance_pct = 0;  // whole percent of the anchor value
  bool cumulative = false;    // taken from the metric's source at load

  // State. While open, anchor_value is the baseline: 0 at the start, then the
  // value at the last reopening. While closed, (anchor_day, anchor_value) is
  // the last time the metric grew beyond the tolerance, or the moment of
  // closing if it has not.
  bool closed = false;
  int64_t anchor_day = 0;
  int64_t anchor_value = 0;

  // One evaluation. `day` is the day that just ended and `value` the reduced
  // metric at its end.
  void step(int64_t day, int64_t value);
};

}  // namespace june
