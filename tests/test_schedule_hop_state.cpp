#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "core/types.h"
#include "doctest.h"

using namespace june;

// Isolated unit tests for the ScheduleHop value type. No WorldState, config or
// ActivityManager — the state machine is exercised through its public interface
// alone.

TEST_CASE("begin then advance completes after one full cycle") {
  ScheduleHop hop = ScheduleHop::begin(/*hop_idx=*/2, /*return_idx=*/-1);
  CHECK(hop.isActive());
  const int16_t n = 3;
  CHECK_FALSE(hop.advanceAndCheckComplete(n));  // progress 0 -> 1
  CHECK_FALSE(hop.advanceAndCheckComplete(n));  // 1 -> 2
  CHECK(hop.advanceAndCheckComplete(n));        // 2 -> 3, cycle complete
}

TEST_CASE("repeats_remaining defers completion until exhausted") {
  ScheduleHop hop = ScheduleHop::begin(1, -1, /*repeats=*/2);
  const int16_t n = 2;
  // Cycle 1: completes a full cycle but two repeats remain -> 2 -> 1, no return.
  CHECK_FALSE(hop.advanceAndCheckComplete(n));
  CHECK_FALSE(hop.advanceAndCheckComplete(n));
  CHECK(hop.repeats_remaining == 1);
  // Cycle 2: 1 -> 0, still no return.
  CHECK_FALSE(hop.advanceAndCheckComplete(n));
  CHECK_FALSE(hop.advanceAndCheckComplete(n));
  CHECK(hop.repeats_remaining == 0);
  // Cycle 3: no repeats left -> completes.
  CHECK_FALSE(hop.advanceAndCheckComplete(n));
  CHECK(hop.advanceAndCheckComplete(n));
}

TEST_CASE("single-slot zero-repeat hop completes on first advance") {
  // Regression guard for the immediate-onset n = 1 edge: one advance must
  // report completion, with no spurious repeat decrement.
  ScheduleHop hop = ScheduleHop::begin(0, -1, /*repeats=*/0);
  CHECK(hop.advanceAndCheckComplete(1));
  CHECK(hop.repeats_remaining == 0);
}

TEST_CASE("effectiveReturnSchedule falls back to permanent when unset") {
  ScheduleHop implicit = ScheduleHop::begin(1, /*return_idx=*/-1);
  CHECK(implicit.effectiveReturnSchedule(/*permanent=*/7) == 7);

  ScheduleHop explicit_return = ScheduleHop::begin(1, /*return_idx=*/4);
  CHECK(explicit_return.effectiveReturnSchedule(/*permanent=*/7) == 4);
}

TEST_CASE("hopStartDay maps a monotonic slot index back to its start day") {
  const int sim_day = 10;
  const int16_t n = 3;
  CHECK(ScheduleHop::hopStartDay(sim_day, n, 0) == 10);       // first slot, day 0
  CHECK(ScheduleHop::hopStartDay(sim_day, n, n - 1) == 10);   // last slot of day 0
  CHECK(ScheduleHop::hopStartDay(sim_day, n, n) == 9);        // first slot of day 1
  CHECK(ScheduleHop::hopStartDay(sim_day, n, 2 * n - 1) == 9);  // last slot of day 1
}

TEST_CASE("setPermanent activates without touching progress or repeats") {
  ScheduleHop hop;
  hop.setPermanent(5);
  CHECK(hop.isActive());
  CHECK(hop.hopped_schedule_id == 5);
  CHECK(hop.return_schedule_id == -1);
  CHECK(hop.temp_slot_progress == 0);
  CHECK(hop.repeats_remaining == 0);
}

TEST_CASE("clear resets every field to inactive defaults") {
  ScheduleHop hop = ScheduleHop::begin(2, 4, /*repeats=*/3);
  hop.advanceAndCheckComplete(2);  // dirty temp_slot_progress
  hop.clear();
  CHECK_FALSE(hop.isActive());
  CHECK(hop.hopped_schedule_id == -1);
  CHECK(hop.return_schedule_id == -1);
  CHECK(hop.temp_slot_progress == 0);
  CHECK(hop.repeats_remaining == 0);
}
