#pragma once

#include <cstddef>
#include <string>

namespace june {

struct ScheduleConfig;

// The Seed Dates one slot start applies: after the previous slot start in the
// run, up to and including this one. Minutes since the Julian Day epoch.
// Consecutive slots' windows meet without overlap, so each seed fires once.
struct SeedWindow {
  long long after_minutes;
  long long up_to_minutes;

  bool contains(long long date_minutes) const {
    return after_minutes < date_minutes && date_minutes <= up_to_minutes;
  }
};

// The window of slot `slot_index` on run day `day`, the run starting on
// `start_date` ("YYYY-MM-DD"). Derived from the schedule alone, so a resumed
// run gets the same windows as an uninterrupted one.
SeedWindow seedWindowForSlot(const ScheduleConfig& schedule,
                             const std::string& start_date, int day,
                             size_t slot_index);

}  // namespace june
