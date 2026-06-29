#pragma once

#include <cstddef>
#include <vector>

namespace june {

// Per-leg presence window for a single rider's leg, expressed in the leg's
// OWN line-local clock (minutes since that line/venue's first stop). These are
// the raw `t_board_min` / `t_alight_min` offsets: NOT slot-relative and NOT
// bounded by [0, slot_duration_min]. Co-presence on a venue is computed by
// intersecting the windows of riders sharing that venue (= that line's clock).
struct EffectiveWindow {
  float eff_board;
  float eff_alight;
};

// Compute per-leg presence windows + a per-rider presence cap for a single
// rider in a single commute slot.
//
// Inputs:
//   t_board_min, t_alight_min: raw line-local leg offsets in minutes (one
//                               entry per leg; order is irrelevant here).
//   slot_duration_min:          full slot length, e.g. 60.0 for a 1h slot.
//   f_presence_out:             optional; receives the rider's presence cap
//                                f_p = min(1, slot_duration_min / T_p), where
//                                T_p = Σ max(0, t_alight − t_board) is the
//                                rider's total minutes on transport this slot.
//                                f_p = 1 for journeys that fit the slot
//                                (~99% of riders); f_p < 1 only for journeys
//                                longer than one slot. Caps each rider's total
//                                modeled commute presence at one slot-hour so
//                                the day budget (≤ 24 h) is conserved.
//
// Output: one EffectiveWindow per input leg, in INPUT order, holding the raw
// [t_board, t_alight] for that leg. No sort, no offset-vs-slot comparison, no
// proportional rescale: within a venue all riders share that line's clock, so
// the raw offsets are a valid common frame for exact co-presence. The budget
// is enforced downstream via f_p, not by distorting the windows.
//
// Empty input → empty output, f_presence_out left at 1. n_legs must equal the
// size of both arrays.
std::vector<EffectiveWindow> computePresenceWindows(
    const float* t_board_min, const float* t_alight_min, std::size_t n_legs,
    float slot_duration_min, float* f_presence_out = nullptr);

}  // namespace june
