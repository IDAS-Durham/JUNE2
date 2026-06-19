#pragma once

#include <istream>
#include <string>
#include <vector>

#include "epidemiology/calendar_event.h"

namespace june {

class WorldState;

// Parses a fixed-schema calendar-events CSV into a day-indexed table.
// Columns (in order):
//   calendar_event_id, date, schedule_name, venue_id, subset_index,
//   compliance_rate, category
// `date` is "YYYY-MM-DD"; start_day = daysBetween(start_date, date).
// `schedule_name` is resolved to an index via world.getScheduleTypeIndex.
// Rows outside [0, num_sim_days) are skipped with a warning. Throws
// std::runtime_error on a malformed row or an unknown schedule_name.
class CalendarEventLoader {
 public:
  static std::vector<std::vector<CalendarEvent>> load(const std::string& path,
                                                      const WorldState& world,
                                                      const std::string& start_date,
                                                      int num_sim_days);

  // Stream form, for testing without touching the filesystem.
  static std::vector<std::vector<CalendarEvent>> parse(std::istream& input,
                                                       const WorldState& world,
                                                       const std::string& start_date,
                                                       int num_sim_days,
                                                       const std::string& source_name);
};

}  // namespace june
