#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <cmath>
#include <vector>

#include "activity/presence_window.h"
#include "doctest.h"

using namespace june;

// =============================================================================
// Unit tests for computePresenceWindows.
//
// The helper emits, per leg, the RAW line-local interval [t_board, t_alight]
// (the leg's own venue clock) — no sort, no clamp to the slot, no proportional
// rescale. Separately it reports a per-rider presence cap
//   f_p = min(1, slot_duration_min / T_p),  T_p = Σ max(0, t_alight − t_board)
// which is 1 for journeys that fit a slot and < 1 only for over-long journeys.
// The cap (applied downstream in the FOI loop) conserves the day budget; the
// windows themselves are never distorted, so co-presence stays exact.
//
// Tests assert pure-function physics properties on synthetic inputs — no
// engine wiring, no I/O.
// =============================================================================

namespace {

constexpr float kSlotMin = 60.0f;
constexpr float kEps = 1e-4f;

}  // namespace

TEST_CASE("computePresenceWindows: empty input yields empty output, f_p=1") {
  float f = -1.0f;
  auto out = computePresenceWindows(nullptr, nullptr, 0, kSlotMin, &f);
  CHECK(out.empty());
  CHECK(f == doctest::Approx(1.0f).epsilon(kEps));
}

TEST_CASE("computePresenceWindows: non-positive slot duration yields empty") {
  float tb[] = {0.0f};
  float ta[] = {10.0f};
  CHECK(computePresenceWindows(tb, ta, 1, 0.0f).empty());
  CHECK(computePresenceWindows(tb, ta, 1, -1.0f).empty());
}

TEST_CASE("computePresenceWindows: f_presence_out is optional (null ok)") {
  float tb[] = {0.0f};
  float ta[] = {20.0f};
  auto out = computePresenceWindows(tb, ta, 1, kSlotMin, nullptr);
  REQUIRE(out.size() == 1);
  CHECK(out[0].eff_board == doctest::Approx(0.0f).epsilon(kEps));
  CHECK(out[0].eff_alight == doctest::Approx(20.0f).epsilon(kEps));
}

TEST_CASE("raw window: single leg inside slot is emitted unchanged, f_p=1") {
  float tb[] = {10.0f};
  float ta[] = {30.0f};
  float f = 0.0f;
  auto out = computePresenceWindows(tb, ta, 1, kSlotMin, &f);
  REQUIRE(out.size() == 1);
  CHECK(out[0].eff_board == doctest::Approx(10.0f).epsilon(kEps));
  CHECK(out[0].eff_alight == doctest::Approx(30.0f).epsilon(kEps));
  CHECK(f == doctest::Approx(1.0f).epsilon(kEps));
}

TEST_CASE("raw window: leg boarding past offset 60 is NOT clamped") {
  // Boards at line-offset 50, alights at 70 (duration 20 ≤ slot). The OLD code
  // clamped eff_alight to the slot (→ [50, 60]), erasing the real [60, 70]
  // co-presence. Raw emission preserves it: window is [50, 70], f_p=1.
  float tb[] = {50.0f};
  float ta[] = {70.0f};
  float f = 0.0f;
  auto out = computePresenceWindows(tb, ta, 1, kSlotMin, &f);
  REQUIRE(out.size() == 1);
  CHECK(out[0].eff_board == doctest::Approx(50.0f).epsilon(kEps));
  CHECK(out[0].eff_alight == doctest::Approx(70.0f).epsilon(kEps));
  CHECK(f == doctest::Approx(1.0f).epsilon(kEps));
}

TEST_CASE("raw window: a leg sitting entirely past the slot keeps its offsets") {
  // A leg in a far line's clock at [120, 150]. Duration 30 ≤ slot, so f_p=1;
  // the window is the raw offsets, never folded into [0, slot].
  float tb[] = {120.0f};
  float ta[] = {150.0f};
  float f = 0.0f;
  auto out = computePresenceWindows(tb, ta, 1, kSlotMin, &f);
  REQUIRE(out.size() == 1);
  CHECK(out[0].eff_board == doctest::Approx(120.0f).epsilon(kEps));
  CHECK(out[0].eff_alight == doctest::Approx(150.0f).epsilon(kEps));
  CHECK(f == doctest::Approx(1.0f).epsilon(kEps));
}

TEST_CASE("raw window: multi-leg windows are independent per leg") {
  // Two legs in different line clocks. Each window is the raw interval; legs
  // do NOT tile or get reordered. Total duration 15 + 20 = 35 ≤ slot → f_p=1.
  float tb[] = {0.0f, 3.0f};   // leg 1 in line-A clock, leg 2 in line-B clock
  float ta[] = {15.0f, 23.0f};
  float f = 0.0f;
  auto out = computePresenceWindows(tb, ta, 2, kSlotMin, &f);
  REQUIRE(out.size() == 2);
  CHECK(out[0].eff_board == doctest::Approx(0.0f).epsilon(kEps));
  CHECK(out[0].eff_alight == doctest::Approx(15.0f).epsilon(kEps));
  CHECK(out[1].eff_board == doctest::Approx(3.0f).epsilon(kEps));
  CHECK(out[1].eff_alight == doctest::Approx(23.0f).epsilon(kEps));
  CHECK(f == doctest::Approx(1.0f).epsilon(kEps));
}

TEST_CASE("f_p cap: journey exactly equal to slot stays f_p=1") {
  // T_p == slot is the boundary: cap stays 1 (windows raw, unchanged).
  float tb[] = {0.0f, 30.0f};
  float ta[] = {30.0f, 60.0f};  // total = 60 == slot
  float f = 0.0f;
  auto out = computePresenceWindows(tb, ta, 2, kSlotMin, &f);
  REQUIRE(out.size() == 2);
  CHECK(f == doctest::Approx(1.0f).epsilon(kEps));
  CHECK(out[1].eff_alight == doctest::Approx(60.0f).epsilon(kEps));
}

TEST_CASE("f_p cap: over-long journey gets f_p = slot / T_p, windows raw") {
  // Three legs totaling 90 min. Windows stay raw (no compression); only the
  // cap shrinks: f_p = 60 / 90 = 0.6667.
  float tb[] = {0.0f, 30.0f, 50.0f};
  float ta[] = {30.0f, 60.0f, 80.0f};  // durations 30 + 30 + 30 = 90
  float f = 0.0f;
  auto out = computePresenceWindows(tb, ta, 3, kSlotMin, &f);
  REQUIRE(out.size() == 3);
  // Raw, un-rescaled windows.
  CHECK(out[0].eff_board == doctest::Approx(0.0f).epsilon(kEps));
  CHECK(out[0].eff_alight == doctest::Approx(30.0f).epsilon(kEps));
  CHECK(out[2].eff_board == doctest::Approx(50.0f).epsilon(kEps));
  CHECK(out[2].eff_alight == doctest::Approx(80.0f).epsilon(kEps));
  CHECK(f == doctest::Approx(60.0f / 90.0f).epsilon(kEps));
}

TEST_CASE("f_p cap: budget conservation — Σ(d/slot × f_p) ≤ 1") {
  // The defining property: a rider's capped presence never exceeds one slot.
  const std::vector<std::vector<std::pair<float, float>>> journeys = {
      {{0.0f, 30.0f}},                                  // 30  → f=1
      {{0.0f, 30.0f}, {30.0f, 60.0f}, {60.0f, 90.0f}},  // 90  → f=2/3
      {{0.0f, 120.0f}},                                 // 120 → f=1/2
      {{5.0f, 9.0f}, {100.0f, 180.0f}},                 // 84  → f=60/84
  };
  for (const auto& j : journeys) {
    std::vector<float> tb, ta;
    float Tp = 0.0f;
    for (auto& [b, a] : j) {
      tb.push_back(b);
      ta.push_back(a);
      Tp += std::max(0.0f, a - b);
    }
    float f = 0.0f;
    auto out = computePresenceWindows(tb.data(), ta.data(), tb.size(), kSlotMin,
                                      &f);
    REQUIRE(out.size() == tb.size());
    // Each rider's total modeled presence = Σ (leg_dur / slot) × f_p ≤ 1.
    float modeled = 0.0f;
    for (const auto& w : out)
      modeled += ((w.eff_alight - w.eff_board) / kSlotMin) * f;
    CHECK(modeled <= 1.0f + kEps);
    // And it equals min(T_p, slot) / slot exactly.
    CHECK(modeled ==
          doctest::Approx(std::min(Tp, kSlotMin) / kSlotMin).epsilon(kEps));
  }
}

TEST_CASE("order independence: leg order does not change windows or f_p") {
  // The helper must not sort. Feeding the same legs in a different order
  // yields the same per-leg windows (in input order) and the same f_p.
  float tb1[] = {0.0f, 30.0f, 50.0f};
  float ta1[] = {30.0f, 60.0f, 80.0f};
  float f1 = 0.0f;
  auto out1 = computePresenceWindows(tb1, ta1, 3, kSlotMin, &f1);

  float tb2[] = {50.0f, 0.0f, 30.0f};  // shuffled
  float ta2[] = {80.0f, 30.0f, 60.0f};
  float f2 = 0.0f;
  auto out2 = computePresenceWindows(tb2, ta2, 3, kSlotMin, &f2);

  REQUIRE(out1.size() == 3);
  REQUIRE(out2.size() == 3);
  CHECK(f1 == doctest::Approx(f2).epsilon(kEps));
  // out2[0] is the shuffled-first leg (50,80); it must equal out1[2].
  CHECK(out2[0].eff_board == doctest::Approx(out1[2].eff_board).epsilon(kEps));
  CHECK(out2[0].eff_alight == doctest::Approx(out1[2].eff_alight).epsilon(kEps));
}

TEST_CASE("degenerate: zero-duration leg contributes 0 to T_p, no leg dropped") {
  // A leg with tb == ta has zero duration: it does not affect T_p (f_p=1 here)
  // and is still emitted as a (zero-width) raw window.
  float tb[] = {0.0f, 30.0f};
  float ta[] = {0.0f, 50.0f};  // first leg zero duration; total = 20
  float f = 0.0f;
  auto out = computePresenceWindows(tb, ta, 2, kSlotMin, &f);
  REQUIRE(out.size() == 2);
  CHECK(out[0].eff_board == doctest::Approx(0.0f).epsilon(kEps));
  CHECK(out[0].eff_alight == doctest::Approx(0.0f).epsilon(kEps));
  CHECK(out[1].eff_board == doctest::Approx(30.0f).epsilon(kEps));
  CHECK(out[1].eff_alight == doctest::Approx(50.0f).epsilon(kEps));
  CHECK(f == doctest::Approx(1.0f).epsilon(kEps));
}

TEST_CASE("degenerate: all-zero-duration legs yield f_p=1, all legs emitted") {
  float tb[] = {0.0f, 0.0f, 0.0f};
  float ta[] = {0.0f, 0.0f, 0.0f};
  float f = 0.0f;
  auto out = computePresenceWindows(tb, ta, 3, kSlotMin, &f);
  REQUIRE(out.size() == 3);
  CHECK(f == doctest::Approx(1.0f).epsilon(kEps));
  for (const auto& w : out) CHECK(w.eff_alight >= w.eff_board - kEps);
}

TEST_CASE("monotonicity: each window has eff_alight >= eff_board") {
  const std::vector<std::vector<std::pair<float, float>>> scenarios = {
      {{0.0f, 30.0f}},
      {{50.0f, 70.0f}},                                 // crosses offset 60
      {{0.0f, 15.0f}, {18.0f, 38.0f}},                  // multi-leg
      {{0.0f, 30.0f}, {30.0f, 60.0f}, {60.0f, 90.0f}},  // over-long
      {{0.0f, 120.0f}},                                 // far-overrunning leg
  };
  for (const auto& s : scenarios) {
    std::vector<float> tb, ta;
    for (const auto& [b, a] : s) {
      tb.push_back(b);
      ta.push_back(a);
    }
    auto out =
        computePresenceWindows(tb.data(), ta.data(), tb.size(), kSlotMin);
    REQUIRE(out.size() == tb.size());
    for (const auto& w : out) CHECK(w.eff_alight >= w.eff_board - kEps);
  }
}
