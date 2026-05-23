#include "../../include/activity/presence_window.h"

#include <algorithm>
#include <cmath>

namespace june {

std::vector<EffectiveWindow> computePresenceWindows(const float* t_board_min,
                                                    const float* t_alight_min,
                                                    std::size_t n_legs,
                                                    float slot_duration_min) {
  std::vector<EffectiveWindow> out;
  if (n_legs == 0 || !(slot_duration_min > 0.0f)) return out;

  // Decide between branch 1 (clamped real times) and branch 2 (proportional
  // compression). Branch 1 applies only when every leg has nonzero overlap
  // with the slot window AND the rider's total raw leg duration fits within
  // the slot.
  float total_leg_dur = 0.0f;
  bool every_leg_overlaps = true;
  for (std::size_t i = 0; i < n_legs; ++i) {
    const float tb = t_board_min[i];
    const float ta = t_alight_min[i];
    const float leg_dur = ta - tb;
    if (!(leg_dur > 0.0f)) {
      // Degenerate leg: force proportional branch so we still place it.
      every_leg_overlaps = false;
    }
    total_leg_dur += std::max(0.0f, leg_dur);
    // "Overlaps the slot" means the half-open intersection of [tb, ta) and
    // [0, slot_duration_min) is non-empty.
    if (!(ta > 0.0f && tb < slot_duration_min)) {
      every_leg_overlaps = false;
    }
  }

  const bool use_clamped =
      every_leg_overlaps && total_leg_dur <= slot_duration_min;

  out.reserve(n_legs);

  if (use_clamped) {
    for (std::size_t i = 0; i < n_legs; ++i) {
      const float tb = std::max(0.0f, t_board_min[i]);
      const float ta = std::min(slot_duration_min, t_alight_min[i]);
      out.push_back({tb, ta});
    }
    return out;
  }

  // Proportional branch. Sort leg indices by t_board so sequential placement
  // follows natural journey order even if the caller passed legs out of
  // order. We emit results in the ORIGINAL input order so callers can match
  // window[i] ↔ leg i.
  std::vector<std::size_t> order(n_legs);
  for (std::size_t i = 0; i < n_legs; ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    if (t_board_min[a] != t_board_min[b])
      return t_board_min[a] < t_board_min[b];
    return a < b;  // stable tiebreak on degenerate inputs
  });

  // If total_leg_dur is zero (all degenerate), fall back to equal slots so
  // every leg still gets a positive window (no leg-drop per D14).
  const bool zero_total = !(total_leg_dur > 0.0f);

  out.assign(n_legs, EffectiveWindow{0.0f, 0.0f});
  float cursor = 0.0f;
  for (std::size_t k = 0; k < n_legs; ++k) {
    const std::size_t i = order[k];
    float leg_dur = std::max(0.0f, t_alight_min[i] - t_board_min[i]);
    float comp_dur = zero_total
                         ? (slot_duration_min / static_cast<float>(n_legs))
                         : (leg_dur / total_leg_dur) * slot_duration_min;
    // Last leg snaps its end exactly to slot_duration_min to absorb any FP
    // drift from accumulated divisions.
    const float eff_board = cursor;
    const float eff_alight =
        (k + 1 == n_legs) ? slot_duration_min
                          : std::min(slot_duration_min, cursor + comp_dur);
    out[i] = {eff_board, eff_alight};
    cursor = eff_alight;
  }
  return out;
}

}  // namespace june
