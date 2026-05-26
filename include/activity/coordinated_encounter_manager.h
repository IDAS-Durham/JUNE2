#pragma once

#include <map>
#include <random>
#include <set>
#include <unordered_map>
#include <vector>

#include "../core/config.h"
#include "../core/world_state.h"
#include "../utils/deterministic_rng.h"
#include "coordinated_encounter_types.h"

namespace june {

class CoordinatedEncounterManager {
 public:
  CoordinatedEncounterManager(const WorldState& world, const Config& config,
                              int mpi_rank);

  // Main entry point for deciding if individuals want to propose encounters
  void generateProposals(int current_day,
                         std::vector<EncounterProposal>& out_proposals,
                         int day_type_idx);

  // Process incoming proposals and decide whether to accept.
  // host_proposals: proposals generated locally (where local people are hosts),
  //   needed for detecting mutual proposals across MPI ranks.
  void processProposals(const std::vector<EncounterProposal>& in_proposals,
                        const std::vector<EncounterProposal>& host_proposals,
                        std::vector<EncounterReply>& out_replies,
                        int day_type_idx);

  // Finalize encounters based on replies and distribute them
  void finalizeEncounters(const std::vector<EncounterReply>& replies,
                          std::vector<CoordinatedEncounter>& out_finalized);

  // Reset daily state
  void resetDaily();

  const std::vector<CoordinatedEncounter>& getDailyEncounters() const {
    return daily_encounters_;
  }

  // Add a remotely-finalized encounter to the daily list (for MPI)
  void addDailyEncounter(const CoordinatedEncounter& enc) {
    daily_encounters_.push_back(enc);
  }

 private:
  const WorldState& world_;
  const Config& config_;
  int mpi_rank_;

  std::vector<CoordinatedEncounter> daily_encounters_;

  // Per-person committed slot tracking (cleared daily)
  std::unordered_map<PersonId, std::set<int>> committed_slots_;

  // Per-person, per-frequency-group daily budget-hit cache (cleared daily).
  // Value is true iff the person "wins" a host-roll today for that group.
  // Computed once per (person, group) using a deterministic RNG keyed on
  // (random_seed, person_id, current_day, group_hash).
  std::unordered_map<PersonId, std::unordered_map<std::string, bool>>
      freq_group_hit_;

  // Per-person, per-frequency-group commitment (cleared daily). Once an
  // encounter of a given group fires for a person, no further encounter in
  // the same group can fire — the budget is spent.
  std::unordered_map<PersonId, std::unordered_map<std::string, bool>>
      freq_group_committed_;

  // Daily aggregate frequency-group budget stats for debug reporting.
  struct FreqGroupStats {
    int persons_evaluated = 0;
    int budget_hits = 0;
    int encounters_emitted = 0;
    double sum_daily_p = 0.0;
  };
  std::unordered_map<std::string, FreqGroupStats> freq_group_stats_;

  // Helper to get virtual venue type ID from the string name
  int getVirtualVenueTypeId(const std::string& matrix_name) const;

  // --- generateProposals helpers ---

  // Logs encounter definition config on Day 0, Rank 0 only
  void logEncounterConfig(const CoordinatedEncounterDef& enc_def) const;

  // First-call-wins populate of the per-person remaining-slot vector for the
  // given day_type. No-op on subsequent calls within the same day.
  void populateInitialRemainingSlotsIfAbsent(
      const Person& person, size_t person_idx, int day_type_idx,
      std::unordered_map<size_t, std::vector<int>>& remaining_slots) const;

  // Resolves today's per-(person, frequency_group) budget hit on first call
  // (using a group-specific RNG so the draw is independent of encounter
  // ordering) and caches the result for the rest of the day. Returns false if
  // the person has no budget today or has already spent it on a
  // higher-priority encounter type in the same group.
  bool isFrequencyGroupBudgetAvailable(const Person& person,
                                       const std::string& fg_name,
                                       int current_day);

  // Returns the slot indices valid for this encounter type from the person's
  // remaining pool
  std::vector<int> getValidSlotsForType(const Person& person,
                                        const CoordinatedEncounterDef& enc_def,
                                        const std::vector<int>& remaining,
                                        int day_type_idx) const;

  // Samples the per-type daily budget, clamped to available slot count
  int sampleTypeBudget(const CoordinatedEncounterDef& enc_def,
                       int num_valid_slots, SplitMix64& gen) const;

  // Venue selection result
  struct VenueSelection {
    VenueId id;
    int type_id;
    bool valid;
  };

  // Selects a venue (virtual or physical) for the encounter
  VenueSelection selectVenue(const Person& person,
                             const CoordinatedEncounterDef& enc_def,
                             int virtual_v_type, SplitMix64& gen) const;

  // Gathers eligible network partners for the person
  std::vector<PersonId> gatherEligiblePartners(
      const Person& person, const CoordinatedEncounterDef& enc_def) const;

  // Samples invite count, shuffles partners, and emits proposals into
  // out_proposals
  void emitProposals(const Person& person,
                     const CoordinatedEncounterDef& enc_def, int slot_idx,
                     const VenueSelection& venue,
                     std::vector<PersonId>& eligible_partners, SplitMix64& gen,
                     std::vector<EncounterProposal>& out_proposals);

  // Per-slot body of generateProposals. Runs the rate gate (frequency-group
  // bypass or proposal_probability roll), venue selection, partner gather, and
  // emitProposals. On success, marks the slot committed for the host and
  // (when fg_name_ptr is set) records the per-(person, group) commitment.
  // Returns true iff a proposal block was emitted.
  bool tryEmitForOneSlot(
      const Person& person, size_t person_idx,
      const CoordinatedEncounterDef& enc_def, int slot_idx, int virtual_v_type,
      const std::string* fg_name_ptr, SplitMix64& gen,
      std::uniform_real_distribution<double>& dist,
      std::unordered_map<size_t, std::vector<int>>& remaining_slots,
      std::vector<EncounterProposal>& out_proposals);

  // Per-(person, enc_def) body of generateProposals. Builds the per-person
  // RNG, runs the frequency-group gate, populates remaining slots, computes
  // the per-type budget, and iterates the per-slot emit loop.
  void proposeForOnePersonOneEncounter(
      const Person& person, size_t person_idx,
      const CoordinatedEncounterDef& enc_def, int current_day, int day_type_idx,
      int enc_type_counter, int virtual_v_type,
      std::unordered_map<size_t, std::vector<int>>& remaining_slots,
      std::vector<EncounterProposal>& out_proposals);

  // --- processProposals helpers ---

  // Finds the encounter definition matching a proposal's venue type
  const CoordinatedEncounterDef* findMatchingEncounterDef(
      const EncounterProposal& prop) const;

  // Checks if the invitee's schedule is compatible with the encounter at the
  // given slot
  bool isScheduleCompatible(const Person& invitee, int slot,
                            const CoordinatedEncounterDef& def,
                            int day_type_idx) const;

 public:
  // Daily aggregate debug summary for encounter pipeline
  struct DailyEncounterStats {
    // Per encounter type (by name)
    struct TypeStats {
      int proposals_generated = 0;
      int accepted = 0;
      int rejected_not_found = 0;
      int rejected_dead = 0;
      int rejected_committed = 0;
      int rejected_no_def = 0;
      int rejected_schedule = 0;
      int rejected_declined = 0;
      int finalized_encounters = 0;
      int total_participants = 0;
    };
    std::map<std::string, TypeStats> by_type;
    int total_proposals = 0;
    int total_finalized = 0;
  };

  // Call after each phase to accumulate stats
  void accumulateProposalStats(const std::vector<EncounterProposal>& proposals);
  void accumulateReplyStats(const std::vector<EncounterReply>& replies);
  void accumulateFinalizeStats(
      const std::vector<CoordinatedEncounter>& finalized);
  void printDailyEncounterSummary(int day) const;

 private:
  DailyEncounterStats daily_stats_;
};

}  // namespace june
