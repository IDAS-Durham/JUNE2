/// @file filtering.cpp
/// @brief Implementation of CSV-driven row filtering for outcome-rate tables
/// and
///        infection seeds.
///
/// See filtering.h for the full API documentation and a description of all
/// supported `filter.*` column types and value formats.
#include "utils/filtering.h"

#include <iostream>
#include <stdexcept>

namespace june {
namespace filtering {

std::vector<SelectionCriterion> parseCriterionFromKeyValue(
    const std::string& key, const std::string& val) {
  std::string property = key;
  // Map legacy alias used in bulk seed CSV headers
  if (property == "age_groups") {
    property = "age";
    std::cout << "  [Config] Mapping property 'age_groups' -> 'age'"
              << std::endl;
  }

  std::vector<SelectionCriterion> results;

  // Support comparison prefixes: ">12", ">=12", "<64", "<=64"
  if (!val.empty() && (val[0] == '<' || val[0] == '>')) {
    std::string op;
    size_t num_start = 1;
    if (val.size() > 1 && val[1] == '=') {
      op = (val[0] == '>') ? ">=" : "<=";
      num_start = 2;
    } else {
      op = (val[0] == '>') ? ">" : "<";
    }
    try {
      SelectionCriterion c;
      c.property_path = property;
      c.operator_type = op;
      c.value = std::stoi(val.substr(num_start));
      return {c};
    } catch (...) {
      // Fall through to other parsers
    }
  }

  // Support ranges like "18-30" or "65-100"
  size_t dash = val.find('-');
  if (dash != std::string::npos && dash > 0 && dash < val.size() - 1) {
    try {
      SelectionCriterion c_min, c_max;
      c_min.property_path = property;
      c_min.operator_type = ">=";
      c_min.value = std::stoi(val.substr(0, dash));

      c_max.property_path = property;
      c_max.operator_type = "<=";
      c_max.value = std::stoi(val.substr(dash + 1));

      results.push_back(c_min);
      results.push_back(c_max);
      return results;
    } catch (...) {
      // Fall through to single-value parse
    }
  }

  SelectionCriterion c;
  c.property_path = property;
  c.operator_type = "==";

  // Try numeric/bool first, fallback to string
  try {
    if (val == "true" || val == "True")
      c.value = true;
    else if (val == "false" || val == "False")
      c.value = false;
    else if (val.find('.') != std::string::npos) {
      c.value = std::stod(val);
    } else {
      c.value = std::stoi(val);
    }
  } catch (...) {
    c.value = val;
  }

  results.push_back(c);
  return results;
}

// The context field `criterion` filters on, or null if it filters on a
// Person property.
static const std::string* contextFactFor(const SelectionCriterion& criterion,
                                         const InfectionContext& ctx) {
  if (criterion.property_path == "infector_symptom") {
    return &ctx.infector_symptom;
  }
  if (criterion.property_path == "transmission_mode") {
    return &ctx.transmission_mode;
  }
  if (criterion.property_path == "infection_source") {
    return &ctx.infection_source;
  }
  return nullptr;
}

// An absent (empty) fact fails every criterion on it, `==` or `!=`, so only
// rows that don't ask for that fact match.
static bool contextFactMatches(const SelectionCriterion& criterion,
                               const std::string& fact) {
  if (fact.empty()) return false;
  const std::string* required = std::get_if<std::string>(&criterion.value);
  if (!required) return false;
  bool equal = (fact == *required);
  if (criterion.operator_type == "==") return equal;
  if (criterion.operator_type == "!=") return !equal;
  return true;
}

bool matchesCriteria(const Person& person, const WorldState* world,
                     const std::vector<SelectionCriterion>& criteria,
                     const InfectionContext& ctx) {
  for (const auto& c : criteria) {
    // #34 owns the cached, world-free evaluation path for the original two
    // context facts. infection_source was added by #40 and is handled here
    // because SelectionCriterion does not resolve that fact from a Person.
    if (c.property_path == "infection_source") {
      const std::string* fact = contextFactFor(c, ctx);
      if (fact == nullptr || !contextFactMatches(c, *fact)) return false;
    } else {
      if (!c.evaluate(person, world, nullptr, &ctx)) return false;
    }
  }
  return true;
}

bool isInfectionContextCriterion(const SelectionCriterion& criterion) {
  return contextFactFor(criterion, InfectionContext{}) != nullptr;
}

std::vector<std::pair<int, std::string>> findFilterColumns(
    const std::vector<std::string>& headers) {
  std::vector<std::pair<int, std::string>> result;
  for (int i = 0; i < (int)headers.size(); ++i) {
    if (headers[i].find("filter.") == 0) {
      result.push_back({i, headers[i].substr(7)});
    }
  }
  return result;
}

std::vector<SelectionCriterion> parseCriteriaFromRow(
    const std::vector<std::string>& fields,
    const std::vector<std::pair<int, std::string>>& filter_cols) {
  std::vector<SelectionCriterion> criteria;
  for (const auto& [col_idx, property_path] : filter_cols) {
    if (col_idx < (int)fields.size() && !fields[col_idx].empty()) {
      auto cs = parseCriterionFromKeyValue(property_path, fields[col_idx]);
      criteria.insert(criteria.end(), cs.begin(), cs.end());
    }
  }
  return criteria;
}

}  // namespace filtering
}  // namespace june
