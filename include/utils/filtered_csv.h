#pragma once

#include <istream>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/config.h"  // SelectionCriterion

namespace june {
namespace csv {

/// A single parsed row of a filter.*-convention CSV.
struct FilteredRow {
  /// Conjunction of criteria drawn from this row's non-empty filter.* cells.
  std::vector<SelectionCriterion> criteria;
  /// All non-filter columns, keyed by header name, values kept as raw strings
  /// so the caller can parse into whatever type it needs.
  std::unordered_map<std::string, std::string> values;
};

/// Parsed contents of a filter.*-convention CSV.
struct FilteredTable {
  /// Header names for every column that did NOT start with "filter.".
  std::vector<std::string> value_columns;
  std::vector<FilteredRow> rows;
};

/// Load a CSV whose keying columns are prefixed "filter." and whose remaining
/// columns are value columns. Comment lines starting with '#' and blank lines
/// are skipped. Throws std::runtime_error on open failure or empty file.
FilteredTable loadFilteredCSV(const std::string& path);

/// Stream form: same as above but reads from an open stream. `source_name` is
/// used in error messages. Throws std::runtime_error on empty/comment-only input.
FilteredTable loadFilteredCSV(std::istream& input, const std::string& source_name);

}  // namespace csv
}  // namespace june
