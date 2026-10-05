#pragma once

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace june {

// =============================================================================
// Time Utilities
// =============================================================================

// Parse "H:MM" or "HH:MM" to minutes since midnight. The one time-of-day
// parser: schedule slots and seed dates both go through it.
inline int parseTimeToMinutes(const std::string& time_str) {
  const auto is_digit = [](char character) {
    return character >= '0' && character <= '9';
  };
  const size_t colon = time_str.find(':');
  const bool well_formed =
      (colon == 1 || colon == 2) && time_str.size() == colon + 3 &&
      is_digit(time_str[0]) && is_digit(time_str[colon - 1]) &&
      is_digit(time_str[colon + 1]) && is_digit(time_str[colon + 2]);
  if (!well_formed) {
    throw std::runtime_error("Invalid time format: " + time_str +
                             " (expected HH:MM)");
  }

  const int hours = std::stoi(time_str.substr(0, colon));
  const int minutes = std::stoi(time_str.substr(colon + 1));
  if (hours > 23 || minutes > 59) {
    throw std::runtime_error("Invalid time values: " + time_str);
  }

  return hours * 60 + minutes;
}

// Calculate duration in hours between two times
// Handles wraparound (e.g., 21:00 to 08:00 = 11 hours)
inline double calculateDuration(const std::string& start,
                                const std::string& end) {
  int start_min = parseTimeToMinutes(start);
  int end_min = parseTimeToMinutes(end);

  int duration_min;
  if (end_min >= start_min) {
    duration_min = end_min - start_min;
  } else {
    // Wraparound case (crosses midnight)
    duration_min = (24 * 60 - start_min) + end_min;
  }

  return duration_min / 60.0;  // Convert to hours
}

namespace time_detail {

inline std::chrono::year_month_day parseYearMonthDay(std::string_view date) {
  const auto refuse = [&]() {
    throw std::runtime_error("Invalid date format: " + std::string(date) +
                             " (expected YYYY-MM-DD)");
  };
  if (date.size() != 10 || date[4] != '-' || date[7] != '-') refuse();
  for (size_t i = 0; i < date.size(); ++i) {
    if (i == 4 || i == 7) continue;
    if (date[i] < '0' || date[i] > '9') refuse();
  }

  const int year = std::stoi(std::string(date.substr(0, 4)));
  const unsigned month =
      static_cast<unsigned>(std::stoi(std::string(date.substr(5, 2))));
  const unsigned day =
      static_cast<unsigned>(std::stoi(std::string(date.substr(8, 2))));
  const auto result = std::chrono::year{year} / std::chrono::month{month} /
                      std::chrono::day{day};
  if (!result.ok()) refuse();
  return result;
}

inline std::chrono::year_month_day toYearMonthDay(const std::tm& date) {
  return std::chrono::year{date.tm_year + 1900} /
         std::chrono::month{static_cast<unsigned>(date.tm_mon + 1)} /
         std::chrono::day{static_cast<unsigned>(date.tm_mday)};
}

inline std::tm toTm(const std::chrono::year_month_day& date) {
  std::tm result = {};
  result.tm_year = static_cast<int>(date.year()) - 1900;
  result.tm_mon = static_cast<unsigned>(date.month()) - 1;
  result.tm_mday = static_cast<unsigned>(date.day());
  return result;
}

}  // namespace time_detail

// Parse date string "YYYY-MM-DD" to std::tm.
inline std::tm parseDate(const std::string& date_str) {
  return time_detail::toTm(time_detail::parseYearMonthDay(date_str));
}

// Parse "YYYY-MM-DD HH:MM" (hour may drop its leading zero) to minutes since
// the Unix epoch, so any two such moments compare and subtract directly,
// pre-1970 included. Throws on any other format or on an impossible date or
// time.
inline long long parseDateTimeMinutes(const std::string& date_time) {
  const auto refuse = [&]() {
    throw std::invalid_argument("invalid date '" + date_time +
                                "' (expected YYYY-MM-DD HH:MM)");
  };
  const std::string date_pattern = "dddd-dd-dd ";
  if (date_time.size() <= date_pattern.size()) refuse();
  for (size_t i = 0; i < date_pattern.size(); ++i) {
    const bool is_digit = date_time[i] >= '0' && date_time[i] <= '9';
    if (date_pattern[i] == 'd' ? !is_digit : date_time[i] != date_pattern[i])
      refuse();
  }
  std::chrono::year_month_day date;
  try {
    date = time_detail::parseYearMonthDay(date_time.substr(0, 10));
  } catch (const std::runtime_error&) {
    refuse();
  }
  int minutes_since_midnight = 0;
  try {
    minutes_since_midnight =
        parseTimeToMinutes(date_time.substr(date_pattern.size()));
  } catch (const std::runtime_error&) {
    refuse();
  }
  const auto days = std::chrono::sys_days{date}.time_since_epoch().count();
  return days * 1440 + minutes_since_midnight;
}

// Add days to a date
inline std::tm addDays(const std::tm& date, int days) {
  const auto result = std::chrono::year_month_day{
      std::chrono::sys_days{time_detail::toYearMonthDay(date)} +
      std::chrono::days{days}};
  return time_detail::toTm(result);
}

// Format date as "YYYY-MM-DD"
inline std::string formatDate(const std::tm& date) {
  const auto ymd = time_detail::toYearMonthDay(date);
  std::ostringstream result;
  result << std::setfill('0') << std::setw(4) << static_cast<int>(ymd.year())
         << '-' << std::setw(2) << static_cast<unsigned>(ymd.month()) << '-'
         << std::setw(2) << static_cast<unsigned>(ymd.day());
  return result.str();
}

// Calculate number of days between two dates
inline int daysBetween(const std::tm& start, const std::tm& end) {
  const auto start_day =
      std::chrono::sys_days{time_detail::toYearMonthDay(start)};
  const auto end_day = std::chrono::sys_days{time_detail::toYearMonthDay(end)};
  return static_cast<int>((end_day - start_day).count());
}

}  // namespace june
