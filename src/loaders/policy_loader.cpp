#include "loaders/policy_loader.h"

#include <yaml-cpp/yaml.h>

#include <iostream>
#include <stdexcept>
#include <unordered_set>
#include <vector>

#include "loaders/config_loader_detail.h"
#include "utils/time_utils.h"

namespace june {

namespace PolicyLoader {

void loadPolicies(PolicyManager& policy_manager, const std::string& filename,
                  const std::string& simulation_start_date) {
  try {
    YAML::Node root = YAML::LoadFile(filename);

    if (!root["policies"]) {
      std::cout << "No policies section found in " << filename << std::endl;
      return;
    }

    const YAML::Node& policies = root["policies"];

    if (policies["symptom_policies"]) {
      loadSymptomPolicies(policy_manager, policies["symptom_policies"],
                          simulation_start_date, filename);
    }

    if (policies["temporal_policies"]) {
      loadTemporalPolicies(policy_manager, policies["temporal_policies"],
                           simulation_start_date, filename);
    }
  } catch (const YAML::Exception& e) {
    throw std::runtime_error("Error loading policy file '" + filename +
                             "': " + e.what());
  }
}

PolicyAction loadPolicyAction(const YAML::Node& node) {
  PolicyAction action;
  const auto parseStringSet = [](const YAML::Node& value) {
    std::unordered_set<std::string> result;
    if (value.IsSequence()) {
      for (const auto& item : value) {
        result.insert(item.as<std::string>());
      }
    } else {
      result.insert(value.as<std::string>());
    }
    return result;
  };

  if (node["override_activities"]) {
    action.override_activities = parseStringSet(node["override_activities"]);
  }

  if (node["override_venue_types"]) {
    action.override_venue_types = parseStringSet(node["override_venue_types"]);
  }

  if (node["exempt_venue_types"]) {
    action.exempt_venue_types = parseStringSet(node["exempt_venue_types"]);
  }

  if (node["replacement"]) {
    action.replacement_activity = node["replacement"].as<std::string>();
  } else {
    action.replacement_activity = "residence";
  }

  if (node["replacement_schedule"]) {
    action.replacement_schedule =
        node["replacement_schedule"].as<std::string>();
  }

  if (node["exempt"]) {
    if (node["exempt"].IsSequence()) {
      for (const auto& exempt_node : node["exempt"]) {
        ActivityExemption exemption;
        exemption.activity_name = exempt_node["activity"].as<std::string>();
        if (exempt_node["selection"].IsDefined()) {
          config_detail::parseSelectionCriteria(exempt_node["selection"],
                                                exemption.criteria);
        }
        action.exemptions.push_back(exemption);
      }
    }
  }

  if (node["compliance_rate"]) {
    action.compliance_rate = node["compliance_rate"].as<double>();
  }

  return action;
}

bool hasExplicitLocationAction(const YAML::Node& node) {
  return node["override_activities"] || node["override_venue_types"] ||
         node["exempt_venue_types"] || node["replacement"] ||
         node["replacement_schedule"] || node["exempt"];
}

ActiveWindow loadActiveWindow(const YAML::Node& node,
                              const std::string& simulation_start_date) {
  ActiveWindow window;
  const std::string policy_name =
      node["name"] ? node["name"].as<std::string>() : std::string("<unnamed>");

  if (node["start_date"] && node["start_time"]) {
    throw std::runtime_error("policy '" + policy_name +
                             "' declares both start_date and start_time; "
                             "give one");
  }
  if (node["end_date"] && node["end_time"]) {
    throw std::runtime_error("policy '" + policy_name +
                             "' declares both end_date and end_time; give one");
  }

  if (node["start_date"]) {
    std::tm sim_start_tm = parseDate(simulation_start_date);
    std::tm policy_start_tm = parseDate(node["start_date"].as<std::string>());
    window.start_time =
        static_cast<double>(daysBetween(sim_start_tm, policy_start_tm));
  } else if (node["start_time"]) {
    window.start_time = node["start_time"].as<double>();
  }

  if (node["end_date"]) {
    std::tm sim_start_tm = parseDate(simulation_start_date);
    std::tm policy_end_tm = parseDate(node["end_date"].as<std::string>());
    window.end_time =
        static_cast<double>(daysBetween(sim_start_tm, policy_end_tm));
  } else if (node["end_time"]) {
    window.end_time = node["end_time"].as<double>();
  }

  const bool start_stated = node["start_date"] || node["start_time"];
  if (start_stated && window.end_time &&
      *window.end_time <= window.start_time) {
    throw std::runtime_error("policy '" + policy_name + "' ends on day " +
                             std::to_string(*window.end_time) +
                             ", at or before its start on day " +
                             std::to_string(window.start_time));
  }

  return window;
}

namespace {

void loadPolicyName(std::string& name, const YAML::Node& node,
                    const char* policy_kind) {
  if (!node["name"]) {
    throw std::runtime_error(std::string(policy_kind) +
                             " must have a 'name' field");
  }
  name = node["name"].as<std::string>();
}

template <typename Policy>
void loadCommonPolicyFields(Policy& policy, const YAML::Node& node,
                            const std::string& simulation_start_date) {
  policy.window = loadActiveWindow(node, simulation_start_date);
  policy.action = loadPolicyAction(node);
  if (node["applies_to"].IsDefined()) {
    config_detail::parseSelectionCriteria(node["applies_to"],
                                          policy.applies_to);
  }
}

}  // namespace

void loadSymptomPolicies(PolicyManager& policy_manager, const YAML::Node& node,
                         const std::string& simulation_start_date,
                         const std::string& policies_filename) {
  if (!node.IsSequence()) {
    throw std::runtime_error("symptom_policies must be a list");
  }

  for (const auto& policy_node : node) {
    SymptomPolicy policy;

    loadPolicyName(policy.name, policy_node, "symptom_policy");

    if (!policy_node["symptoms"]) {
      throw std::runtime_error("symptom_policy '" + policy.name +
                               "' must have 'symptoms' field");
    }
    policy.trigger_symptoms =
        policy_node["symptoms"].as<std::vector<std::string>>();

    loadCommonPolicyFields(policy, policy_node, simulation_start_date);

    if (policy_node["transmission_effects_file"]) {
      if (!hasExplicitLocationAction(policy_node))
        policy.action.has_location_override = false;
      loadTransmissionEffects(
          policy_manager,
          policy_node["transmission_effects_file"].as<std::string>(),
          TransmissionPolicyKind::Symptom,
          static_cast<uint16_t>(policy_manager.getSymptomPolicyCount()),
          policy.action.compliance_rate, policy.name, policies_filename);
    }

    if (policy_node["follow_up_policy"]) {
      policy.follow_up_policy_name =
          policy_node["follow_up_policy"].as<std::string>();
    }
    if (policy_node["inherit_compliance"]) {
      policy.inherit_compliance = policy_node["inherit_compliance"].as<bool>();
    }
    if (policy_node["inherit_refusal"]) {
      policy.inherit_refusal = policy_node["inherit_refusal"].as<bool>();
    }

    policy_manager.addSymptomPolicy(policy);
  }
}

void loadTemporalPolicies(PolicyManager& policy_manager, const YAML::Node& node,
                          const std::string& simulation_start_date,
                          const std::string& policies_filename) {
  if (!node.IsSequence()) {
    throw std::runtime_error("temporal_policies must be a list");
  }

  for (const auto& policy_node : node) {
    TemporalPolicy policy;

    loadPolicyName(policy.name, policy_node, "temporal_policy");
    loadCommonPolicyFields(policy, policy_node, simulation_start_date);

    if (policy_node["transmission_effects_file"]) {
      if (!hasExplicitLocationAction(policy_node))
        policy.action.has_location_override = false;
      loadTransmissionEffects(
          policy_manager,
          policy_node["transmission_effects_file"].as<std::string>(),
          TransmissionPolicyKind::Temporal,
          static_cast<uint16_t>(policy_manager.getTemporalPolicyCount()),
          policy.action.compliance_rate, policy.name, policies_filename);
    }

    policy_manager.addTemporalPolicy(policy);
  }
}

}  // namespace PolicyLoader

}  // namespace june
