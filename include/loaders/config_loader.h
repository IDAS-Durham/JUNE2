#pragma once

#include <string>

#include "core/config.h"

namespace june {

class ConfigLoader {
 public:
  // Load all configuration files based on the master simulation config
  static Config loadAll(
      const std::string& simulation_file = "config/simulation.yaml");

  static VaccinationConfig loadVaccination(const std::string& filename);
  static ContactMatrixConfig loadContactMatrices(const std::string& filename);
  static CoordinatedEncounterConfig loadCoordinatedEncounters(
      const std::string& filename);
  static SimulationConfig loadSimulation(const std::string& filename);

 private:
  static ScheduleConfig loadSchedule(const std::string& filename);
  static PerformanceConfig loadPerformance(const std::string& filename);
  static ParallelConfig loadParallel(const std::string& filename);
  static ActivityPreferenceConfig loadActivityPreferences(
      const std::string& filename);
};

}  // namespace june
