#include "loaders/calendar_event_loader.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "core/world_state.h"
#include "utils/time_utils.h"

namespace june {

namespace {

constexpr int kExpectedColumns = 7;

std::string trim(const std::string& s) {
  size_t begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return "";
  size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

std::vector<std::string> splitCsvLine(const std::string& line) {
  std::vector<std::string> fields;
  std::stringstream ss(line);
  std::string field;
  while (std::getline(ss, field, ',')) fields.push_back(trim(field));
  return fields;
}

}  // namespace

std::vector<std::vector<CalendarEvent>> CalendarEventLoader::parse(
    std::istream& input, const WorldState& world,
    const std::string& start_date, int num_sim_days,
    const std::string& source_name) {
  std::vector<std::vector<CalendarEvent>> events_by_day(
      num_sim_days > 0 ? num_sim_days : 0);

  std::tm sim_start_tm = parseDate(start_date);

  std::string line;
  int line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    std::string trimmed = trim(line);
    if (trimmed.empty()) continue;
    // Skip an optional header row.
    if (line_number == 1 && trimmed.rfind("calendar_event_id", 0) == 0) continue;

    std::vector<std::string> fields = splitCsvLine(line);
    if (static_cast<int>(fields.size()) != kExpectedColumns) {
      throw std::runtime_error(source_name + ":" + std::to_string(line_number) +
                               ": expected " + std::to_string(kExpectedColumns) +
                               " columns, got " +
                               std::to_string(fields.size()));
    }

    CalendarEvent event;
    try {
      event.calendar_event_id = std::stoi(fields[0]);
      std::tm event_tm = parseDate(fields[1]);
      event.start_day = daysBetween(sim_start_tm, event_tm);
      const std::string& schedule_name = fields[2];
      int schedule_idx = world.getScheduleTypeIndex(schedule_name);
      if (schedule_idx < 0) {
        throw std::runtime_error("unknown schedule_name '" + schedule_name +
                                 "'");
      }
      event.schedule_type_idx = static_cast<int16_t>(schedule_idx);
      event.venue_id = static_cast<VenueId>(std::stoll(fields[3]));
      event.subset_index = static_cast<SubsetIndex>(std::stoi(fields[4]));
      event.compliance_rate = std::stof(fields[5]);
      event.category = fields[6];
    } catch (const std::exception& parse_error) {
      throw std::runtime_error(source_name + ":" + std::to_string(line_number) +
                               ": " + parse_error.what());
    }

    if (event.start_day < 0 || event.start_day >= num_sim_days) {
      std::cerr << "WARNING: " << source_name << ":" << line_number
                << ": event date " << fields[1] << " (day " << event.start_day
                << ") is outside the simulation window [0, " << num_sim_days
                << "); skipping" << std::endl;
      continue;
    }

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
