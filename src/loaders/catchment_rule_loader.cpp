#include "loaders/catchment_rule_loader.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace june {

namespace {

constexpr int kExpectedColumns = 2;

std::string trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

}  // namespace

std::unordered_map<int32_t, std::vector<GeoUnitId>> CatchmentRuleLoader::parse(
    std::istream& input, const std::string& source_name) {
  std::unordered_map<int32_t, std::vector<GeoUnitId>> rules;

  std::string line;
  int line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    std::string trimmed = trim(line);
    if (trimmed.empty()) continue;
    if (line_number == 1 && trimmed.rfind("catchment_rule_id", 0) == 0) continue;

    std::vector<std::string> fields;
    std::stringstream ss(line);
    std::string field;
    while (std::getline(ss, field, ',')) fields.push_back(trim(field));

    if (static_cast<int>(fields.size()) != kExpectedColumns) {
      throw std::runtime_error(source_name + ":" + std::to_string(line_number) +
                               ": expected 2 columns, got " +
                               std::to_string(fields.size()));
    }

    try {
      int32_t rule_id = std::stoi(fields[0]);
      GeoUnitId geo_unit_id = static_cast<GeoUnitId>(std::stoi(fields[1]));
      rules[rule_id].push_back(geo_unit_id);
    } catch (const std::exception& e) {
      throw std::runtime_error(source_name + ":" + std::to_string(line_number) +
                               ": " + e.what());
    }
  }

  return rules;
}

std::unordered_map<int32_t, std::vector<GeoUnitId>> CatchmentRuleLoader::load(
    const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    throw std::runtime_error("CatchmentRuleLoader: cannot open '" + path + "'");
  }
  return parse(file, path);
}

}  // namespace june
