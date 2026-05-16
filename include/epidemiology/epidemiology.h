#pragma once

#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../core/types.h"
#include "../core/world_state.h"
#include "../utils/event_logger.h"
#include "disease.h"

namespace june {

struct EpiSlotStats {
  int transitions = 0;
  int recoveries = 0;
  int deaths = 0;
  int active_remaining = 0;
};

class Epidemiology {
 public:
  Epidemiology(WorldState& world, const Disease* disease,
               EventLogger* event_logger = nullptr);

  // Update infection states (symptom changes, recoveries, deaths) for all
  // actively infected people
  EpiSlotStats updateInfectionStates(
      double current_simulation_time,
      const std::vector<PersonLocation>& locations);

  // Apply decay to venue fomite loads based on time elapsed
  void updateVenueFomites(double current_simulation_time, double delta_hours);

  // Track a newly infected person
  void trackInfection(PersonId pid);

  // Remove a person from active infection tracking
  void untrackInfection(PersonId pid);

  // Get the set of actively infected person IDs
  const std::unordered_set<PersonId>& getActiveInfections() const {
    return active_infections_;
  }

  // Get mutable reference to active infections (e.g. for interaction manager)
  std::unordered_set<PersonId>& getActiveInfectionsMutable() {
    return active_infections_;
  }

 private:
  WorldState& world_;
  const Disease* disease_;
  EventLogger* event_logger_;

  // Only track people who are currently infected
  std::unordered_set<PersonId> active_infections_;

  // Track last processed transition time from trajectory for each infected
  // person This allows us to replay only new transitions from the trajectory
  std::unordered_map<PersonId, double> last_processed_transition_time_;
};

}  // namespace june
