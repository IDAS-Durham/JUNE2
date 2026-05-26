#pragma once

#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../activity/coordinated_encounter_manager.h"
#include "../activity/runtime_bin_allocator.h"
#include "../core/config.h"
#include "../core/world_state.h"
#include "../epidemiology/disease.h"
#include "../epidemiology/epidemiology.h"
#include "../epidemiology/infection_seed.h"
#include "../epidemiology/interaction_manager.h"
#include "../epidemiology/policy.h"
#include "../epidemiology/vaccination_manager.h"
#include "../loaders/disease_loader.h"
#include "../loaders/policy_loader.h"
#include "../utils/event_logging/event_logger.h"
#include "../utils/memory_utils.h"
#include "../utils/profiler.h"
#include "../utils/random.h"
#include "../utils/time_utils.h"
#include "simulation/compartmental_model_manager.h"

#ifdef USE_MPI
#include "../parallel/domain.h"
#include "../parallel/domain_manager.h"
#endif

namespace june {

#ifndef USE_MPI
// Forward declaration for when MPI is not available
class DomainManager;
#endif

class Simulator {
 public:
  Simulator(
      WorldState& world, const Config& config,
      DomainManager* domain_mgr = nullptr,
      const std::string& infection_seeds_file = "config/infection_seeds.yaml",
      const std::string& output_filename = "simulation_events.h5");

  // Run the full simulation
  void run();

  // Apply configured infection seeds at a specific time
  void applyInfectionSeeds(const std::string& current_datetime);

  // Get event logger for external access
  EventLogger* getEventLogger() { return &event_logger_; }

  // Write a checkpoint of all mutable simulation state (P3). Atomic: state
  // is written into "<root>.tmp/", then renamed; manifest.yaml is written
  // last as the commit marker. Per-rank shards keyed on global ids make the
  // checkpoint rank-count independent.
  void writeCheckpoint(int completed_day, const std::string& date_iso);

  // Restore all mutable state from a checkpoint directory (P4). Overlays the
  // delta onto the already-loaded world by global id, restores manager +
  // scalar state, rebuilds derived caches, and makes run() resume at
  // completed_day + 1. Rank-count independent.
  void restoreFromCheckpoint(const std::string& checkpoint_dir);

 private:
  WorldState& world_;
  const Config& config_;
  DomainManager*
      domain_mgr_;  // Optional: nullptr for serial mode, set for parallel mode

  // Disease and infection management
  std::unique_ptr<Disease> disease_;
  std::unique_ptr<InfectionSeeder> infection_seeder_;

  // Activity management
  ActivityManager activity_manager_;

  // Runtime bin allocator (carriages on transport_line venues etc.). Cheap
  // no-op when SimulationConfig::partial_presence is empty.
  std::unique_ptr<RuntimeBinAllocator> runtime_bin_allocator_;

  // Policy management (symptom-based behavior, lockdowns, etc.)
  std::unique_ptr<PolicyManager> policy_manager_;

  // Interaction and transmission management
  std::unique_ptr<InteractionManager> interaction_manager_;
  std::unique_ptr<VaccinationManager> vaccination_manager_;
  std::unique_ptr<CoordinatedEncounterManager> coordinated_encounter_manager_;

  std::unique_ptr<CompartmentalModelManager> compartmental_model_manager_;

  // Event logging
  EventLogger event_logger_;
  // Monotonic per-rank counter stamped into CoordinatedEncounterEvent.group_id
  // so every pair-row of the same real encounter shares the same id. Rank is
  // packed into the high 16 bits at stamping time for cross-rank uniqueness.
  uint64_t next_encounter_group_id_ = 0;

  // Day to resume the main loop at (set by restoreFromCheckpoint; 0 = fresh
  // run). Equals checkpoint completed_day + 1.
  int resume_from_day_ = 0;

  // Current simulation state
  std::tm current_date_;
  int current_day_num_;
  double current_simulation_time_;  // In days from start
  int total_days_;
  // Per-day-type occurrence counts (indexed by day type index)
  std::vector<int> day_type_counts_;

  // Person locations (person_id -> location)
  std::vector<PersonLocation> locations_;

  // Incremental lookup tracking
  std::unordered_set<PersonId>
      lookups_written_;  // Tracks which people have been saved to HDF5

  std::unique_ptr<Epidemiology> epidemiology_;

  // Returns the MPI rank of this process, or 0 for serial / no-MPI builds.
  int getRank() const {
#ifdef USE_MPI
    return domain_mgr_ ? domain_mgr_->getRank() : 0;
#else
    return 0;
#endif
  }

  // Simulation loop functions
  void runOneDay(int day, int rank);
  void simulateDay(int day_num);
  void simulateTimeSlot(const TimeSlot& slot, int time_slot_index,
                        int day_type_idx, double delta_hours);

  // Coordinated-encounter daily negotiation: generate proposals locally,
  // exchange across ranks, process, exchange replies, finalize, log locally,
  // and broadcast finalized encounters back so remote participants pick them
  // up. Runs the full Phases 1–4 when coordinated_encounter_manager_ is set.
  void negotiateAndLogDailyEncounters(int day, int rank);

  // Log this rank's finalized encounters with a rank-tagged group_id. Called
  // before the cross-rank merge so paired rows are never double-logged.
  void logFinalizedEncountersLocally(
      const std::vector<CoordinatedEncounter>& finalized, int rank);

  // Two-pass coordinated-encounter injection for this slot: build per-slot
  // lookups, dedup + sort daily_encounters, compute local eligibility,
  // Allgatherv global eligibility, then stamp venue_id / encounter_type_id
  // onto every eligible participant's PersonLocation.
  void injectCoordinatedEncountersIntoSlot(int time_slot_index);

#ifdef USE_MPI
  // Step 2 of simulateTimeSlot in MPI builds: trigger the cross-rank
  // visitor exchange, then fill `augmented_locations` with this rank's
  // locals at locally-owned venues plus incoming visitors (sorted by
  // person_id for deterministic processing order), and populate the
  // visitor_ids set + visitor_data_map used by transmission processing.
  void exchangeVisitorsAndBuildAugmented(
      double delta_hours,
      std::vector<PersonLocation>& augmented_locations,
      std::unordered_set<PersonId>& visitor_ids,
      std::unordered_map<PersonId, VisitorInfo>& visitor_data_map);
#endif

  // End-of-run output: write final epidemic events + lookups (collective on
  // all ranks; appends to the per-rank events file), then on rank 0 print
  // the wall-clock summary, activity/interaction stats, and encounter stats.
  void writeFinalEventsAndLookups(int rank);
  void printRunSummary();

  // End-of-day checkpoint trigger. Consults the configured cadence and, if
  // the day fires, announces on rank 0 and writes a checkpoint.
  void maybeWriteCheckpoint(int day, int rank);

  // Fomite initialization
  void initFomiteState();

  // Statistics and output
  void outputStatistics();
  void outputInfectionStatistics();
  void printSimulationState(const std::string& time_slot_name,
                            double delta_hours);

  // Dynamic flushing control
  std::string events_filename_;
  void checkAndFlushEvents(bool is_day_end = false);
};

}  // namespace june
