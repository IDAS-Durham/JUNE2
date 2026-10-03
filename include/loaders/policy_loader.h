#pragma once

#include <cstdint>
#include <string>

#include "epidemiology/policy.h"
#include "epidemiology/transmission_modifiers.h"

namespace YAML {
class Node;
}

namespace june {

namespace PolicyLoader {
// Load policies from YAML file.
void loadPolicies(PolicyManager& policy_manager, const std::string& filename,
                  const std::string& simulation_start_date);

// Load a policy action.
PolicyAction loadPolicyAction(const YAML::Node& node);

bool hasExplicitLocationAction(const YAML::Node& node);

// Load the four date-range keys shared by every policy kind that carries an
// ActiveWindow: start_date > start_time > 0.0, end_date > end_time > no end.
ActiveWindow loadActiveWindow(const YAML::Node& node,
                              const std::string& simulation_start_date);

// Load symptom policies.
void loadSymptomPolicies(PolicyManager& policy_manager, const YAML::Node& node,
                         const std::string& simulation_start_date,
                         const std::string& policies_filename);

// Load temporal policies (lockdowns, etc.).
void loadTemporalPolicies(PolicyManager& policy_manager, const YAML::Node& node,
                          const std::string& simulation_start_date,
                          const std::string& policies_filename);

void loadTransmissionEffects(PolicyManager& policy_manager,
                             const std::string& csv_path,
                             TransmissionPolicyKind policy_kind,
                             uint16_t policy_index, double compliance_rate,
                             const std::string& policy_name,
                             const std::string& policies_filename);
}  // namespace PolicyLoader

}  // namespace june
