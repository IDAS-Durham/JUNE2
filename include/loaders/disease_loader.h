#pragma once

#include <string>
#include <vector>

#include "epidemiology/disease.h"

namespace june {

// =============================================================================
// Disease Configuration Loader
// =============================================================================

namespace DiseaseLoader {
// Load disease from YAML config with trajectories and CSV outcome rates.
// When verbose is true, a [DEBUG] line is printed for each PDF-based curve
// (gamma, lognormal, beta) showing the old peak value and the
// max_infectiousness value required to preserve previous infectiousness
// magnitudes.
Disease loadFromYAML(const std::string& yaml_path, bool verbose = false,
                     std::vector<std::string>* referenced_paths = nullptr);

// Load outcome rates from filter-column CSV
OutcomeRates loadOutcomeRatesFromCSV(const std::string& csv_path);
}  // namespace DiseaseLoader

}  // namespace june
