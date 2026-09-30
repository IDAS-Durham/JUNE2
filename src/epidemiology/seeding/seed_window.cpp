#include "epidemiology/seeding/seed_window.h"

#include "core/config.h"
#include "utils/time_utils.h"

namespace june {

namespace {

const std::vector<TimeSlot>& slotsOnDay(const ScheduleConfig& schedule,
                                        int day) {
  return *schedule.schedule_types[0]
              .slots_by_day_type_idx[schedule.getDayTypeIndex(day)];
}

long long slotStartMinutes(const ScheduleConfig& schedule,
                           const std::string& start_date, int day,
                           size_t slot_index) {
  const std::string date = formatDate(addDays(parseDate(start_date), day));
  return parseDateTimeMinutes(date + " " +
                              slotsOnDay(schedule, day)[slot_index].start);
}

}  // namespace

SeedWindow seedWindowForSlot(const ScheduleConfig& schedule,
                             const std::string& start_date, int day,
                             size_t slot_index) {
  const long long up_to_minutes =
      slotStartMinutes(schedule, start_date, day, slot_index);
  if (slot_index > 0) {
    return {slotStartMinutes(schedule, start_date, day, slot_index - 1),
            up_to_minutes};
  }
  if (day > 0) {
    const size_t last_slot_index = slotsOnDay(schedule, day - 1).size() - 1;
    return {slotStartMinutes(schedule, start_date, day - 1, last_slot_index),
            up_to_minutes};
  }
  // The run's first slot: nothing precedes it, so only its own start counts.
  return {up_to_minutes - 1, up_to_minutes};
}

}  // namespace june
