#pragma once

#include <istream>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/types.h"

namespace june {

// Parses catchment_rules.csv (long format) into a rule-id -> geo_unit list map.
// Columns: catchment_rule_id (int32_t), geo_unit_id (GeoUnitId / int32_t).
// Throws std::runtime_error on malformed rows.
class CatchmentRuleLoader {
 public:
  static std::unordered_map<int32_t, std::vector<GeoUnitId>> load(
      const std::string& path);

  static std::unordered_map<int32_t, std::vector<GeoUnitId>> parse(
      std::istream& input, const std::string& source_name);
};

}  // namespace june
