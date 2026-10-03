#pragma once

#include <string>

#include "core/config.h"

namespace june {

namespace ConfigLoader {
// Load all configuration files based on the master simulation config
Config loadAll(const std::string& simulation_file);

VaccinationConfig loadVaccination(const std::string& filename);
ContactMatrixConfig loadContactMatrices(const std::string& filename);
CoordinatedEncounterConfig loadCoordinatedEncounters(
    const std::string& filename);
SimulationConfig loadSimulation(const std::string& filename);

ScheduleConfig loadSchedule(const std::string& filename);
PerformanceConfig loadPerformance(const std::string& filename);
ParallelConfig loadParallel(const std::string& filename);
ActivityPreferenceConfig loadActivityPreferences(const std::string& filename);
}  // namespace ConfigLoader

}  // namespace june
