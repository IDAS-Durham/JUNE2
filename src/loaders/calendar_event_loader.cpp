#include "loaders/calendar_event_loader.h"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "core/world_state.h"
#include "utils/filtered_csv.h"
#include "utils/time_utils.h"

namespace june {

std::vector<std::vector<CalendarEvent>> CalendarEventLoader::parse(
    std::istream& input, const WorldState& world,
    const std::string& start_date, int num_sim_days,
    const std::string& source_name) {
  std::vector<std::vector<CalendarEvent>> events_by_day(
      num_sim_days > 0 ? num_sim_days : 0);

  std::tm sim_start_tm = parseDate(start_date);

  csv::FilteredTable table;
  try {
    table = csv::loadFilteredCSV(input, source_name);
  } catch (const std::exception& e) {
    throw std::runtime_error(std::string(source_name) + ": " + e.what());
  }

  int row_number = 1;  // 1-indexed for error messages (header is row 1)
  for (const auto& row : table.rows) {
    ++row_number;

    auto get = [&](const std::string& col) -> const std::string& {
      auto it = row.values.find(col);
      if (it == row.values.end()) {
        throw std::runtime_error(source_name + ":" +
                                 std::to_string(row_number) +
                                 ": missing required column '" + col + "'");
      }
      return it->second;
    };

    CalendarEvent event;
    try {
      event.calendar_event_id = std::stoi(get("calendar_event_id"));
      std::tm event_tm = parseDate(get("date"));
      event.start_day = daysBetween(sim_start_tm, event_tm);
      const std::string& schedule_name = get("schedule_name");
      int schedule_idx = world.getScheduleTypeIndex(schedule_name);
      if (schedule_idx < 0) {
        throw std::runtime_error("unknown schedule_name '" + schedule_name + "'");
      }
      event.schedule_type_idx = static_cast<int16_t>(schedule_idx);
      event.hosting_geo_unit_id =
          static_cast<GeoUnitId>(std::stoi(get("hosting_geo_unit_id")));
      event.venue_type_name = get("venue_type_name");
      event.catchment_rule_id = std::stoi(get("catchment_rule_id"));
      const std::string& dur_str = get("duration_days");
      event.duration_days =
          dur_str.empty() ? int16_t{1}
                          : static_cast<int16_t>(std::stoi(dur_str));
      if (event.duration_days < 1) {
        std::cerr << "WARNING: " << source_name << ":" << row_number
                  << ": duration_days=" << event.duration_days
                  << " < 1, clamped to 1\n";
        event.duration_days = 1;
      }
      event.compliance_rate = std::stof(get("compliance_rate"));
      event.category = get("category");
      event.attendee_filters = row.criteria;
    } catch (const std::exception& parse_error) {
      throw std::runtime_error(source_name + ":" + std::to_string(row_number) +
                               ": " + parse_error.what());
    }

    if (event.start_day < 0 || event.start_day >= num_sim_days) {
#ifndef NDEBUG
      std::cerr << "DEBUG: " << source_name << ":" << row_number
                << ": event date (day " << event.start_day
                << ") is outside the simulation window [0, " << num_sim_days
                << "); skipping\n";
#endif
      continue;
    }

    // No candidate_venue_builder / venue_selector installed: the manager
    // derives the catchment pool from hosting_geo_unit_id + venue_type_name and
    // hash-selects by default.
    events_by_day[event.start_day].push_back(std::move(event));
  }

  return events_by_day;
}

std::vector<std::vector<CalendarEvent>> CalendarEventLoader::load(
    const std::string& path, const WorldState& world,
    const std::string& start_date, int num_sim_days) {
  std::ifstream file(path);
  if (!file.is_open()) {
    throw std::runtime_error("CalendarEventLoader: cannot open '" + path + "'");
  }
  return parse(file, world, start_date, num_sim_days, path);
}

}  // namespace june
