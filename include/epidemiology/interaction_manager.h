#pragma once

#include <map>
#include <optional>
#include <random>
#include <unordered_set>
#include <vector>

#include "../activity/activity_manager.h"
#include "../activity/runtime_bin_allocator.h"
#include "../core/config.h"
#include "../core/world_state.h"
#include "../utils/age_utils.h"
#include "../utils/event_logging/event_logger.h"
#include "../utils/event_logging/event_types.h"
#include "../utils/time_utils.h"
#include "disease.h"
#include "policy.h"

namespace june {

class CompartmentalModelManager;  // optional plugin — null when inactive

// =============================================================================
// InteractionMember - Lightweight person reference
// =============================================================================
struct InteractionMember {
  PersonId id;
  size_t array_index;
  SubsetIndex subset_index;
  uint8_t encounter_type_id;
};

// =============================================================================
// Structures used in transmission processing (moved to header for reuse)
// =============================================================================

struct SusceptibleMember {
  PersonId id;
  double susceptibility;
  const VisitorInfo* visitor;
  uint8_t encounter_type_id;
};

struct BinGroup {
  std::vector<SusceptibleMember> susceptible;
  std::vector<PersonId> infectious_ids;
  // [mode_index][person_idx_within_bin] — per-mode infectiousness
  std::vector<std::vector<double>> infectiousness_by_mode;
  std::vector<double> total_infectiousness_by_mode;
  int total_size = 0;
  // [mode_index] cumulative weights derived from I_{b,m} for that mode.
  // Empty if the mode has no positive weight. Built once per bin/mode in
  // STEP 2 of processVenueTransmissions and sampled many times in STEP 3b
  // via sampleFromCumulative — replaces a per-call std::discrete_distribution
  // construction (the 116M "smoking gun" from the 60M profile).
  std::vector<std::vector<double>> cumulative_by_mode;
  // Per-fomite-mode deposition totals for this bin
  // [local_fomite_mode_index][sub_bin_k]
  std::vector<std::vector<double>> total_fomite_deposition_sub;

  // Clear person-proportional fields post-transmission while cache-hot.
  // Does NOT touch total_fomite_deposition_sub; that is handled exclusively
  // by initFomiteSubBins, called pre-use with the correct slot parameters.
  void clearAfterUse(int num_modes) {
    susceptible.clear();
    infectious_ids.clear();
    infectiousness_by_mode.resize(num_modes);
    for (auto& v : infectiousness_by_mode) v.clear();
    total_infectiousness_by_mode.assign(num_modes, 0.0);
    total_size = 0;
    cumulative_by_mode.resize(num_modes);
    for (auto& c : cumulative_by_mode) c.clear();
  }

  // Size and zero fomite sub-bin accumulators for the current slot's
  // delta_hours. Called pre-use so total_fomite_deposition_sub[fm] has exactly
  // n_sub_per_mode[fm] elements before any deposition is accumulated into them.
  void initFomiteSubBins(int num_fomite_modes,
                         const std::vector<int>& n_sub_per_mode) {
    total_fomite_deposition_sub.resize(num_fomite_modes);
    for (int fm = 0; fm < num_fomite_modes; ++fm) {
      int n_sub = fm < (int)n_sub_per_mode.size() ? n_sub_per_mode[fm] : 1;
      total_fomite_deposition_sub[fm].assign(n_sub, 0.0);
    }
  }
};

struct SourceEntry {
  int mode;
  int inf_bin;
  // For sibling-mixing sources, inf_bin == SIBLING_INF_BIN_SENTINEL and
  // sibling_parent_inf_bin holds the bin under the PARENT matrix to use
  // when sampling the infector from the per-parent flat list.
  int sibling_parent_inf_bin = -1;
};
// Sentinel for SourceEntry::inf_bin meaning "this source is sibling-venue
// mixing; sample infector from ParentAggregate". Distinct from -1 (fomite)
// and -2 (compartmental).
constexpr int SIBLING_INF_BIN_SENTINEL = -3;

// =============================================================================
// ParentAggregate - per-tick aggregation of infectiousness across all child
// venues of a single parent venue (e.g. classrooms of one school). Built in
// processTransmissions::buildParentAggregates() before the main per-venue
// loop. Each child venue (classroom/office/uni_groups) reads from its
// ParentAggregate to add a "sibling-mixing" force-of-infection term.
//
// All child venues of a parent live on the same MPI rank (children share
// the parent's MGU), so this map is rank-local with no MPI communication.
// =============================================================================
struct ParentInfectorEntry {
  PersonId person_id;
  VenueId child_venue_id;  // origin child, used to exclude own-venue at sample
  // Per-mode integrated infectiousness (24*∫I dt), same units as
  // BinGroup::infectiousness_by_mode entries.
  std::vector<double> inf_by_mode;
};

struct ParentAggregate {
  // [parent_bin][mode] → sum of integrated infectiousness across all children
  std::vector<std::vector<double>> total_inf_by_bin_mode;
  // [parent_bin] → headcount across all children
  std::vector<int> size_by_bin;
  // [parent_bin] → flat list of infectors from any child, used for sibling
  // infector sampling (filter by child_venue_id at sample time to exclude own)
  std::vector<std::vector<ParentInfectorEntry>> infectors_by_bin;

  // Per-child contributions, so each child can subtract its own share when
  // computing the sibling FOI it sees. Keyed by child venue id.
  // child_size[child_venue_id][parent_bin]
  std::unordered_map<VenueId, std::vector<int>> child_size_by_bin;
  // child_inf[child_venue_id][parent_bin][mode]
  std::unordered_map<VenueId, std::vector<std::vector<double>>>
      child_inf_by_bin_mode;
  // Cached parent venue metadata (filled at first insertion)
  uint8_t parent_venue_type_id = 255;
};

// =============================================================================
// CarriageMember — per-member record in a runtime carriage of a
// partial-presence venue. Built by buildPartialPresenceCarriages, consumed
// by the sub-interval accumulator inside computePartialPresenceLambda.
// =============================================================================
struct CarriageMember {
  PersonId pid;
  size_t array_index;
  SubsetIndex subset_index;
  uint8_t enc_type_id;
  Person* person;  // null for visitors
  const VisitorInfo* visitor;
  float eff_board;
  float eff_alight;
  int matrix_bin;
};

// =============================================================================
// PartialPresenceSubBin — per-(matrix-bin) scratch accumulator reused across
// sub-intervals in computePartialPresenceLambda. reset() clears storage to
// start a new sub-interval.
// =============================================================================
struct PartialPresenceSubBin {
  std::vector<double> total_inf_by_mode;  // size num_modes
  std::vector<PersonId> infectious_ids;
  // Per (mode) → flat list aligned with infectious_ids for sampling.
  std::vector<std::vector<double>> inf_per_person_by_mode;  // [mode][i]
  int total_size = 0;
  void reset(int num_modes_) {
    total_inf_by_mode.assign(num_modes_, 0.0);
    infectious_ids.clear();
    inf_per_person_by_mode.assign(num_modes_, {});
    total_size = 0;
  }
};

// =============================================================================
// InteractionManager - Handles disease transmission through interactions
// =============================================================================

class InteractionManager {
 public:
  struct PerformanceStats {
    size_t matrix_lookups = 0;
    size_t bin_lookups = 0;
    size_t grouping_ops = 0;

    void print() const {
      std::cout << "\n=== InteractionManager Performance Stats ==="
                << std::endl;
      std::cout << "  Matrix lookups:      " << matrix_lookups << std::endl;
      std::cout << "  Bin lookups:         " << bin_lookups << std::endl;
      std::cout << "  Grouping operations: " << grouping_ops << std::endl;
    }
  };

  InteractionManager(WorldState& world,
                     const ContactMatrixConfig& contact_matrices,
                     const SimulationConfig& simulation_config,
                     const ParallelConfig& parallel_config, Disease* disease,
                     EventLogger* event_logger = nullptr);

  // Calculate and apply transmissions for a time slot.
  // Returns the number of new infections.
  // Pass comp_model to enable compartmental uptake FOI; null = no plugin.
  int processTransmissions(
      const std::vector<PersonLocation>& locations, double current_time,
      double delta_hours,
      std::unordered_set<PersonId>* active_infections = nullptr,
      const std::unordered_set<PersonId>* visitor_ids = nullptr,
      std::vector<PendingInfection>* pending_infections = nullptr,
      const std::unordered_map<PersonId, VisitorInfo>* visitor_data = nullptr,
      const CompartmentalModelManager* comp_model = nullptr);

  PerformanceStats& getStats() { return stats_; }

  // Set the current day type index (used by EventLogger for per-type stats)
  void setCurrentDayTypeIdx(int idx) { current_day_type_idx_ = idx; }

  // Wire the runtime bin allocator so partial-presence venues (commute lines,
  // etc.) can resolve per-rider carriage assignments and presence windows.
  // Caller (the Simulator) sets this once after both objects exist. Null is
  // a valid state — partial-presence venues degrade to full-slot FOI without
  // an allocator.
  void setRuntimeBinAllocator(const RuntimeBinAllocator* a) {
    runtime_bin_allocator_ = a;
  }

  // Per-susceptible accumulated λ + source attribution from a single
  // partial-presence venue. Output of computePartialPresenceLambda; consumed
  // by processPartialPresenceVenue's Bernoulli step. Exposed publicly so
  // engine tests can assert the FOI math without driving the stochastic
  // infection draw.
  struct PartialPresenceAccumSource {
    int mode;
    PersonId infector;
    double weighted;
  };
  struct PartialPresenceLambdaResult {
    std::unordered_map<PersonId, double> susc_lambda;
    std::unordered_map<PersonId, std::vector<PartialPresenceAccumSource>>
        susc_sources;
  };

  // Steps 1–2 of partial-presence transmission processing, extracted as a
  // pure accumulator: bucket members into runtime bins ("carriages"), walk
  // sub-intervals delimited by effective presence windows, accumulate
  // per-susceptible λ and per-source attribution weights. No infection
  // side-effects, no RNG. Preconditions are enforced here (throws on
  // violation) so callers can't accidentally bypass them.
  //
  // The wrapping processPartialPresenceVenue calls this then performs the
  // single Bernoulli draw + infection write per susceptible. Tests call
  // this directly to assert λ deterministically.
  PartialPresenceLambdaResult computePartialPresenceLambda(
      const std::vector<InteractionMember>& members, Venue* venue,
      VenueId actual_venue_id, double current_time, double delta_hours,
      const std::unordered_map<PersonId, VisitorInfo>* visitor_data,
      uint8_t encounter_type_id);

 private:
  WorldState& world_;
  const ContactMatrixConfig& contact_matrices_;
  const SimulationConfig& simulation_config_;
  const ParallelConfig& parallel_config_;
  Disease* disease_;
  EventLogger* event_logger_;
  int current_day_type_idx_ = 0;
  std::unordered_map<uint8_t, std::string> encounter_subset_overrides_;

  // Pre-pass over locations (already sorted by venue) building
  // parent_aggregates_ for the current tick. Called once per
  // processTransmissions invocation before the per-venue loop. Iterates each
  // venue group, computes per-bin per-mode integrated infectiousness under
  // the PARENT's contact matrix, and stores per-child contributions so each
  // child can subtract its own share later. Walks members in person_id order
  // (the same order the main loop uses) for deterministic FP accumulation.
  void buildParentAggregates(
      double current_time, double delta_hours,
      const std::unordered_map<PersonId, VisitorInfo>* visitor_data);

  // Emit the per-tick parent-aggregate summary lines under
  // JUNE_DEBUG_PARENT_MIXING. Iterates parent_aggregates_ in sorted-key order
  // (deterministic across runs) and prints headline counts plus details of
  // up to 3 parents with non-zero infectiousness.
  void dumpParentAggregatesDebug(double current_time,
                                 double delta_hours) const;

  // Update parent_aggregates_ for the venue group [group_start, group_end)
  // (already grouped by venue_id in active_locations_buffer_). Returns early
  // if the venue is virtual, has no parent, or the parent's contact matrix
  // is missing. Walks members in person_id order so sibling FP sums match
  // the main loop's STEP 1 ordering across rank counts.
  void aggregateOneVenueGroupForParent(
      size_t group_start, size_t group_end, double current_time,
      double delta_hours, int num_modes,
      const std::unordered_map<PersonId, VisitorInfo>* visitor_data);

  // Look up parent_aggregates_[parent_id], lazily initialising its per-bin
  // arrays + the per-child-venue contribution entries. Returns the
  // ParentAggregate reference so the caller can accumulate into it directly.
  ParentAggregate& ensureParentAggregateInitialised(VenueId parent_id,
                                                    VenueId child_venue_id,
                                                    uint8_t parent_type_id,
                                                    int parent_num_bins,
                                                    int num_modes);

  // Fill inf_by_mode with the per-mode integrated infectiousness (24*∫I dt)
  // contributed by this member over [current_time, current_time+delta_hours].
  // Visitor branch reads pre-computed values from the sending rank; local
  // branch calls Infection::getIntegratedInfectiousness. Returns true iff
  // the member contributes any positive infectiousness.
  bool gatherMemberInfectiousnessByMode(
      const Person* person, const VisitorInfo* visitor, double current_time,
      double delta_hours, int num_modes,
      std::vector<double>& inf_by_mode) const;

  // Copy active_locations_buffer_[group_start..group_end) into a fresh vector
  // and sort it by person_id. Used by the parent-aggregate pre-pass to walk
  // venue members in the same person_id order as the main loop's STEP 1,
  // keeping the sibling FP sum order bit-identical across rank counts.
  std::vector<PersonLocation> buildPersonIdSortedMembers(
      size_t group_start, size_t group_end) const;

  // Resolve `pid` to a local Person* (preferring the hint at array_index)
  // or to a VisitorInfo* from visitor_data. Returns true iff at least one
  // resolved. Either out param is set to nullptr if not found.
  bool resolvePersonAndVisitor(
      PersonId pid, size_t array_index,
      const std::unordered_map<PersonId, VisitorInfo>* visitor_data,
      Person*& person_out, const VisitorInfo*& visitor_out) const;

  // Throws std::runtime_error if any v1 precondition of partial-presence FOI
  // is violated for this venue/encounter. Pure check, no side effects.
  void validatePartialPresencePreconditions(const Venue* venue,
                                            VenueId actual_venue_id,
                                            uint8_t encounter_type_id) const;

  // Step 1 of computePartialPresenceLambda: resolve each member's carriage
  // (via runtime_bin_allocator_), matrix_bin, and effective presence window,
  // and group them into one carriage bucket each. Each bucket is sorted by
  // person_id at the end so per-carriage FP accumulation matches across
  // ranks. Returns one bucket per carriage; some may be empty.
  std::vector<std::vector<CarriageMember>> buildPartialPresenceCarriages(
      const std::vector<InteractionMember>& members, Venue* venue,
      VenueId actual_venue_id, const ContactMatrix* matrix,
      int num_bins_needed, uint16_t num_bins,
      const std::unordered_map<PersonId, VisitorInfo>* visitor_data) const;

  // Collect [0, eff_board, eff_alight, slot_duration_min] event times for one
  // carriage, sort and de-duplicate within 1e-5 tolerance. The accumulator
  // walks sub-intervals delimited by consecutive entries. Returns the deduped
  // event times.
  std::vector<float> collectSubIntervalEventTimes(
      const std::vector<CarriageMember>& car, float slot_duration_min) const;

  // For sub-interval [t0, t1) of carriage `car`, walk every member present
  // throughout the sub-interval and classify it: (a) infectious — append to
  // sub_bins[bin].infectious_ids + push per-mode integrated infectiousness
  // scaled by `scale`; (b) susceptible — append &member to
  // susc_by_bin[bin]; (c) dead/no role — only update headcount. sub_bins is
  // pre-reset by the caller for this sub-interval.
  void classifyMembersInSubInterval(
      const std::vector<CarriageMember>& car, float t0, float t1, double scale,
      double current_time, double delta_hours, int num_modes,
      std::vector<PartialPresenceSubBin>& sub_bins,
      std::vector<std::vector<const CarriageMember*>>& susc_by_bin) const;

  // Return the contacts entry for (susc_bin, inf_bin), preferring
  // mode_matrix->contacts[][] when in bounds, falling back to
  // fallback_matrix->getContacts(...), else contact_matrices_.default_contacts.
  // Encapsulates the per-(mode, bin-pair) lookup with the same three-step
  // fallback chain used in both the main FOI loop and partial-presence.
  double lookupContactsForBinPair(const ContactMatrix* mode_matrix,
                                  const ContactMatrix* fallback_matrix,
                                  int susc_bin, int inf_bin) const;

  // For one (carriage, sub-interval), iterate (susc_bin, mode, inf_bin) over
  // pre-classified sub_bins + susc_by_bin and accumulate per-susceptible λ
  // contributions into susc_lambda and per-(susc, source) attribution
  // AccumSources into susc_sources. Pure FOI-math step; no infection writes,
  // no RNG. mode_matrix lookup, default-contacts fallback, and the
  // own-bin minus-one adjustment match the main-loop semantics.
  void accumulatePartialLambdaContributions(
      const std::vector<PartialPresenceSubBin>& sub_bins,
      const std::vector<std::vector<const CarriageMember*>>& susc_by_bin,
      uint8_t venue_type_id, const ContactMatrix* matrix, int num_bins_needed,
      int num_modes, const TransmissionParams& trans_params,
      PartialPresenceLambdaResult& result) const;

  // Walk the sub-intervals of one carriage, classifying members and
  // accumulating per-susceptible λ + AccumSources into result. sub_bins is
  // the caller-owned scratch buffer reused per sub-interval.
  void accumulateOneCarriage(
      const std::vector<CarriageMember>& car, float slot_duration_min,
      double current_time, double delta_hours, int num_modes,
      int num_bins_needed, uint8_t venue_type_id, const ContactMatrix* matrix,
      const TransmissionParams& trans_params,
      std::vector<PartialPresenceSubBin>& sub_bins,
      PartialPresenceLambdaResult& result) const;

  // Per-member body of the parent-aggregate pre-pass: resolve person/visitor,
  // compute parent_bin under parent_matrix, bump headcount in agg/csize,
  // gather per-mode infectiousness, and (if positive) accumulate into
  // agg.total_inf_by_bin_mode + cinf + agg.infectors_by_bin.
  void accumulateOneMemberIntoParent(
      const PersonLocation& loc, Venue* venue, const ContactMatrix* parent_matrix,
      int parent_num_bins, ParentAggregate& agg, std::vector<int>& csize,
      std::vector<std::vector<double>>& cinf, VenueId child_venue_id,
      double current_time, double delta_hours, int num_modes,
      const std::unordered_map<PersonId, VisitorInfo>* visitor_data,
      std::vector<double>& inf_by_mode_scratch) const;

  // Populate active_locations_buffer_ with non-unallocated entries from
  // `locations`, then sort by (venue_id, encounter_type_id-when-virtual).
  // Bumps stats_.grouping_ops.
  void filterAndSortActiveLocations(
      const std::vector<PersonLocation>& locations);

  // Count local (non-visitor) members of the current venue group whose
  // encounter_type_id names a coordinated encounter, and log via event_logger_
  // if any. group_start/group_end index into active_locations_buffer_.
  void logCoordinatedEncounterParticipants(
      size_t group_start, size_t group_end,
      const std::unordered_set<PersonId>* visitor_ids);

  // Fast pre-check: true iff processVenueTransmissions could possibly produce
  // transmission for this venue group, considering fomite history, compartmental
  // uptake, and the presence of any infectious member in group_members_buffer_.
  // Used to skip venues with zero infectious source before paying the FOI cost.
  bool venueGroupHasTransmissionSource(
      const Venue* venue, VenueId venue_id,
      const std::unordered_map<PersonId, VisitorInfo>* visitor_data,
      const CompartmentalModelManager* comp_model) const;

  // Starting at group_start (an index into active_locations_buffer_), advance
  // past every contiguous entry that shares the same venue_id (and, for virtual
  // venues, encounter_type_id) and copy each into group_members_buffer_. The
  // buffer is cleared first, then sorted by person_id at the end so that
  // FP accumulation in STEP 1 is deterministic across rank counts.
  // Returns the index one past the end of the group.
  size_t collectAndSortGroupMembers(size_t group_start);

  // Emit the per-tick parent-mixing summary line when JUNE_DEBUG_PARENT_MIXING
  // is enabled and any infections occurred this tick.
  void printTickParentMixingSummary(int total_new_infections,
                                    double current_time) const;

  // Process one contiguous venue group already loaded into
  // group_members_buffer_ (between group_start and group_end in
  // active_locations_buffer_). Resolves the venue, logs coordinated-encounter
  // participants, runs the fast pre-check, and dispatches to
  // processVenueTransmissions. Returns new infections (0 if group is skipped).
  int processOneVenueGroup(
      size_t group_start, size_t group_end, double current_time,
      double delta_hours, std::unordered_set<PersonId>* active_infections,
      const std::unordered_set<PersonId>* visitor_ids,
      std::vector<PendingInfection>* pending_infections,
      const std::unordered_map<PersonId, VisitorInfo>* visitor_data,
      const CompartmentalModelManager* comp_model);

  // Compute the bin index for a person under a given contact matrix.
  // Mirrors the STEP 1 logic in processVenueTransmissions so the pre-pass
  // can bin people under the PARENT matrix (which may have a different bin
  // scheme than the child).
  int computeBinIndexForMatrix(const Person* person, const Venue* venue,
                               SubsetIndex subset_index,
                               uint8_t encounter_type_id,
                               const ContactMatrix* matrix, int num_bins) const;

  // Process transmissions within a single venue/subset
  int processVenueTransmissions(
      const std::vector<InteractionMember>& members, Venue* venue,
      VenueId actual_venue_id, double current_time, double delta_hours,
      std::unordered_set<PersonId>* active_infections = nullptr,
      const std::unordered_set<PersonId>* visitor_ids = nullptr,
      std::vector<PendingInfection>* pending_infections = nullptr,
      const std::unordered_map<PersonId, VisitorInfo>* visitor_data = nullptr,
      uint8_t encounter_type_id = 255,
      const CompartmentalModelManager* comp_model = nullptr);

  // Return person_ids of susceptibles sorted ascending; the partial-presence
  // post-pass iterates susceptibles in this order so per-call work is
  // deterministic across MPI rank counts.
  std::vector<PersonId> orderSusceptibles(
      const std::unordered_map<PersonId, double>& susc_lambda) const;

  // Weight-sample one (mode, infector) from accumulated AccumSource entries.
  // Sorts in place by (mode, infector) for deterministic order, builds the
  // cumulative weights, and draws one sample with the given RNG. Returns
  // mode=0, infector=-1 when the source list is empty / all-zero-weight.
  std::pair<int, PersonId> sampleInfectorFromAccumSources(
      std::vector<PartialPresenceAccumSource>& srcs, SplitMix64& rng) const;

  // Look up the infector's current symptom id. For local persons reads from
  // Infection::getTrajectory(); for cross-rank visitors reads from
  // VisitorInfo::symptom_id. Returns 0 if infector_id is negative or neither
  // a local infection nor a visitor record exists.
  uint16_t resolveInfectorSymptomId(
      PersonId infector_id, double current_time,
      const std::unordered_map<PersonId, VisitorInfo>* visitor_data) const;

  // Apply a single partial-presence infection: either queue a
  // PendingInfection for the susceptible's home rank (if visitor susceptible)
  // or create the Infection in place and log it via event_logger_.
  void applyPartialPresenceInfection(
      PersonId susc_id, Person* susc_person, const VisitorInfo* visitor,
      PersonId infector_id, uint8_t transmission_mode_index,
      uint16_t infector_symptom_id, double current_time, Venue* venue,
      uint8_t venue_type_id, VenueId actual_venue_id,
      std::unordered_set<PersonId>* active_infections,
      std::vector<PendingInfection>* pending_infections);

  // Sibling of processVenueTransmissions for partial-presence venues
  // (transport_line, etc. — anything declared in
  // SimulationConfig::partial_presence). Per-rider carriage assignments come
  // from the runtime_bin_allocator_; per-rider effective presence windows
  // come from the membership_metadata side-table + presence_window helper.
  //
  // v1 scope (assumed and enforced — throws on violation):
  //   - Physical venue (actual_venue_id >= 0); not a virtual encounter venue.
  //   - No parent venue (transport lines have none in current MAY output).
  //   - No coordinated encounter participants (encounter_type_id == 255).
  //   - Direct-contact FOI only; no fomite / compartmental uptake on
  //     partial-presence venue types in v1.
  // Violations throw with a descriptive error rather than silently
  // falling back, per the project's no-silent-fallbacks rule.
  int processPartialPresenceVenue(
      const std::vector<InteractionMember>& members, Venue* venue,
      VenueId actual_venue_id, double current_time, double delta_hours,
      std::unordered_set<PersonId>* active_infections,
      const std::unordered_set<PersonId>* visitor_ids,
      std::vector<PendingInfection>* pending_infections,
      const std::unordered_map<PersonId, VisitorInfo>* visitor_data,
      uint8_t encounter_type_id, const CompartmentalModelManager* comp_model);

  PerformanceStats stats_;

  // Optimization buffers (reused across calls to avoid allocation)
  std::vector<PersonLocation> active_locations_buffer_;
  std::vector<InteractionMember> group_members_buffer_;

  // BinGroup reuse buffer
  std::vector<BinGroup> bins_buffer_;
  // Tracks which bin indices were actually used (for selective clearing)
  std::vector<int> used_bins_;

  // Source/weight buffers for transmission sampling
  std::vector<SourceEntry> sources_buffer_;
  std::vector<double> source_weights_buffer_;
  // Cumulative-weight scratch for source sampling (rebuilt per susc_bin,
  // sampled once per susceptible). Avoids per-bin std::discrete_distribution
  // allocation; see sampleFromCumulative in utils/random.h.
  std::vector<double> source_cumulative_buffer_;

  // Scratch buffers for sibling-infector two-stage sampling. Reused across
  // infections so we don't reallocate on every sibling-attributed event.
  std::vector<double> sibling_cum_buffer_;
  std::vector<size_t> sibling_pool_indices_buffer_;

  // Per-mode infectiousness scratch buffer
  std::vector<double> im_scratch_buffer_;

  // Cached uniform distribution for transmission rolls
  std::uniform_real_distribution<double> uniform_dist_{0.0, 1.0};

  // Base seed for deterministic per-entity RNG (MPI reproducibility)
  uint64_t base_seed_ = 0;

  // Per-tick parent-venue aggregates for cross-child-venue ("sibling")
  // mixing. Cleared and rebuilt at the start of each processTransmissions
  // call. Rank-local by construction: all child venues of a parent share
  // its MGU, so no MPI sync is required.
  std::unordered_map<VenueId, ParentAggregate> parent_aggregates_;

  // Wired by Simulator after construction. Null = no partial-presence
  // bucketing (sub-interval FOI is skipped; processVenueTransmissions
  // gate stays a no-op).
  const RuntimeBinAllocator* runtime_bin_allocator_ = nullptr;

  // Cached env-flag enabling verbose parent-mixing debug prints
  // (JUNE_DEBUG_PARENT_MIXING=1). Read once at construction.
  bool debug_parent_mixing_ = false;
  // Per-tick debug counters so we can summarise at the end of each tick
  // instead of spamming once per encounter.
  mutable int dbg_sibling_infections_ = 0;
  mutable int dbg_sample_susc_prints_ = 0;
  mutable int dbg_sample_infection_prints_ = 0;
};

}  // namespace june
