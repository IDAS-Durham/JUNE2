#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <string>
#include <vector>

#include "core/config.h"
#include "core/world_state.h"
#include "doctest.h"
#include "epidemiology/seeding/seed_window.h"
#include "utils/time_utils.h"

using namespace june;

namespace {

std::vector<TimeSlot> slotsStartingAt(const std::vector<std::string>& starts) {
  std::vector<TimeSlot> slots;
  for (size_t i = 0; i < starts.size(); ++i) {
    TimeSlot slot;
    slot.name = "slot" + std::to_string(i);
    slot.start = starts[i];
    slot.end = i + 1 < starts.size() ? starts[i + 1] : starts[0];
    slots.push_back(slot);
  }
  return slots;
}

// Days alternate workday (08:00, 12:00, 21:00) and rest_day (10:00, 18:00).
ScheduleConfig alternatingSchedule() {
  WorldState world;
  ScheduleConfig schedule;
  schedule.day_type_cycle = {"workday", "rest_day"};
  schedule.day_type_names = {"workday", "rest_day"};
  ScheduleType schedule_type;
  schedule_type.name = "everyone";
  schedule_type.slots_by_day_type["workday"] =
      slotsStartingAt({"08:00", "12:00", "21:00"});
  schedule_type.slots_by_day_type["rest_day"] =
      slotsStartingAt({"10:00", "18:00"});
  schedule.schedule_types.push_back(schedule_type);
  schedule.resolve(world);
  return schedule;
}

long long minutesAt(const std::string& date_time) {
  return parseDateTimeMinutes(date_time);
}

}  // namespace

TEST_CASE("the run's first slot takes only seeds dated at its start") {
  const ScheduleConfig schedule = alternatingSchedule();
  const SeedWindow window = seedWindowForSlot(schedule, "2024-01-01", 0, 0);

  CHECK(window.up_to_minutes == minutesAt("2024-01-01 08:00"));
  CHECK(window.after_minutes == minutesAt("2024-01-01 08:00") - 1);
}

TEST_CASE("a later slot takes seeds after the day's previous slot start") {
  const ScheduleConfig schedule = alternatingSchedule();
  const SeedWindow window = seedWindowForSlot(schedule, "2024-01-01", 0, 2);

  CHECK(window.after_minutes == minutesAt("2024-01-01 12:00"));
  CHECK(window.up_to_minutes == minutesAt("2024-01-01 21:00"));
}

TEST_CASE("a day's first slot takes seeds after the previous day's last start") {
  // Day 1 is a rest day after a workday; day 2 a workday after a rest day.
  // The same windows hold for a run resumed on either day.
  const ScheduleConfig schedule = alternatingSchedule();
  const SeedWindow rest_day = seedWindowForSlot(schedule, "2024-01-31", 1, 0);
  const SeedWindow workday = seedWindowForSlot(schedule, "2024-01-31", 2, 0);

  CHECK(rest_day.after_minutes == minutesAt("2024-01-31 21:00"));
  CHECK(rest_day.up_to_minutes == minutesAt("2024-02-01 10:00"));
  CHECK(workday.after_minutes == minutesAt("2024-02-01 18:00"));
  CHECK(workday.up_to_minutes == minutesAt("2024-02-02 08:00"));
}
