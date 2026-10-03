#include "loaders/catchment_rule_loader.h"

#include <stdexcept>
#include <string>

#include "utils/filtered_csv.h"

namespace june {

constexpr int kExpectedColumns = 2;

std::unordered_map<int32_t, std::vector<GeoUnitId>> CatchmentRuleLoader::parse(
    std::istream& input, const std::string& source_name) {
  std::unordered_map<int32_t, std::vector<GeoUnitId>> rules;

  const csv::FilteredTable table = csv::loadFilteredCSV(input, source_name);
  if (static_cast<int>(table.value_columns.size()) != kExpectedColumns ||
      table.value_columns[0] != "catchment_rule_id" ||
      table.value_columns[1] != "geo_unit_id") {
    throw std::runtime_error(source_name +
                             ": expected columns catchment_rule_id, "
                             "geo_unit_id");
  }

  for (size_t i = 0; i < table.rows.size(); ++i) {
    const int line_number = static_cast<int>(i) + 2;
    const auto& row = table.rows[i];
    try {
      const auto rule_it = row.values.find("catchment_rule_id");
      const auto geo_unit_it = row.values.find("geo_unit_id");
      if (rule_it == row.values.end() || geo_unit_it == row.values.end()) {
        throw std::runtime_error("expected 2 columns");
      }
      int32_t rule_id = std::stoi(rule_it->second);
      GeoUnitId geo_unit_id =
          static_cast<GeoUnitId>(std::stoi(geo_unit_it->second));
      rules[rule_id].push_back(geo_unit_id);
    } catch (const std::exception& e) {
      throw std::runtime_error(source_name + ":" + std::to_string(line_number) +
                               ": " + e.what());
    }
  }

  return rules;
}

}  // namespace june
