#include "loaders/policy_loader.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <iostream>
#include <optional>
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

    if (root["observed_metrics"]) {
      loadObservedMetrics(policy_manager, root["observed_metrics"], filename);
    }

    if (!root["policies"]) {
      if (!root["observed_metrics"]) {
        std::cout << "No policies section found in " << filename << std::endl;
      }
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

void loadObservedMetrics(PolicyManager& policy_manager, const YAML::Node& node,
                         const std::string& policies_filename) {
  if (!node.IsMap()) {
    throw std::runtime_error("observed_metrics in '" + policies_filename +
                             "' must be a mapping of metric name to fields.");
  }
  static const std::unordered_set<std::string> kFields = {"source", "scope",
                                                          "activity", "stage"};
  for (const auto& kv : node) {
    const std::string name = kv.first.as<std::string>();
    const YAML::Node& body = kv.second;
    if (!body.IsMap()) {
      throw std::runtime_error("observed_metrics '" + name + "' in '" +
                               policies_filename + "' must be a mapping.");
    }
    ObservedMetricConfig cfg;
    for (const auto& field : body) {
      const std::string key = field.first.as<std::string>();
      if (!kFields.count(key)) {
        throw std::runtime_error(
            "observed_metrics '" + name + "' in '" + policies_filename +
            "': unknown field '" + key +
            "'. Fields are source, scope, activity, stage.");
      }
      const std::string value = field.second.as<std::string>();
      if (key == "source") cfg.source = value;
      if (key == "scope") cfg.scope = value;
      if (key == "activity") cfg.activity = value;
      if (key == "stage") cfg.stage = value;
    }
    policy_manager.observedMetrics().declare(name, cfg);
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

int64_t readWholeNumber(const YAML::Node& value, const std::string& where,
                        const std::string& key) {
  try {
    return value.as<int64_t>();
  } catch (const YAML::Exception&) {
    throw std::runtime_error(where + "'" + key + "' must be a whole number.");
  }
}

// Parse and check one policy's trigger block. Everything that needs only the
// YAML and the declared metrics is checked here, so a bad trigger fails at
// load with the policy and metric named.
PolicyTrigger loadTrigger(const YAML::Node& node,
                          const ObservedMetrics& metrics,
                          const std::string& label) {
  const std::string where = label + " trigger: ";
  if (!node.IsMap()) throw std::runtime_error(where + "must be a mapping.");

  static const std::unordered_set<std::string> kFields = {
      "metric",    "activate_at",   "deactivate_at", "calm_days",
      "tolerance", "tolerance_pct", "response",      "decision_unit"};
  for (const auto& kv : node) {
    const std::string key = kv.first.as<std::string>();
    if (!kFields.count(key)) {
      throw std::runtime_error(
          where + "unknown field '" + key +
          "'. Fields are metric, activate_at, deactivate_at, calm_days, "
          "tolerance, tolerance_pct.");
    }
  }
  if (node["decision_unit"]) {
    throw std::runtime_error(where +
                             "'decision_unit' is not built yet; it belongs to "
                             "the voluntary response form.");
  }
  if (!node["metric"])
    throw std::runtime_error(where + "'metric' is required.");
  const std::string metric = node["metric"].as<std::string>();
  const auto declared = metrics.declaredNames();
  if (std::find(declared.begin(), declared.end(), metric) == declared.end()) {
    std::string known;
    for (const auto& n : declared) known += (known.empty() ? "" : ", ") + n;
    throw std::runtime_error(
        where + "unknown metric '" + metric +
        "'. Declared metrics: " + (known.empty() ? "none" : known) + ".");
  }
  if (!metrics.isWorldScope(metric)) {
    throw std::runtime_error(where + "metric '" + metric +
                             "' has scope 'venue'; a trigger on a venue scope "
                             "metric is not built yet.");
  }

  const auto number = [&](const char* key) -> std::optional<int64_t> {
    if (!node[key]) return std::nullopt;
    return readWholeNumber(node[key], where, key);
  };
  const auto activate_at = number("activate_at");
  const auto deactivate_at = number("deactivate_at");
  const auto calm_days = number("calm_days");
  const auto tolerance = number("tolerance");
  const auto tolerance_pct = number("tolerance_pct");

  const bool has_threshold =
      activate_at || deactivate_at || calm_days || tolerance || tolerance_pct;
  if (node["response"]) {
    if (has_threshold) {
      throw std::runtime_error(where +
                               "takes either the threshold fields or a "
                               "'response' list, never both.");
    }
    throw std::runtime_error(where +
                             "a 'response' list is not built yet; only the "
                             "threshold form is.");
  }
  if (!has_threshold) {
    throw std::runtime_error(where +
                             "needs the threshold fields ('activate_at' and "
                             "the fields that go with it) or a 'response' "
                             "list.");
  }

  PolicyTrigger trigger;
  trigger.metric = metric;
  trigger.cumulative = metrics.isCumulative(metric);
  if (!activate_at) {
    throw std::runtime_error(where + "'activate_at' is required.");
  }
  if (*activate_at < 1) {
    throw std::runtime_error(where + "'activate_at' must be at least 1, got " +
                             std::to_string(*activate_at) + ".");
  }
  trigger.activate_at = *activate_at;

  if (trigger.cumulative) {
    if (deactivate_at) {
      throw std::runtime_error(where + "'deactivate_at' is not valid for the " +
                               "cumulative metric '" + metric +
                               "'; it reopens after 'calm_days' without " +
                               "growth.");
    }
    if (!calm_days) {
      throw std::runtime_error(where + "'calm_days' is required for the " +
                               "cumulative metric '" + metric + "'.");
    }
    if (*calm_days < 1) {
      throw std::runtime_error(where + "'calm_days' must be at least 1, got " +
                               std::to_string(*calm_days) + ".");
    }
    trigger.calm_days = *calm_days;
    if (tolerance) {
      if (*tolerance < 0) {
        throw std::runtime_error(where + "'tolerance' must not be negative.");
      }
      trigger.tolerance = *tolerance;
    }
    if (tolerance_pct) {
      if (*tolerance_pct < 0 || *tolerance_pct > 100) {
        throw std::runtime_error(
            where + "'tolerance_pct' must be a whole percent from 0 to 100.");
      }
      trigger.tolerance_pct = *tolerance_pct;
    }
  } else {
    if (!deactivate_at) {
      throw std::runtime_error(where + "'deactivate_at' is required for the " +
                               "current metric '" + metric + "'.");
    }
    if (calm_days || tolerance || tolerance_pct) {
      throw std::runtime_error(where +
                               "'calm_days', 'tolerance' and 'tolerance_pct' " +
                               "are only valid for a cumulative metric, and " +
                               "'" + metric + "' is a current one.");
    }
    if (*deactivate_at < 0) {
      throw std::runtime_error(where + "'deactivate_at' must not be negative.");
    }
    if (*deactivate_at >= trigger.activate_at) {
      throw std::runtime_error(where + "'deactivate_at' (" +
                               std::to_string(*deactivate_at) +
                               ") must be below 'activate_at' (" +
                               std::to_string(trigger.activate_at) + ").");
    }
    trigger.deactivate_at = *deactivate_at;
  }
  return trigger;
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
    if (policy_node["trigger"]) {
      policy.trigger =
          loadTrigger(policy_node["trigger"], policy_manager.observedMetrics(),
                      "symptom_policy '" + policy.name + "'");
    }

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
    if (policy_node["trigger"]) {
      policy.trigger =
          loadTrigger(policy_node["trigger"], policy_manager.observedMetrics(),
                      "temporal_policy '" + policy.name + "'");
    }

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
