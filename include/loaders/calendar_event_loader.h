#pragma once

#include <istream>
#include <string>
#include <vector>

#include "epidemiology/calendar_event.h"

namespace june {

class WorldState;

// Parses a calendar-events CSV (filter.*-convention) into a day-indexed table.
// Required columns: calendar_event_id, date, schedule_name,
//   hosting_geo_unit_id, venue_type_name, catchment_rule_id, duration_days,
//   compliance_rate, category.
// Optional filter.* columns are parsed into CalendarEvent::attendee_filters.
// `duration_days` may be blank (defaults to 1).
// `date` is "YYYY-MM-DD"; start_day = daysBetween(start_date, date).
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
