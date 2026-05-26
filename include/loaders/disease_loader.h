#pragma once

#include <map>
#include <optional>
#include <string>

#include "../epidemiology/disease.h"

// Forward declaration for YAML
namespace YAML {
class Node;
}

namespace june {

// =============================================================================
// Disease Configuration Loader
// =============================================================================

class DiseaseLoader {
 public:
  // Load disease from YAML config with trajectories and CSV outcome rates.
  // When verbose is true, a [DEBUG] line is printed for each PDF-based curve
  // (gamma, lognormal, beta) showing the old peak value and the
  // max_infectiousness value required to preserve previous infectiousness
  // magnitudes.
  static Disease loadFromYAML(const std::string& yaml_path,
                              bool verbose = false);

 private:
  // Load outcome rates from filter-column CSV
  static OutcomeRates loadOutcomeRatesFromCSV(const std::string& csv_path);

  static OutcomeRates loadOutcomeRatesFromConfig(const YAML::Node& config,
                                                 const std::string& yaml_path);
  static void loadNaturalImmunity(const YAML::Node& config,
                                  TransmissionParams& transmission);
  static void validateOutcomeRowSums(const OutcomeRates& outcome_rates);

  static void parseDepositionStages(
      const YAML::Node& mode_node,
      std::vector<std::shared_ptr<InfectiousnessCurve>>& deposition_by_symptom,
      const std::string& mode_type_prefix, const std::string& mode_name,
      const std::vector<SymptomTag>& symptom_tags, bool verbose);

  // Parse trajectory definitions from YAML
  static std::vector<TrajectoryDefinition> parseTrajectories(
      const YAML::Node& trajectories_node);

  // Parse one trajectory entry; returns nullopt if 'stages' is missing.
  static std::optional<TrajectoryDefinition> parseOneTrajectory(
      const YAML::Node& traj_node);

  static std::vector<SymptomTag> loadSymptomTags(const YAML::Node& config);
  static DiseaseStageSettings loadStageSettings(const YAML::Node& config);
  static void validateTrajectoryStageRefs(
      const std::vector<TrajectoryDefinition>& trajectories,
      const std::vector<SymptomTag>& symptom_tags);

  // Parse distribution parameters from YAML
  static DistributionParams parseDistribution(const YAML::Node& dist_node);

  // Parse an infectiousness curve from YAML.
  // context_label identifies the curve (e.g. "animal_bite / bacteraemia") for
  // diagnostic output when verbose is true.
  static std::shared_ptr<InfectiousnessCurve> parseCurve(
      const YAML::Node& curve_node, const std::string& context_label = "",
      bool verbose = false);
};

}  // namespace june
