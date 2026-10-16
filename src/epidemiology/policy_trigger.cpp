#include "epidemiology/policy_trigger.h"

#include <algorithm>

namespace june {

void PolicyTrigger::step(int64_t day, int64_t value) {
  if (!cumulative) {
    if (!closed && value >= activate_at) {
      closed = true;
    } else if (closed && value <= deactivate_at.value()) {
      closed = false;
    }
    return;
  }

  if (!closed) {
    if (value - anchor_value >= activate_at) {
      closed = true;
      anchor_day = day;
      anchor_value = value;
    }
    return;
  }

  const int64_t allowed =
      std::max(tolerance, tolerance_pct * anchor_value / 100);
  if (value - anchor_value > allowed) {
    anchor_day = day;
    anchor_value = value;
  } else if (day - anchor_day >= calm_days) {
    closed = false;
    anchor_value = value;
  }
}

}  // namespace june
