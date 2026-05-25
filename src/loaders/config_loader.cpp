#include "loaders/config_loader.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#ifdef USE_MPI
#include <mpi.h>
#endif

#include "utils/filtered_csv.h"
#include "utils/filtering.h"

namespace june {

namespace {

bool config_loader_log_rank0() {
#ifdef USE_MPI
  int initialized = 0;
  MPI_Initialized(&initialized);
  if (!initialized) return true;
  int rank = 0;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  return rank == 0;
#else
  return true;
#endif
}

// Map a frequency-group `rate_unit` literal (e.g. "per_month") onto the
// number of days it covers. Used to convert raw CSV rates into a daily
// probability (raw / divisor). Throws if the unit is not recognised.
double rateUnitDivisor(const std::string& rate_unit, const std::string& fg_name) {
  if (rate_unit == "per_day") return 1.0;
  if (rate_unit == "per_week") return 7.0;
  if (rate_unit == "per_month") return 30.0;
  if (rate_unit == "per_year") return 365.0;
  throw std::runtime_error("frequency_group '" + fg_name +
                           "' has unknown rate_unit '" + rate_unit + "'");
}

// Parse a single `frequency_groups:` entry (one named CSV-backed rate source).
// Reads the CSV via csv::loadFilteredCSV, converts the per-person rate to a
// daily probability using the configured rate_unit, and populates the
// FrequencyGroup struct. Logs a one-line summary on rank 0.
FrequencyGroup parseFrequencyGroup(const std::string& name,
                                   const YAML::Node& gnode) {
  FrequencyGroup fg;
  fg.name = name;
  if (!gnode["csv"])
    throw std::runtime_error("frequency_group '" + fg.name +
                             "' missing required field: csv");
  fg.csv_path = gnode["csv"].as<std::string>();
  fg.rate_column = gnode["rate_column"]
                       ? gnode["rate_column"].as<std::string>()
                       : "events_per_month";
  fg.rate_unit = gnode["rate_unit"]
                     ? gnode["rate_unit"].as<std::string>()
                     : "per_month";

  const double divisor = rateUnitDivisor(fg.rate_unit, fg.name);

  csv::FilteredTable table = csv::loadFilteredCSV(fg.csv_path);
  bool has_rate_col = false;
  for (const auto& c : table.value_columns) {
    if (c == fg.rate_column) {
      has_rate_col = true;
      break;
    }
  }
  if (!has_rate_col)
    throw std::runtime_error("frequency_group '" + fg.name + "' CSV '" +
                             fg.csv_path + "' missing rate_column '" +
                             fg.rate_column + "'");

  for (const auto& row : table.rows) {
    FrequencyRow fr;
    fr.criteria = row.criteria;
    auto it = row.values.find(fg.rate_column);
    if (it == row.values.end() || it->second.empty()) continue;
    try {
      double raw = std::stod(it->second);
      fr.daily_probability = raw / divisor;
    } catch (...) {
      continue;
    }
    fg.rows.push_back(std::move(fr));
  }

  if (config_loader_log_rank0()) {
    std::cout << "[CoordEnc] Loaded frequency_group '" << fg.name
              << "' from '" << fg.csv_path << "' (" << fg.rows.size()
              << " rows, rate_column='" << fg.rate_column
              << "', rate_unit='" << fg.rate_unit << "')" << std::endl;
  }

  return fg;
}

// Parse a `{type, mean|p|count}` YAML node into an InviteDistribution.
// Strict variant used by `invite_distribution`: both the `type` key and the
// distribution-specific parameter are required (throws on absence). The
// `enc_name` is woven into error messages to identify the offending encounter.
void parseInviteDistributionStrict(const YAML::Node& dist_node,
                                   InviteDistribution& dist,
                                   const std::string& enc_name) {
  if (!dist_node["type"])
    throw std::runtime_error(
        "Coordinated encounter '" + enc_name +
        "' invite_distribution missing required field: type");
  dist.type = parseDistributionType(dist_node["type"].as<std::string>());

  if (dist.type == DistributionType::POISSON) {
    if (!dist_node["mean"])
      throw std::runtime_error("Poisson invite_distribution for '" + enc_name +
                               "' requires 'mean' parameter.");
    dist.mean = dist_node["mean"].as<double>();
  } else if (dist.type == DistributionType::BINOMIAL) {
    if (!dist_node["p"])
      throw std::runtime_error("Binomial invite_distribution for '" + enc_name +
                               "' requires 'p' parameter.");
    dist.p = dist_node["p"].as<double>();
  } else if (dist.type == DistributionType::FIXED) {
    if (!dist_node["count"])
      throw std::runtime_error("Fixed invite_distribution for '" + enc_name +
                               "' requires 'count' parameter.");
    dist.count = dist_node["count"].as<int>();
  }
}

// Parse the required scalar/list fields of a coordinated-encounter entry
// (name, network, trigger_slots, allowed_venues) plus the optional `enabled`
// flag. Throws if any required field is missing.
void parseEncounterRequiredFields(const YAML::Node& enc_node,
                                  CoordinatedEncounterDef& enc_def) {
  if (!enc_node["name"])
    throw std::runtime_error(
        "Coordinated encounter missing required field: name");
  enc_def.name = enc_node["name"].as<std::string>();

  if (enc_node["enabled"]) {
    enc_def.enabled = enc_node["enabled"].as<bool>();
  }

  if (!enc_node["network"])
    throw std::runtime_error("Coordinated encounter '" + enc_def.name +
                             "' missing required field: network");
  enc_def.network = enc_node["network"].as<std::string>();

  if (!enc_node["trigger_slots"])
    throw std::runtime_error("Coordinated encounter '" + enc_def.name +
                             "' missing required field: trigger_slots");
  if (enc_node["trigger_slots"].IsSequence()) {
    for (const auto& slot : enc_node["trigger_slots"]) {
      enc_def.trigger_slots.push_back(slot.as<std::string>());
    }
  }

  if (!enc_node["allowed_venues"])
    throw std::runtime_error("Coordinated encounter '" + enc_def.name +
                             "' missing required field: allowed_venues");
  if (enc_node["allowed_venues"].IsSequence()) {
    for (const auto& venue : enc_node["allowed_venues"]) {
      enc_def.allowed_venues.push_back(venue.as<std::string>());
    }
  }
}

// Resolve the per-encounter rate source: either an inline scalar
// `proposal_probability` (signalled by has_proposal_prob) or a named
// `frequency_group` referencing the surrounding config's frequency_groups
// map. Exactly one must be declared. Throws on unknown group, on both
// declared, or on neither.
void parseEncounterRateSource(
    const YAML::Node& enc_node, CoordinatedEncounterDef& enc_def,
    const std::unordered_map<std::string, FrequencyGroup>& frequency_groups,
    bool has_proposal_prob) {
  if (enc_node["frequency_group"]) {
    std::string fg_name = enc_node["frequency_group"].as<std::string>();
    if (!frequency_groups.count(fg_name)) {
      throw std::runtime_error(
          "Coordinated encounter '" + enc_def.name +
          "' references unknown frequency_group '" + fg_name +
          "' (not declared in frequency_groups block)");
    }
    enc_def.frequency_group = fg_name;
  }

  if (enc_def.frequency_group.has_value() && has_proposal_prob) {
    throw std::runtime_error(
        "Coordinated encounter '" + enc_def.name +
        "' declares both 'proposal_probability' and "
        "'frequency_group'; these are mutually exclusive. Remove "
        "proposal_probability when using a frequency_group.");
  }
  if (!enc_def.frequency_group.has_value() && !has_proposal_prob) {
    throw std::runtime_error(
        "Coordinated encounter '" + enc_def.name +
        "' missing rate source: specify either 'proposal_probability' "
        "or 'frequency_group'.");
  }
}

// Forward decl: defined further down (lenient companion to
// parseInviteDistributionStrict).
void parseDailyMaxDistribution(const YAML::Node& dmd_node,
                               InviteDistribution& dist);

// Parse one entry from `coordinated_encounters.encounters[]`. Dispatches
// through the smaller per-section helpers (required fields, invite
// distribution, rate source, daily-max distribution) and reads the optional
// scalar/list fields (acceptance_probability, is_virtual /
// virtual_contact_matrix, min_attendees, priority, network_partner_filter).
CoordinatedEncounterDef parseEncounter(
    const YAML::Node& enc_node,
    const std::unordered_map<std::string, FrequencyGroup>& frequency_groups) {
  CoordinatedEncounterDef enc_def;
  parseEncounterRequiredFields(enc_node, enc_def);

  // proposal_probability is optional when frequency_group is set.
  // Validated by parseEncounterRateSource below.
  bool has_proposal_prob =
      static_cast<bool>(enc_node["proposal_probability"]);
  if (has_proposal_prob) {
    enc_def.proposal_probability =
        enc_node["proposal_probability"].as<double>();
  }

  if (!enc_node["invite_distribution"])
    throw std::runtime_error(
        "Coordinated encounter '" + enc_def.name +
        "' missing required field: invite_distribution");
  parseInviteDistributionStrict(enc_node["invite_distribution"],
                                enc_def.invite_distribution, enc_def.name);

  // acceptance_probability is optional. When absent, defaults to 1.0
  // (no refusal), which is appropriate whenever a frequency CSV is the
  // realized-rate source and any refusal dynamics are already baked in.
  if (enc_node["acceptance_probability"]) {
    enc_def.acceptance_probability =
        enc_node["acceptance_probability"].as<double>();
  }

  if (enc_node["is_virtual"]) {
    enc_def.is_virtual = enc_node["is_virtual"].as<bool>();
  }

  if (enc_def.is_virtual) {
    if (!enc_node["virtual_contact_matrix"])
      throw std::runtime_error("Virtual coordinated encounter '" +
                               enc_def.name +
                               "' missing required field: virtual_contact_matrix");
    enc_def.virtual_contact_matrix =
        enc_node["virtual_contact_matrix"].as<std::string>();
  }

  if (enc_node["min_attendees"]) {
    enc_def.min_attendees = enc_node["min_attendees"].as<int>();
  }
  if (enc_node["priority"]) {
    enc_def.priority = enc_node["priority"].as<int>();
  }
  if (enc_node["network_partner_filter"]) {
    enc_def.network_partner_filter =
        enc_node["network_partner_filter"].as<std::string>();
  }

  parseEncounterRateSource(enc_node, enc_def, frequency_groups,
                           has_proposal_prob);

  if (enc_node["daily_max_distribution"]) {
    parseDailyMaxDistribution(enc_node["daily_max_distribution"],
                              enc_def.daily_max_distribution);
  }

  return enc_def;
}

// Lenient counterpart to parseInviteDistributionStrict, used for
// `daily_max_distribution` blocks. Every key is optional: missing `type`
// leaves the existing default in place, and a missing distribution-specific
// parameter is silently skipped. No throws.
void parseDailyMaxDistribution(const YAML::Node& dmd_node,
                               InviteDistribution& dist) {
  if (dmd_node["type"]) {
    dist.type = parseDistributionType(dmd_node["type"].as<std::string>());
  }
  if (dist.type == DistributionType::POISSON && dmd_node["mean"]) {
    dist.mean = dmd_node["mean"].as<double>();
  } else if (dist.type == DistributionType::BINOMIAL && dmd_node["p"]) {
    dist.p = dmd_node["p"].as<double>();
  } else if (dist.type == DistributionType::FIXED && dmd_node["count"]) {
    dist.count = dmd_node["count"].as<int>();
  }
}

// Parse a YAML sequence of `{property, operator, value}` entries into a vector
// of SelectionCriterion. The scalar `value` is dispatched int -> double ->
// string; sequence `value` becomes vector<int32_t>. Shared by loadSchedule,
// loadActivityPreferences, and loadVaccination (campaign selection blocks).
void parseSelectionCriteria(const YAML::Node& selection_node,
                            std::vector<SelectionCriterion>& out) {
  for (const auto& criterion_node : selection_node) {
    SelectionCriterion criterion;
    criterion.property_path =
        criterion_node["property"].as<std::string>();
    criterion.operator_type =
        criterion_node["operator"].as<std::string>();

    const auto& value_node = criterion_node["value"];
    if (value_node.IsSequence()) {
      criterion.value = value_node.as<std::vector<int32_t>>();
    } else if (value_node.IsScalar()) {
      try {
        criterion.value = value_node.as<int>();
      } catch (...) {
        try {
          criterion.value = value_node.as<double>();
        } catch (...) {
          criterion.value = value_node.as<std::string>();
        }
      }
    }
    out.push_back(std::move(criterion));
  }
}

}  // namespace

Config ConfigLoader::loadAll(const std::string& simulation_file) {
  Config config;

  // 1. Load the master simulation config first
  config.simulation = loadSimulation(simulation_file);

  // 2. Load all other configs using paths specified in the simulation config
  config.schedule = loadSchedule(config.simulation.schedules_file);
  config.contact_matrices =
      loadContactMatrices(config.simulation.contact_matrices_file);
  config.performance = loadPerformance(config.simulation.performance_file);
  config.parallel = loadParallel(config.simulation.parallel_file);
  config.activity_preferences =
      loadActivityPreferences(config.simulation.activity_preferences_file);
  config.vaccination = loadVaccination(config.simulation.vaccines_file);
  config.coordinated_encounters =
      loadCoordinatedEncounters(config.simulation.coordinated_encounters_file);

  return config;
}

SimulationConfig ConfigLoader::loadSimulation(const std::string& filename) {
  YAML::Node root = YAML::LoadFile(filename);
  SimulationConfig config;

  // Time settings
  if (root["time"]) {
    auto time = root["time"];
    config.start_date = time["start_date"].as<std::string>();
    config.end_date = time["end_date"].as<std::string>();
  }

  // File links - support both nested config_paths: section and legacy top-level
  // keys
  auto paths = root["config_paths"] ? root["config_paths"] : root;

  if (paths["disease_file"])
    config.disease_file = paths["disease_file"].as<std::string>();
  if (paths["contact_matrices_file"])
    config.contact_matrices_file =
        paths["contact_matrices_file"].as<std::string>();
  if (paths["schedules_file"])
    config.schedules_file = paths["schedules_file"].as<std::string>();
  if (paths["vaccines_file"])
    config.vaccines_file = paths["vaccines_file"].as<std::string>();
  if (paths["activity_preferences_file"])
    config.activity_preferences_file =
        paths["activity_preferences_file"].as<std::string>();
  if (paths["coordinated_encounters_file"])
    config.coordinated_encounters_file =
        paths["coordinated_encounters_file"].as<std::string>();
  if (paths["group_encounters_file"]) {
    throw std::runtime_error(
        "simulation.yaml: 'group_encounters_file' is no longer a top-level "
        "config_paths entry. Move it into coordinated_encounters.yaml as a "
        "`parameters_file` field on the encounter entry whose network is "
        "\"group_sex_roster\".");
  }
  if (paths["performance_file"])
    config.performance_file = paths["performance_file"].as<std::string>();
  if (paths["parallel_file"])
    config.parallel_file = paths["parallel_file"].as<std::string>();
  if (paths["policies_file"])
    config.policies_file = paths["policies_file"].as<std::string>();
  if (paths["infection_seeds_file"])
    config.infection_seeds_file =
        paths["infection_seeds_file"].as<std::string>();
  if (paths["compartmental_model_sidecar"])
    config.compartmental_model_sidecar =
        paths["compartmental_model_sidecar"].as<std::string>();

  // Seeds for reproducibility (optional)
  if (root["random_seed"]) {
    config.random_seed = root["random_seed"].as<unsigned int>();
  }

  // Verbose / debug-level output at load time
  if (root["verbose"]) {
    config.verbose = root["verbose"].as<bool>();
  }

  // Output settings
  if (root["output"]) {
    auto output = root["output"];
    config.stats_interval_days = output["stats_interval_days"].as<int>();

    if (output["save_full_person_details"]) {
      config.save_full_person_details =
          output["save_full_person_details"].as<std::string>();
    }
    if (output["save_person_activities"]) {
      config.save_person_activities =
          output["save_person_activities"].as<std::string>();
    }
    if (output["compression_level"]) {
      config.compression_level = output["compression_level"].as<int>();
    }
    if (output["flush_interval_days"]) {
      config.flush_interval_days = output["flush_interval_days"].as<int>();
    }
    if (output["max_event_buffer_size"]) {
      config.max_event_buffer_size = output["max_event_buffer_size"].as<int>();
    }
    if (output["save_population_summary"]) {
      config.save_population_summary =
          output["save_population_summary"].as<bool>();
    }
    if (output["summary_properties"]) {
      config.summary_properties =
          output["summary_properties"].as<std::vector<std::string>>();
    }
  }

  // Regional Risk Factors settings
  if (root["regional_risk"]) {
    auto regional_risk = root["regional_risk"];
    if (regional_risk["enabled"]) {
      config.regional_risk.enabled = regional_risk["enabled"].as<bool>();
    }
    if (regional_risk["regional_risk_file"]) {
      config.regional_risk.regional_risk_file =
          regional_risk["regional_risk_file"].as<std::string>();
    }
  }

  // Partial-presence venue types (optional). Map of venue type name to
  // { target_group_size: N } — the target rider count per ephemeral runtime
  // bin within a venue of that type (e.g. train_line: 100 ≈ one carriage).
  // num_bins at slot time = max(1, ceil(N_global_riders / target_group_size)),
  // so bins are bounded by target above and equal-sized below it. Omitting
  // the block (or an empty map) leaves the feature off.
  if (root["partial_presence"]) {
    auto pp = root["partial_presence"];
    if (pp["enabled_venue_types"] && pp["enabled_venue_types"].IsMap()) {
      for (auto it = pp["enabled_venue_types"].begin();
           it != pp["enabled_venue_types"].end(); ++it) {
        std::string name = it->first.as<std::string>();
        int tgs = it->second["target_group_size"]
                      ? it->second["target_group_size"].as<int>()
                      : 0;
        config.partial_presence.target_group_size_by_name[name] = tgs;
      }
    }
  }

  // Checkpoint / restart settings (optional). Cadence is mutually exclusive:
  // on_dates (non-null, non-empty) takes precedence over every_n_days. A
  // null YAML value leaves the corresponding optional empty.
  if (root["checkpoint"]) {
    auto cp = root["checkpoint"];
    if (cp["enabled"]) {
      config.checkpoint.enabled = cp["enabled"].as<bool>();
    }
    if (cp["output_dir"]) {
      config.checkpoint.output_dir = cp["output_dir"].as<std::string>();
    }
    if (cp["every_n_days"] && !cp["every_n_days"].IsNull()) {
      config.checkpoint.every_n_days = cp["every_n_days"].as<int>();
    }
    if (cp["on_dates"] && !cp["on_dates"].IsNull()) {
      config.checkpoint.on_dates =
          cp["on_dates"].as<std::vector<std::string>>();
    }
    if (cp["keep_last"]) {
      config.checkpoint.keep_last = cp["keep_last"].as<int>();
    }
  }

  return config;
}

ScheduleConfig ConfigLoader::loadSchedule(const std::string& filename) {
  YAML::Node root = YAML::LoadFile(filename);
  ScheduleConfig config;

  // Day type cycle
  if (root["day_type_cycle"]) {
    config.day_type_cycle =
        root["day_type_cycle"].as<std::vector<std::string>>();
  }

  // Derive ordered unique day type names from cycle
  for (const auto& dt : config.day_type_cycle) {
    if (std::find(config.day_type_names.begin(), config.day_type_names.end(),
                  dt) == config.day_type_names.end()) {
      config.day_type_names.push_back(dt);
    }
  }

  // Schedule types
  if (root["schedule_types"]) {
    // Optional CSV for probabilistic schedule assignment
    if (root["schedule_csv"]) {
      config.csv_path = root["schedule_csv"].as<std::string>();
    }

    // Load default schedule type
    if (root["default_schedule_type"]) {
      config.default_schedule_type =
          root["default_schedule_type"].as<std::string>();
    }

    // Helper: parse a slot list node into a vector of TimeSlot
    auto parseSlotList =
        [](const YAML::Node& slot_list_node) -> std::vector<TimeSlot> {
      std::vector<TimeSlot> slots;
      for (const auto& slot : slot_list_node) {
        TimeSlot ts;
        ts.name = slot["name"].as<std::string>();
        ts.start = slot["start"].as<std::string>();
        ts.end = slot["end"].as<std::string>();
        ts.allowed_activities =
            slot["allowed_activities"].as<std::vector<std::string>>();

        if (slot["coordinated_only_activities"])
          ts.coordinated_only_activities = slot["coordinated_only_activities"]
                                               .as<std::vector<std::string>>();

        if (slot["specified_activity"]) {
          SpecifiedActivity spec_act;
          spec_act.type = slot["specified_activity"]["type"].as<std::string>();
          spec_act.index = slot["specified_activity"]["index"].as<int>();
          if (slot["specified_activity"]["venue_type"]) {
            spec_act.venue_type =
                slot["specified_activity"]["venue_type"].as<std::string>();
          }
          ts.specified_activity = spec_act;
        }

        if (slot["hop_on_activity"]) {
          for (const auto& hop_kv : slot["hop_on_activity"]) {
            std::string act_name = hop_kv.first.as<std::string>();
            if (hop_kv.second.IsScalar()) {
              // Direct hop: activity_name → schedule_name
              ts.hop_on_activity[act_name] = hop_kv.second.as<std::string>();
            } else if (hop_kv.second.IsMap()) {
              // Property-dispatched hop: schedule name resolved at runtime
              TimeSlot::PropertyDispatchHop dispatch;
              dispatch.property_name =
                  hop_kv.second["property"].as<std::string>();
              dispatch.schedule_name_template =
                  hop_kv.second["schedule_template"].as<std::string>();
              ts.property_hop_dispatch[act_name] = std::move(dispatch);
            }
          }
        }

        slots.push_back(ts);
      }
      return slots;
    };

    // Parse each schedule type
    for (const auto& type_kv : root["schedule_types"]) {
      ScheduleType sched_type;
      sched_type.name = type_kv.first.as<std::string>();
      const auto& type_node = type_kv.second;

      // Priority
      if (type_node["priority"]) {
        sched_type.priority = type_node["priority"].as<int>();
      }

      // Temporary schedule flag and flat_slots
      if (type_node["is_temporary"]) {
        sched_type.is_temporary = type_node["is_temporary"].as<bool>();
      }
      if (type_node["return_schedule"]) {
        sched_type.return_schedule =
            type_node["return_schedule"].as<std::string>();
      }
      if (type_node["flat_slots"]) {
        sched_type.flat_slots = parseSlotList(type_node["flat_slots"]);
      }

      // Selection criteria
      if (type_node["selection"]) {
        parseSelectionCriteria(type_node["selection"],
                               sched_type.selection_criteria);
      }

      // Slot lists — one per day type name
      for (const auto& dt_name : config.day_type_names) {
        if (type_node[dt_name]) {
          sched_type.slots_by_day_type[dt_name] =
              parseSlotList(type_node[dt_name]);
        }
      }

      // Per-schedule override: activities to treat as HYBRID (per-tick re-
      // roll, cached venue) even when listed globally as deterministic in
      // performance.yaml.
      if (type_node["force_hybrid_activities"]) {
        sched_type.force_hybrid_activities =
            type_node["force_hybrid_activities"].as<std::vector<std::string>>();
      }

      // Linked activities: share one dice roll per (person, sim_day).
      // See ScheduleType::linked_activities for semantics.
      if (type_node["linked_activities"]) {
        sched_type.linked_activities =
            type_node["linked_activities"].as<std::vector<std::string>>();
      }

      // Participation rates: activity -> { day_type_name: rate, ... }
      if (type_node["participation"]) {
        for (const auto& kv : type_node["participation"]) {
          std::string activity = kv.first.as<std::string>();
          for (const auto& dt_name : config.day_type_names) {
            if (kv.second[dt_name]) {
              sched_type.participation_by_day_type[dt_name][activity] =
                  kv.second[dt_name].as<double>();
            }
          }
        }
      }

      config.schedule_types.push_back(sched_type);
    }

    // Sort schedule types by priority (highest first)
    std::sort(config.schedule_types.begin(), config.schedule_types.end(),
              [](const ScheduleType& a, const ScheduleType& b) {
                return a.priority > b.priority;
              });
  }

  return config;
}

ContactMatrixConfig ConfigLoader::loadContactMatrices(
    const std::string& filename) {
  YAML::Node root = YAML::LoadFile(filename);
  ContactMatrixConfig config;

  // Load beta values
  if (root["betas"]) {
    for (const auto& beta_kv : root["betas"]) {
      std::string venue_type = beta_kv.first.as<std::string>();
      double beta = beta_kv.second.as<double>();
      config.betas[venue_type] = beta;
    }
  }

  // Load global beta scaling
  if (root["global_beta"]) {
    if (root["global_beta"]["value"]) {
      config.global_beta.value = root["global_beta"]["value"].as<double>();
    }
    if (root["global_beta"]["enabled"]) {
      config.global_beta.enabled = root["global_beta"]["enabled"].as<bool>();
    }
  }

  // Load default values
  if (root["default_beta"]) {
    config.default_beta = root["default_beta"].as<double>();
  }
  if (root["default_contacts"]) {
    config.default_contacts = root["default_contacts"].as<double>();
  }
  if (root["default_proportion_physical"]) {
    config.default_proportion_physical =
        root["default_proportion_physical"].as<double>();
  }
  if (root["alpha_physical"]) {
    config.alpha_physical = root["alpha_physical"].as<double>();
  }
  if (root["default_characteristic_time"]) {
    config.default_characteristic_time =
        root["default_characteristic_time"].as<double>();
  }

  // Helper lambda: parse a full ContactMatrix from a YAML node
  auto parseContactMatrix = [](const YAML::Node& matrix_node) -> ContactMatrix {
    ContactMatrix cm;
    if (matrix_node["bins"]) {
      cm.bins = matrix_node["bins"].as<std::vector<std::string>>();
    }
    if (matrix_node["characteristic_time"]) {
      cm.characteristic_time = matrix_node["characteristic_time"].as<double>();
    }
    double beta = 1.0;
    if (matrix_node["beta"]) {
      beta = matrix_node["beta"].as<double>();
    }
    if (matrix_node["contacts"]) {
      for (const auto& row : matrix_node["contacts"]) {
        std::vector<double> row_vec;
        for (const auto& val : row) row_vec.push_back(val.as<double>() * beta);
        cm.contacts.push_back(row_vec);
      }
    }
    if (matrix_node["proportion_physical"]) {
      for (const auto& row : matrix_node["proportion_physical"]) {
        std::vector<double> row_vec;
        for (const auto& val : row) row_vec.push_back(val.as<double>());
        cm.proportion_physical.push_back(row_vec);
      }
    }
    return cm;
  };

  // Load optional default fallback matrix
  if (root["default_contacts_matrix"]) {
    config.default_matrix = parseContactMatrix(root["default_contacts_matrix"]);
  }

  // Track mode names in insertion order
  std::vector<std::string> seen_modes_ordered;
  auto addMode = [&seen_modes_ordered](const std::string& m) {
    for (const auto& s : seen_modes_ordered)
      if (s == m) return;
    seen_modes_ordered.push_back(m);
  };

  // Load contact matrices
  if (root["contact_matrices"]) {
    for (const auto& matrix_kv : root["contact_matrices"]) {
      std::string venue_type = matrix_kv.first.as<std::string>();
      const auto& matrix_node = matrix_kv.second;

      if (matrix_node["modes"]) {
        // Multi-mode: parse each mode entry
        bool first_mode = true;
        for (const auto& mode_kv : matrix_node["modes"]) {
          std::string mode_name = mode_kv.first.as<std::string>();
          addMode(mode_name);
          ContactMatrix cm = parseContactMatrix(mode_kv.second);
          config.mode_matrices[venue_type][mode_name] = cm;
          // Backward compat: store first mode matrix in the flat matrices map
          if (first_mode) {
            config.matrices[venue_type] = cm;
            first_mode = false;
          }
        }
      } else {
        // Single-mode fallback: store as "default" mode and in flat map
        addMode("default");
        ContactMatrix cm = parseContactMatrix(matrix_node);
        config.mode_matrices[venue_type]["default"] = cm;
        config.matrices[venue_type] = cm;
      }
    }
  }

  // Set mode_names from ordered union of seen modes (or single "default")
  if (!seen_modes_ordered.empty()) {
    config.mode_names = seen_modes_ordered;
  }

  return config;
}

PerformanceConfig ConfigLoader::loadPerformance(const std::string& filename) {
  PerformanceConfig config;

  // Try to load file, use defaults if it doesn't exist
  try {
    YAML::Node root = YAML::LoadFile(filename);

    if (root["performance"]) {
      const auto& perf = root["performance"];

      if (perf["precompute_schedules"]) {
        config.precompute_schedules = perf["precompute_schedules"].as<bool>();
      }

      if (perf["deterministic_activities"]) {
        config.deterministic_activities =
            perf["deterministic_activities"].as<std::vector<std::string>>();
      }

      if (perf["hybrid_activities"]) {
        config.hybrid_activities =
            perf["hybrid_activities"].as<std::vector<std::string>>();
      }

      if (perf["stochastic_activities"]) {
        config.stochastic_activities =
            perf["stochastic_activities"].as<std::vector<std::string>>();
      }

      if (perf["track_active_infections_only"]) {
        config.track_active_infections_only =
            perf["track_active_infections_only"].as<bool>();
      }
    }
  } catch (const std::exception& e) {
    // File doesn't exist or parse error - use defaults
    std::cerr << "Warning: Could not load " << filename << ": " << e.what()
              << std::endl;
    std::cerr << "Using default performance settings (maximum performance mode)"
              << std::endl;

    // Defaults: maximum performance (all activities deterministic)
    config.precompute_schedules = true;
    config.deterministic_activities.clear();  // Empty = all deterministic
    config.stochastic_activities.clear();
    config.track_active_infections_only = true;
  }

  return config;
}

ParallelConfig ConfigLoader::loadParallel(const std::string& filename) {
  ParallelConfig config;

  YAML::Node root = YAML::LoadFile(filename);

  if (!root["parallel"]) {
    throw std::runtime_error("Parallel config file '" + filename +
                             "' is missing the top-level 'parallel:' block.");
  }

  YAML::Node parallel = root["parallel"];

  if (parallel["enabled"]) {
    config.enabled = parallel["enabled"].as<bool>();
  }

  if (parallel["verbose_mpi"]) {
    config.verbose_mpi = parallel["verbose_mpi"].as<bool>();
  }

  if (!parallel["partitioning"]) {
    throw std::runtime_error(
        "Parallel config '" + filename +
        "' is missing the required 'parallel.partitioning' block "
        "(must specify level, centroids_file, adjacency_file).");
  }

  YAML::Node part = parallel["partitioning"];

  if (!part["level"]) {
    throw std::runtime_error(
        "Parallel config '" + filename +
        "' is missing required key 'parallel.partitioning.level' "
        "(e.g. 'SGU', 'MGU', 'LGU').");
  }
  config.partition_level = part["level"].as<std::string>();

  if (!part["centroids_file"]) {
    throw std::runtime_error(
        "Parallel config '" + filename +
        "' is missing required key 'parallel.partitioning.centroids_file'.");
  }
  config.centroids_file = part["centroids_file"].as<std::string>();

  if (!part["adjacency_file"]) {
    throw std::runtime_error(
        "Parallel config '" + filename +
        "' is missing required key 'parallel.partitioning.adjacency_file'.");
  }
  config.adjacency_file = part["adjacency_file"].as<std::string>();

  if (part["metis"] && part["metis"]["imbalance_tolerance"]) {
    config.metis_imbalance_tolerance =
        part["metis"]["imbalance_tolerance"].as<double>();
  }

  if (parallel["chunked_loading"]) {
    YAML::Node chunked = parallel["chunked_loading"];

    if (chunked["person_metadata_chunk_size"]) {
      config.person_metadata_chunk_size =
          chunked["person_metadata_chunk_size"].as<size_t>();
    }

    if (chunked["geo_unit_chunk_size"]) {
      config.geo_unit_chunk_size = chunked["geo_unit_chunk_size"].as<size_t>();
    }
  }

  if (parallel["communication"]) {
    YAML::Node comm = parallel["communication"];
    if (comm["buffer_size_mb"]) {
      config.buffer_size_mb = comm["buffer_size_mb"].as<int>();
    }
  }

  if (parallel["output"]) {
    YAML::Node output = parallel["output"];

    if (output["save_partition"]) {
      config.save_partition = output["save_partition"].as<bool>();
    }

    if (output["partition_file"]) {
      config.partition_file = output["partition_file"].as<std::string>();
    }

    if (output["report_load_balance"]) {
      config.report_load_balance = output["report_load_balance"].as<bool>();
    }

    if (output["report_communication"]) {
      config.report_communication = output["report_communication"].as<bool>();
    }

    if (output["report_interval_days"]) {
      config.report_interval_days = output["report_interval_days"].as<int>();
    }
  }

  return config;
}

ActivityPreferenceConfig ConfigLoader::loadActivityPreferences(
    const std::string& filename) {
  ActivityPreferenceConfig config;

  try {
    YAML::Node root = YAML::LoadFile(filename);
    if (root["profiles"]) {
      for (const auto& profile_node : root["profiles"]) {
        PreferenceProfile profile;
        profile.name = profile_node["name"]
                           ? profile_node["name"].as<std::string>()
                           : "unnamed";
        profile.activity = profile_node["activity"]
                               ? profile_node["activity"].as<std::string>()
                               : "";
        profile.priority =
            profile_node["priority"] ? profile_node["priority"].as<int>() : 0;

        // Load selection criteria
        if (profile_node["selection"]) {
          parseSelectionCriteria(profile_node["selection"],
                                 profile.selection_criteria);
        }

        // Load preference weights
        if (profile_node["weights"]) {
          for (const auto& weight_kv : profile_node["weights"]) {
            profile.preference_weights[weight_kv.first.as<std::string>()] =
                weight_kv.second.as<double>();
          }
        }
        config.profiles.push_back(profile);
      }
    }

    // Sort by priority
    std::sort(config.profiles.begin(), config.profiles.end(),
              [](const PreferenceProfile& a, const PreferenceProfile& b) {
                return a.priority > b.priority;
              });

  } catch (const std::exception& e) {
    std::cerr << "Warning: Could not load " << filename << ": " << e.what()
              << std::endl;
    std::cerr << "Using default activity preferences (all weights = 1.0)"
              << std::endl;
  }

  return config;
}

VaccinationConfig ConfigLoader::loadVaccination(const std::string& filename) {
  VaccinationConfig config;
  if (!std::filesystem::exists(filename)) {
    if (config_loader_log_rank0()) {
      std::cout << "Warning: " << filename
                << " not found. Vaccination will be disabled." << std::endl;
    }
    config.enabled = false;
    return config;
  }
  try {
    YAML::Node root = YAML::LoadFile(filename);
    if (!root) return config;

    if (root["enabled"]) {
      config.enabled = root["enabled"].as<bool>();
    } else {
      config.enabled = true;  // Fallback to true if file exists but no flag
    }

    // Load vaccines
    if (root["vaccines"]) {
      for (const auto& vacc_kv : root["vaccines"]) {
        VaccineConfig vc;
        vc.name = vacc_kv.first.as<std::string>();
        const auto& vacc_node = vacc_kv.second;

        if (vacc_node["doses"]) {
          const auto& doses_node = vacc_node["doses"];
          for (size_t i = 0; i < doses_node.size(); ++i) {
            const auto& d_node = doses_node[i];
            DoseConfig dc;
            dc.number = static_cast<int>(i);
            if (d_node["days_to_effective"])
              dc.days_to_effective = d_node["days_to_effective"].as<double>();
            if (d_node["days_to_waning"])
              dc.days_to_waning = d_node["days_to_waning"].as<double>();
            if (d_node["days_to_finished"])
              dc.days_to_finished = d_node["days_to_finished"].as<double>();
            if (d_node["waning_factor"])
              dc.waning_factor = d_node["waning_factor"].as<double>();

            // Load efficacies
            auto parse_eff = [](const YAML::Node& node) {
              std::unordered_map<std::string, std::vector<AgeEfficacy>> res;
              if (node) {
                for (const auto& eff_kv : node) {
                  std::string disease = eff_kv.first.as<std::string>();
                  if (eff_kv.second.IsMap()) {
                    for (const auto& age_kv : eff_kv.second) {
                      std::string age_range = age_kv.first.as<std::string>();
                      size_t dash = age_range.find('-');
                      if (dash != std::string::npos) {
                        AgeEfficacy ae;
                        ae.min_age = std::stoi(age_range.substr(0, dash));
                        ae.max_age = std::stoi(age_range.substr(dash + 1));
                        ae.efficacy = age_kv.second.as<double>();
                        res[disease].push_back(ae);
                      }
                    }
                  } else {
                    res[disease].push_back(
                        {0, 100, eff_kv.second.as<double>()});
                  }
                }
              }
              return res;
            };

            dc.infection_efficacy = parse_eff(d_node["infection_efficacy"]);
            dc.symptom_efficacy = parse_eff(d_node["symptom_efficacy"]);
            vc.doses.push_back(dc);
          }
        }
        config.vaccines[vc.name] = vc;
      }
    }

    // Load campaigns
    if (root["campaigns"]) {
      for (const auto& camp_kv : root["campaigns"]) {
        VaccinationCampaignConfig camp;
        camp.name = camp_kv.first.as<std::string>();
        const auto& camp_node = camp_kv.second;

        if (camp_node["start_date"])
          camp.start_date = camp_node["start_date"].as<std::string>();
        if (camp_node["end_date"])
          camp.end_date = camp_node["end_date"].as<std::string>();
        if (camp_node["vaccine_type"])
          camp.vaccine_type = camp_node["vaccine_type"].as<std::string>();
        if (camp_node["dose_sequence"])
          camp.dose_sequence =
              camp_node["dose_sequence"].as<std::vector<int>>();
        if (camp_node["days_to_next_dose"])
          camp.days_to_next_dose =
              camp_node["days_to_next_dose"].as<std::vector<double>>();
        if (camp_node["daily_coverage"])
          camp.daily_coverage = camp_node["daily_coverage"].as<double>();

        // Selection criteria
        if (camp_node["selection"]) {
          parseSelectionCriteria(camp_node["selection"],
                                 camp.selection_criteria);
        }

        if (camp_node["last_dose_type_filter"]) {
          camp.last_dose_type_filter =
              camp_node["last_dose_type_filter"].as<std::vector<std::string>>();
        }

        config.campaigns.push_back(camp);
      }
    }

  } catch (const std::exception& e) {
    std::cerr << "Warning: Could not load " << filename << ": " << e.what()
              << std::endl;
    config.enabled = false;
  }
  return config;
}

CoordinatedEncounterConfig ConfigLoader::loadCoordinatedEncounters(
    const std::string& filename) {
  CoordinatedEncounterConfig config;
  if (!std::filesystem::exists(filename)) {
    throw std::runtime_error(
        "coordinated_encounters file not found: " + filename +
        ". simulation.yaml's `coordinated_encounters_file` must point at "
        "an existing YAML — disabling the feature silently is not safe.");
  }
  YAML::Node root = YAML::LoadFile(filename);
  if (!root) {
    throw std::runtime_error(
        "coordinated_encounters file " + filename +
        " parsed to a null root node (file empty or malformed).");
  }
  {
    if (root["coordinated_encounters"]) {
      const auto& ce_node = root["coordinated_encounters"];

      if (ce_node["enabled"]) {
        config.enabled = ce_node["enabled"].as<bool>();
      }
      if (ce_node["log_commitments"]) {
        config.log_commitments = ce_node["log_commitments"].as<bool>();
      }

      // Optional: frequency_groups block. Each group specifies a CSV whose
      // per-person rate overrides the scalar proposal_probability for any
      // encounter that declares `frequency_group: "<name>"`.
      if (ce_node["frequency_groups"]) {
        for (const auto& kv : ce_node["frequency_groups"]) {
          std::string name = kv.first.as<std::string>();
          config.frequency_groups[name] =
              parseFrequencyGroup(name, kv.second);
        }
      }

      if (!ce_node["encounters"]) {
        throw std::runtime_error(
            "coordinated_encounters block must define 'encounters' list.");
      }
      for (const auto& enc_node : ce_node["encounters"]) {
        config.encounters.push_back(
            parseEncounter(enc_node, config.frequency_groups));
      }

      // Sort encounters by priority (lower number = higher precedence =
      // processed first)
      std::sort(config.encounters.begin(), config.encounters.end(),
                [](const CoordinatedEncounterDef& a,
                   const CoordinatedEncounterDef& b) {
                  return a.priority < b.priority;
                });
    }
  }
  return config;
}

}  // namespace june
