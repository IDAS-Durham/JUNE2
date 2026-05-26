#pragma once

#include <iostream>
#include <vector>

#include "../core/config.h"
#include "../core/types.h"
#include "../core/world_state.h"

#ifdef USE_MPI
#include <mpi.h>
#endif

namespace june {

class PolicyManager;

class ActivityManager {
 public:
  struct PerformanceStats {
    size_t map_allocations = 0;
    size_t string_lookups = 0;
    size_t weights_cached = 0;
    size_t rng_constructions = 0;

    void print() const {
      std::cout << "\n=== ActivityManager Performance Stats ===" << std::endl;
      std::cout << "  Map allocations:   " << map_allocations << std::endl;
      std::cout << "  String lookups:    " << string_lookups << std::endl;
      std::cout << "  Weights cached:    " << weights_cached << std::endl;
      std::cout << "  RNG constructions: " << rng_constructions << std::endl;
    }
  };

  ActivityManager(WorldState& world, const Config& config);

  // Assign schedule types to all people based on selection criteria
  void assignScheduleTypes();

  // Set policy manager (for runtime behavior overrides)
  void setPolicyManager(PolicyManager* policy_manager);

  // Set current simulation time (needed for policy checks)
  void setCurrentTime(double current_time) {
    current_simulation_time_ = current_time;
  }

  // Assign all people to activities for a given time slot
  void assignActivities(const TimeSlot& slot, int day_type_idx,
                        std::vector<PersonLocation>& locations);

  // Get performance stats
  PerformanceStats& getStats() { return stats_; }

  // Initialize locations (everyone starts at residence)
  void initializeLocations(std::vector<PersonLocation>& locations);

  // Pre-compute schedules for all people
  void precomputeSchedules();

  // Assign activities using pre-computed schedules
  void assignActivitiesFromSchedule(int time_slot_index, int day_type_idx,
                                    std::vector<PersonLocation>& locations);

 private:
  WorldState& world_;
  const Config& config_;

  // Policy manager (optional - nullptr if no policies)
  PolicyManager* policy_manager_ = nullptr;

  // Current simulation time (for policy checks)
  double current_simulation_time_ = 0.0;

  // Select activity for a person based on time slot and participation rates
  // Returns activity index (int16_t) instead of string for performance
  int16_t selectActivity(const Person& person, const TimeSlot& slot,
                         size_t slot_idx, const ScheduleType* schedule_type,
                         int day_type_idx, uint64_t time_key);

  // Select specific venue/subset for an activity
  std::pair<VenueId, SubsetIndex> selectVenue(
      const Person& person, int16_t activity_idx,
      const TimeSlot&
          slot,  // Used for specified_activity (to select specific venue index)
      uint64_t time_key);

  PerformanceStats stats_;

  // Caching
  int16_t dead_act_idx_ = -1;
  int16_t residence_act_idx_ = -1;
  int16_t none_act_idx_ = -1;
  int16_t no_venue_act_idx_ = -1;

  // Optimization buffers (reused to avoid allocations)
  std::vector<std::vector<std::pair<VenueId, SubsetIndex>>>
      venues_by_id_buffer_;
  std::vector<uint8_t> venue_type_ids_buffer_;
  std::vector<double> weights_buffer_;
  // Cumulative-weight scratch reused per selectVenue() call. Built from
  // weights_buffer_ via buildCumulative; sampled with sampleFromCumulative.
  // Replaces a per-call std::discrete_distribution construction.
  std::vector<double> cumulative_buffer_;
  // Cross-rank venues (not loaded locally) collected during selectVenue()
  std::vector<std::pair<VenueId, SubsetIndex>> cross_rank_venues_buffer_;

  void ensureIndicesCached();

  // Marks a PersonLocation as belonging to a dead person. Does not touch
  // person_id / person_array_index — caller sets those if needed.
  void setDeadLocation(PersonLocation& loc) const;

  // Base seed for deterministic per-entity RNG (MPI reproducibility)
  uint64_t base_seed_ = 0;

  // Handles a person who is currently on a hopped (temporary) schedule.
  // Assigns from flat_slots[temp_slot_progress] and advances the counter.
  // Auto-returns the person when all flat_slots are exhausted.
  // Returns true if the person was handled (caller should continue the loop).
  bool advanceHoppedSchedule(Person& person, PersonLocation& loc,
                             size_t person_array_idx, int day_type_idx);

  // Scans backwards through an already-hopped schedule's flat_slots to find
  // the last slot that produced a real venue (venue_id >= 0). Used to resolve
  // a pin venue when the policy fires during a no_venue transit slot.
  // Falls back to home residence if no prior overnight slot is found.
  std::pair<VenueId, SubsetIndex> findLastNonNullVenueOnHop(
      const Person& person);
};

}  // namespace june
