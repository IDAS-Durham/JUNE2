#pragma once

#include <vector>

#include "core/config.h"

// Forward-declare YAML::Node to keep yaml-cpp out of the public include
// surface. Only TUs that actually parse YAML need the full header.
namespace YAML {
class Node;
}  // namespace YAML

namespace june {
namespace config_detail {

// Parse a YAML sequence of `{property, operator, value}` entries into a
// vector of SelectionCriterion. Scalar values use the shared bool -> int ->
// double -> string dispatch; sequence values are validated by the loader's
// local sequence parser.
// Shared by schedule, activity preferences, vaccination, encounters, and
// policy loading.
void parseSelectionCriteria(const YAML::Node& selection_node,
                            std::vector<SelectionCriterion>& out);

}  // namespace config_detail
}  // namespace june
