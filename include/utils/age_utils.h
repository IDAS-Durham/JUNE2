#pragma once

#include <string>
#include <vector>

#include "core/config.h"

namespace june {

// =============================================================================
// Canonical 5-year age bands: single source of truth used by both
// getAgeBracketString() (runtime lookups) and normalisedAgeBrackets()
// (CSV loading).  Add or change bands here and both sites update automatically.
// =============================================================================
inline constexpr int kAgeBands[][2] = {
    {0, 4},   {5, 9},   {10, 14}, {15, 19}, {20, 24}, {25, 29}, {30, 34},
    {35, 39}, {40, 44}, {45, 49}, {50, 54}, {55, 59}, {60, 64}, {65, 69},
    {70, 74}, {75, 79}, {80, 84}, {85, 89}, {90, 94}, {95, 99}};
inline constexpr int kNumAgeBands =
    static_cast<int>(sizeof(kAgeBands) / sizeof(kAgeBands[0]));

// Returns the standard 5-year bracket string used in disease lookups, like
// "[10, 14]"
inline std::string getAgeBracketString(int age) {
  for (int i = 0; i < kNumAgeBands; ++i) {
    if (age >= kAgeBands[i][0] && age <= kAgeBands[i][1]) {
      return "[" + std::to_string(kAgeBands[i][0]) + ", " +
             std::to_string(kAgeBands[i][1]) + "]";
    }
  }

  return "[95, 99]";  // Default/Max
}

// Checks if an age fits in a range string like "0-19", "[0, 19]", or "65+"
inline bool matchesAgeRange(int age, const std::string& range_str) {
  if (range_str.empty()) return false;

  // Handle "65+" format
  if (range_str.back() == '+') {
    try {
      int min_age = std::stoi(range_str.substr(0, range_str.size() - 1));
      return age >= min_age;
    } catch (...) {
      return false;
    }
  }

  // Handle "[0, 4]" or "0-4" format
  size_t sep_pos = range_str.find_first_of("-,");
  if (sep_pos != std::string::npos) {
    try {
      size_t start_pos = (range_str[0] == '[') ? 1 : 0;
      int min_age = std::stoi(range_str.substr(start_pos, sep_pos - start_pos));

      size_t end_pos = range_str.find_first_of("]", sep_pos + 1);
      if (end_pos == std::string::npos) end_pos = range_str.size();
      int max_age =
          std::stoi(range_str.substr(sep_pos + 1, end_pos - sep_pos - 1));

      return age >= min_age && age <= max_age;
    } catch (...) {
      return false;
    }
  }

  return false;
}

// Finds the specific age group name from a config list that this age belongs to
inline std::string getAgeGroupName(int age,
                                   const std::vector<AgeGroup>& groups) {
  for (const auto& group : groups) {
    if (age >= group.min_age && age <= group.max_age) {
      return group.name;
    }
  }
  return "unknown";
}

}  // namespace june
