#pragma once

#include <climits>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "../core/types.h"

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
// rider — without exchanging bin indices.
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
  //   slot                       — current TimeSlot.
  //   current_simulation_time    — days since simulation start (float).
  //   delta_hours                — slot duration in hours (for FOI clamp,
  //                                consumed downstream).
  //   locations                  — per-person slot assignments just populated.
  //
  // The allocator includes EVERY leg of every multi-leg rider on partial-
  // presence venues, even legs whose raw (t_board_min, t_alight_min) sit
  // outside [0, slot_duration_min). This is intentional: long-distance
  // commuters (e.g. Durham → London, ~3 hr) must still expose riders on
  // their destination-region legs to capture geographic disease spread.
  // The downstream sub-interval FOI loop is responsible for distributing
  // each rider's compressed presence across their legs proportionally
  // (so total per-rider transit time is bounded by the slot duration).
  void allocateForSlot(int time_slot_index, int day_type_idx,
                       const TimeSlot& slot, double current_simulation_time,
                       double delta_hours,
                       const std::vector<PersonLocation>& locations);

  // Runtime bin index assigned to a specific (person, leg) — keyed by the
  // flat activity_venue index (same key as
  // WorldState::getMembershipField). Returns kNoBin if the leg isn't on a
  // partial-presence venue this slot.
  uint16_t getBinIndex(uint32_t av_idx) const {
    auto it = bin_by_av_idx_.find(av_idx);
    return it == bin_by_av_idx_.end() ? kNoBin : it->second;
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
  // of (rider, leg) pairs on partial-presence venues this slot — small
  // even at national scale (~150k at 60M).
  std::unordered_map<uint32_t, uint16_t> bin_by_av_idx_;
};

}  // namespace june
