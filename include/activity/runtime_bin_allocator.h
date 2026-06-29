#pragma once

#include <climits>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "core/types.h"
#include "presence_window.h"

namespace june {

class Config;
class WorldState;
struct TimeSlot;

// Per-slot allocator that buckets riders of partial-presence venues into
// ephemeral runtime bins (e.g. carriages of a train_line). Gated on
// SimulationConfig::partial_presence; an empty config (default) makes
// allocateForSlot a one-test no-op so non-commute scenarios pay zero cost.
//
// Multi-presence: a rider with N legs of a single activity (e.g. a 3-leg
// train commute) is placed in N bins, one per leg. Bin assignments are
// keyed on flat activity_venue index (the same index used by
// WorldState::getMembershipField), so a person is uniquely identified per
// leg without a 2D person×leg array.
//
// Bucketing is config-driven and capacity-free: per slot, the number of
// bins for a venue emerges as max(1, ceil(N_global_riders /
// target_group_size)), where target_group_size is a per-venue-type config knob
// (≈ vehicle occupancy). Bin assignment is a round-robin deal over a canonical
// hash-sort of the GLOBAL rider list, so bins differ in size by at most 1
// and bit-identical assignments are computed on every rank for the same
// rider without exchanging bin indices.
//
// MPI: one Allgatherv per slot collects (venue_id, person_id) pairs across
// ranks for partial-presence venues (with one pair per (person, leg) for
// multi-leg riders). Bucketing is then a pure function of (seed,
// slot_minutes, venue_id, person_id, num_bins) on every rank.
class RuntimeBinAllocator {
 public:
  static constexpr uint16_t kNoBin = UINT16_MAX;

  RuntimeBinAllocator(const WorldState& world, const Config& config);

  // Called once per slot, after ActivityManager::assignActivitiesFromSchedule
  // returns. Cheap no-op when partial_presence is disabled.
  //   slot:                       current TimeSlot.
  //   current_simulation_time:    days since simulation start (float).
  //   delta_hours:                slot duration in hours (for FOI clamp,
  //                                consumed downstream).
  //   locations:                  per-person slot assignments just populated.
  //
  // The allocator includes EVERY leg of every multi-leg rider on partial-
  // presence venues, even legs whose raw (t_board_min, t_alight_min) sit
  // outside [0, slot_duration_min). This is intentional: long-distance
  // commuters (e.g. Durham → London, ~3 hr) must still expose riders on
  // their destination-region legs to capture geographic disease spread.
  // Co-presence is computed on the raw line-local offsets (exact contacts);
  // each rider's total commute presence is capped at one slot-hour by the
  // per-rider presence factor f_p (see getPresenceFactor), applied downstream
  // in the FOI loop so the day budget stays bounded.
  void allocateForSlot(int time_slot_index, int day_type_idx,
                       const TimeSlot& slot, double current_simulation_time,
                       double delta_hours,
                       const std::vector<PersonLocation>& locations);

  // Runtime bin index assigned to a specific (person, leg), keyed by the
  // flat activity_venue index (same key as
  // WorldState::getMembershipField). Returns kNoBin if the leg isn't on a
  // partial-presence venue this slot.
  uint16_t getBinIndex(uint32_t av_idx) const {
    auto it = bin_by_av_idx_.find(av_idx);
    return it == bin_by_av_idx_.end() ? kNoBin : it->second;
  }

  // Runtime bin index for a specific (venue, person). Same value as the
  // av_idx-keyed lookup above, but for callers (e.g. the FOI loop) that
  // don't have the flat activity_venue index in hand. Returns kNoBin if
  // the (venue, person) pair wasn't bucketed this slot.
  uint16_t getBinIndex(VenueId venue_id, PersonId person_id) const {
    const uint64_t key = (static_cast<uint64_t>(venue_id) << 32) |
                         static_cast<uint64_t>(person_id);
    auto it = bin_by_vid_pid_.find(key);
    return it == bin_by_vid_pid_.end() ? kNoBin : it->second;
  }

  // Number of bins allocated for a venue this slot. Returns 0 if the venue
  // isn't partial-presence or has no riders this slot. Used by the FOI loop
  // to size per-carriage scratch buffers.
  uint16_t getNumBins(VenueId venue_id) const {
    auto it = num_bins_by_venue_.find(venue_id);
    return it == num_bins_by_venue_.end() ? 0 : it->second;
  }

  // Per-(venue, person) effective presence window for this slot, in minutes
  // since slot start. Computed on each rider's home rank (so the proportional
  // policy for long-distance commuters sees the rider's full leg list) and
  // broadcast globally via the same Allgatherv that distributes bin
  // assignments, so cross-rank visitors get the SAME window the home rank
  // computed  (required for MPI-deterministic FOI on cross-LGU venues).
  //
  // Returns {0, 0} if the (venue, person) wasn't bucketed this slot.
  EffectiveWindow getPresenceWindow(VenueId venue_id,
                                    PersonId person_id) const {
    const uint64_t key = (static_cast<uint64_t>(venue_id) << 32) |
                         static_cast<uint64_t>(person_id);
    auto it = windows_by_vid_pid_.find(key);
    return it == windows_by_vid_pid_.end() ? EffectiveWindow{0.0f, 0.0f}
                                           : it->second;
  }

  // Per-rider presence cap f_p = min(1, slot / T_p) for this slot, where T_p is
  // the rider's total transport minutes. f_p = 1 for journeys that fit the slot
  // (~99% of riders) and < 1 for over-long journeys, scaling that rider's FOI
  // contribution (as source AND sink) so its total modeled commute presence
  // stays ≤ one slot-hour. A pure function of the rider's own legs, computed on
  // the home rank and broadcast with the windows, so it is identical on every
  // rank (MPI bit-identity). Returns 1.0 if the (venue, person) wasn't bucketed
  // this slot.
  float getPresenceFactor(VenueId venue_id, PersonId person_id) const {
    const uint64_t key = (static_cast<uint64_t>(venue_id) << 32) |
                         static_cast<uint64_t>(person_id);
    auto it = f_presence_by_vid_pid_.find(key);
    return it == f_presence_by_vid_pid_.end() ? 1.0f : it->second;
  }

  // True iff the SimulationConfig declares any partial-presence venue types
  // that are actually present in this world.
  bool isActive() const { return venue_type_mask_ != 0; }

 private:
  const WorldState& world_;
  const Config& config_;

  // Bit i set iff venue type id i is a partial-presence type. Copied from
  // SimulationConfig::partial_presence::enabled_venue_type_mask at ctor.
  uint64_t venue_type_mask_ = 0;

  // Sparse map: flat activity_venue index → bin index for the current
  // slot. Cleared at the top of each call; size is bounded by the number
  // of (rider, leg) pairs on partial-presence venues this slot.
  std::unordered_map<uint32_t, uint16_t> bin_by_av_idx_;

  // Global (venue, person) → bin map, identical on every rank. Same data
  // as bin_by_av_idx_ but keyed for callers that don't have the flat
  // av_idx (the FOI loop only carries InteractionMember = {id, ...}).
  // Key layout: (uint64_t(venue_id) << 32) | uint64_t(person_id).
  std::unordered_map<uint64_t, uint16_t> bin_by_vid_pid_;

  // Per-venue bin count for this slot. Used by the FOI loop to size
  // per-carriage scratch buffers without re-deriving from rider counts.
  std::unordered_map<VenueId, uint16_t> num_bins_by_venue_;

  // Global (venue, person) → presence window. Same broadcast as
  // bin_by_vid_pid_; the FOI loop consults this for both local and
  // cross-rank visitor riders so windows are identical on every rank.
  std::unordered_map<uint64_t, EffectiveWindow> windows_by_vid_pid_;

  // Global (venue, person) → per-rider presence cap f_p. Broadcast alongside
  // the windows in the same Allgatherv so it is bit-identical on every rank.
  // The same f_p is replicated for each of a multi-leg rider's (venue, person)
  // keys (f_p is a rider-level property, not a per-venue one).
  std::unordered_map<uint64_t, float> f_presence_by_vid_pid_;
};

}  // namespace june
