#include "utils/filtered_csv.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#include "utils/filtering.h"

namespace june {
namespace csv {

namespace {

std::string trim(const std::string& s) {
  auto first = s.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  auto last = s.find_last_not_of(" \t\r\n");
  return s.substr(first, last - first + 1);
}

std::vector<std::string> splitCSVLine(const std::string& line) {
  std::vector<std::string> fields;
  std::istringstream lss(line);
  std::string f;
  while (std::getline(lss, f, ',')) {
    fields.push_back(trim(f));
  }
  return fields;
}

}  // namespace

FilteredTable loadFilteredCSV(std::istream& input,
                              const std::string& source_name) {
  std::string header_line;
  {
    bool found = false;
    while (std::getline(input, header_line)) {
      auto fnw = header_line.find_first_not_of(" \t\r\n");
      if (fnw != std::string::npos && header_line[fnw] != '#') {
        found = true;
        break;
      }
    }
    if (!found)
      throw std::runtime_error("Filtered CSV is empty or all comments: " +
                               source_name);
  }

  std::vector<std::string> headers = splitCSVLine(header_line);
  auto filter_cols = filtering::findFilterColumns(headers);

  std::unordered_set<int> filter_idx_set;
  for (const auto& pc : filter_cols) filter_idx_set.insert(pc.first);

  FilteredTable table;
  std::vector<std::pair<int, std::string>> value_cols;
  for (int i = 0; i < (int)headers.size(); ++i) {
    if (headers[i].empty()) continue;
    if (filter_idx_set.count(i)) continue;
    value_cols.push_back({i, headers[i]});
    table.value_columns.push_back(headers[i]);
  }

  std::string line;
  while (std::getline(input, line)) {
    auto first = line.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) continue;
    if (line[first] == '#') continue;

    std::vector<std::string> fields = splitCSVLine(line);
    FilteredRow row;
    row.criteria = filtering::parseCriteriaFromRow(fields, filter_cols);
    for (const auto& [col_idx, name] : value_cols) {
      if (col_idx < (int)fields.size()) {
        row.values[name] = fields[col_idx];
      }
    }
    table.rows.push_back(std::move(row));
  }

  return table;
}

FilteredTable loadFilteredCSV(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    throw std::runtime_error("Failed to open filtered CSV: " + path);
  }
  return loadFilteredCSV(file, path);
}

}  // namespace csv
}  // namespace june
