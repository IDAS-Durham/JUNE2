#include "activity/presence_window.h"

#include <algorithm>

namespace june {

std::vector<EffectiveWindow> computePresenceWindows(const float* t_board_min,
                                                    const float* t_alight_min,
                                                    std::size_t n_legs,
                                                    float slot_duration_min,
                                                    float* f_presence_out) {
  if (f_presence_out) *f_presence_out = 1.0f;
  std::vector<EffectiveWindow> out;
  if (n_legs == 0 || !(slot_duration_min > 0.0f)) return out;

  // Raw-window emission. Each leg's presence window is its raw line-local
  // interval [t_board, t_alight] in that line's own clock. We do NOT sort,
  // clamp to the slot, or proportionally rescale: within a venue (= one line)
  // all riders share that line's clock, so the raw offsets are a valid common
  // frame for exact co-presence. The day budget is conserved separately by the
  // per-rider presence cap f_p, not by distorting the windows.
  float total_leg_dur = 0.0f;
  out.reserve(n_legs);
  for (std::size_t i = 0; i < n_legs; ++i) {
    const float tb = t_board_min[i];
    const float ta = t_alight_min[i];
    total_leg_dur += std::max(0.0f, ta - tb);
    out.push_back({tb, ta});
  }

  // Per-rider presence cap: f_p = min(1, slot / T_p). T_p is the rider's total
  // minutes on transport this slot. f_p < 1 only when the journey is longer
  // than one slot; applied to the FOI contribution downstream so a rider's
  // total modeled commute presence never exceeds one slot-hour.
  if (f_presence_out && total_leg_dur > slot_duration_min) {
    *f_presence_out = slot_duration_min / total_leg_dur;
  }
  return out;
}

}  // namespace june
