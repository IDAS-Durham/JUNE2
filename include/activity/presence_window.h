#pragma once

#include <cstddef>
#include <vector>

namespace june {

// Per-leg effective time window within a slot, in minutes since slot start.
// eff_alight > eff_board; both lie in [0, slot_duration_min].
struct EffectiveWindow {
  float eff_board;
  float eff_alight;
};

// Compute per-leg effective presence windows for a single rider in a single
// slot.
//
// Inputs:
//   t_board_min, t_alight_min: raw leg times in minutes (one entry per leg,
//                               in natural journey order, t_alight > t_board).
//   slot_duration_min:          full slot length, e.g. 60.0 for a 1h slot.
//
// Output: one EffectiveWindow per input leg, same order.
//
// Policy (implements D14 from COMMUTE_HANDOVER §4):
//   1. If every leg overlaps [0, slot_duration_min) AND the rider's total
//      raw leg duration fits in the slot, return clamped real times:
//        eff_board  = max(0, t_board)
//        eff_alight = min(slot_duration_min, t_alight)
//      This preserves Chester-le-Street partial-overlap physics exactly
//      for commuters whose journey fits the slot.
//   2. Otherwise (long-distance commuter, or a leg sits outside the slot
//      window), distribute the rider's legs proportionally across the slot:
//        comp_dur_i = (leg_dur_i / total_leg_dur) * slot_duration_min
//        leg 1: [0, c1), leg 2: [c1, c1+c2), …, leg N ends at slot_duration_min
//      Bounds total per-rider transit FOI by the slot duration (no
//      overcount) while keeping geographic coverage of every leg.
//
// Empty input → empty output. n_legs must equal the size of both arrays.
std::vector<EffectiveWindow> computePresenceWindows(const float* t_board_min,
                                                    const float* t_alight_min,
                                                    std::size_t n_legs,
                                                    float slot_duration_min);

}  // namespace june
