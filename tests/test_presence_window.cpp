#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <cmath>
#include <vector>

#include "activity/presence_window.h"
#include "doctest.h"

using namespace june;

// =============================================================================
// Unit tests for computePresenceWindows.
//
// The helper implements two branches (D14 in COMMUTE_HANDOVER §4):
//   - Clamped: every leg overlaps the slot AND total raw duration ≤ slot.
//     Returns max(0, tb), min(slot, ta) per leg. Preserves real timing.
//   - Proportional: otherwise. Compresses each leg to
//     (leg_dur / total_leg_dur) * slot, placed sequentially [0, c1) [c1,
//     c1+c2) … ending exactly at slot_duration_min. Last leg snaps to slot
//     end to absorb FP drift.
//
// Tests assert pure-function physics properties on synthetic inputs — no
// engine wiring, no I/O.
// =============================================================================

namespace {

constexpr float kSlotMin = 60.0f;
constexpr float kEps = 1e-4f;

}  // namespace

TEST_CASE("computePresenceWindows: empty input yields empty output") {
  auto out = computePresenceWindows(nullptr, nullptr, 0, kSlotMin);
  CHECK(out.empty());
}

TEST_CASE("computePresenceWindows: non-positive slot duration yields empty") {
  float tb[] = {0.0f};
  float ta[] = {10.0f};
  CHECK(computePresenceWindows(tb, ta, 1, 0.0f).empty());
  CHECK(computePresenceWindows(tb, ta, 1, -1.0f).empty());
}

TEST_CASE("clamped branch: single leg fully inside slot is unchanged") {
  // Leg [10, 30) of a 60-min slot — fits cleanly.
  float tb[] = {10.0f};
  float ta[] = {30.0f};
  auto out = computePresenceWindows(tb, ta, 1, kSlotMin);
  REQUIRE(out.size() == 1);
  CHECK(out[0].eff_board == doctest::Approx(10.0f).epsilon(kEps));
  CHECK(out[0].eff_alight == doctest::Approx(30.0f).epsilon(kEps));
}

TEST_CASE("clamped branch: partial overlap on a shared line") {
  // Two riders on the same line:
  //   A boards at 0, alights at 20 — present [0, 20)
  //   B boards at 10, alights at 20 — present [10, 20)
  // Their windows should be exactly the raw times — the helper does not
  // know about other riders; it operates per-rider. Overlap-only exposure
  // is the FOI loop's job. This test asserts the helper preserves the raw
  // times unchanged when they fit the slot.
  float tb_a[] = {0.0f};
  float ta_a[] = {20.0f};
  auto out_a = computePresenceWindows(tb_a, ta_a, 1, kSlotMin);
  REQUIRE(out_a.size() == 1);
  CHECK(out_a[0].eff_board == doctest::Approx(0.0f).epsilon(kEps));
  CHECK(out_a[0].eff_alight == doctest::Approx(20.0f).epsilon(kEps));

  float tb_b[] = {10.0f};
  float ta_b[] = {20.0f};
  auto out_b = computePresenceWindows(tb_b, ta_b, 1, kSlotMin);
  REQUIRE(out_b.size() == 1);
  CHECK(out_b[0].eff_board == doctest::Approx(10.0f).epsilon(kEps));
  CHECK(out_b[0].eff_alight == doctest::Approx(20.0f).epsilon(kEps));
}

TEST_CASE("clamped branch: multi-leg within slot with gap is unchanged") {
  // Two legs with an interchange gap [15, 18). Total raw duration = 35 ≤ 60.
  // Every leg overlaps the slot → clamped branch applies and the gap is
  // preserved as zero exposure in the FOI loop.
  float tb[] = {0.0f, 18.0f};
  float ta[] = {15.0f, 38.0f};
  auto out = computePresenceWindows(tb, ta, 2, kSlotMin);
  REQUIRE(out.size() == 2);
  CHECK(out[0].eff_board == doctest::Approx(0.0f).epsilon(kEps));
  CHECK(out[0].eff_alight == doctest::Approx(15.0f).epsilon(kEps));
  CHECK(out[1].eff_board == doctest::Approx(18.0f).epsilon(kEps));
  CHECK(out[1].eff_alight == doctest::Approx(38.0f).epsilon(kEps));
}

TEST_CASE("clamped branch: leg crossing slot start clamps to 0") {
  // Leg starts before the slot at t=-5, alights inside at t=20. Branch-1
  // boundary: ta > 0 means "overlaps the slot" so clamp; total duration
  // 25 ≤ 60.
  float tb[] = {-5.0f};
  float ta[] = {20.0f};
  auto out = computePresenceWindows(tb, ta, 1, kSlotMin);
  REQUIRE(out.size() == 1);
  CHECK(out[0].eff_board == doctest::Approx(0.0f).epsilon(kEps));
  CHECK(out[0].eff_alight == doctest::Approx(20.0f).epsilon(kEps));
}

TEST_CASE("clamped branch: leg crossing slot end clamps to slot") {
  // Leg boards inside slot at 50, alights past it at 70. tb < slot_duration
  // so "overlaps"; total duration 20 ≤ 60.
  float tb[] = {50.0f};
  float ta[] = {70.0f};
  auto out = computePresenceWindows(tb, ta, 1, kSlotMin);
  REQUIRE(out.size() == 1);
  CHECK(out[0].eff_board == doctest::Approx(50.0f).epsilon(kEps));
  CHECK(out[0].eff_alight == doctest::Approx(60.0f).epsilon(kEps));
}

TEST_CASE(
    "proportional branch: total duration exceeds slot triggers compression") {
  // Three legs totaling 90 min in a 60-min slot. Every leg overlaps but
  // total > slot → branch 2.
  //   leg 0 = 30 min raw → 30/90 * 60 = 20 min compressed → [0, 20)
  //   leg 1 = 30 min raw → 30/90 * 60 = 20 min compressed → [20, 40)
  //   leg 2 = 30 min raw → 30/90 * 60 = 20 min compressed → [40, 60)
  float tb[] = {0.0f, 30.0f, 50.0f};
  float ta[] = {30.0f, 60.0f, 80.0f};
  auto out = computePresenceWindows(tb, ta, 3, kSlotMin);
  REQUIRE(out.size() == 3);

  // Windows are emitted in original input order. With the tb array above
  // already monotonic, that matches journey order.
  CHECK(out[0].eff_board == doctest::Approx(0.0f).epsilon(kEps));
  CHECK(out[0].eff_alight == doctest::Approx(20.0f).epsilon(kEps));
  CHECK(out[1].eff_board == doctest::Approx(20.0f).epsilon(kEps));
  CHECK(out[1].eff_alight == doctest::Approx(40.0f).epsilon(kEps));
  CHECK(out[2].eff_board == doctest::Approx(40.0f).epsilon(kEps));
  CHECK(out[2].eff_alight == doctest::Approx(60.0f).epsilon(kEps));
}

TEST_CASE("proportional branch: leg sitting outside slot still gets a window") {
  // Two legs: one in slot, one starting at t=120 (past 60-min slot). The
  // far leg means "not every leg overlaps" → branch 2. Both legs must
  // still appear in the output (D14: no leg drop).
  float tb[] = {0.0f, 120.0f};
  float ta[] = {30.0f, 150.0f};
  auto out = computePresenceWindows(tb, ta, 2, kSlotMin);
  REQUIRE(out.size() == 2);
  // total = 60, comp = (30/60)*60 = 30 each
  CHECK(out[0].eff_alight - out[0].eff_board ==
        doctest::Approx(30.0f).epsilon(kEps));
  CHECK(out[1].eff_alight - out[1].eff_board ==
        doctest::Approx(30.0f).epsilon(kEps));
}

TEST_CASE("proportional branch: last leg ends exactly at slot duration") {
  // FP-drift guard: with non-power-of-two ratios, compressed durations
  // may not sum to exactly slot_duration_min. The helper must snap the
  // last leg's eff_alight to slot_duration_min to absorb this.
  float tb[] = {0.0f, 10.0f, 25.0f, 45.0f, 70.0f};
  float ta[] = {10.0f, 25.0f, 45.0f, 70.0f, 100.0f};  // 5 legs, total = 100
  auto out = computePresenceWindows(tb, ta, 5, kSlotMin);
  REQUIRE(out.size() == 5);
  CHECK(out[4].eff_alight == doctest::Approx(kSlotMin).epsilon(kEps));
}

TEST_CASE("proportional branch: windows are tile-contiguous starting at 0") {
  // In branch 2, leg k+1 starts where leg k ends; no gaps, no overlap.
  float tb[] = {0.0f, 30.0f, 70.0f};
  float ta[] = {25.0f, 65.0f, 100.0f};  // total = 80 > 60 → branch 2
  auto out = computePresenceWindows(tb, ta, 3, kSlotMin);
  REQUIRE(out.size() == 3);
  CHECK(out[0].eff_board == doctest::Approx(0.0f).epsilon(kEps));
  CHECK(out[0].eff_alight == doctest::Approx(out[1].eff_board).epsilon(kEps));
  CHECK(out[1].eff_alight == doctest::Approx(out[2].eff_board).epsilon(kEps));
  CHECK(out[2].eff_alight == doctest::Approx(kSlotMin).epsilon(kEps));
}

TEST_CASE(
    "proportional branch preserves input order even with shuffled boards") {
  // Caller passes legs out of natural journey order. Helper sorts internally
  // by t_board for sequential placement but emits results in INPUT order.
  float tb[] = {50.0f, 0.0f, 25.0f};
  float ta[] = {80.0f, 20.0f, 45.0f};  // total = 70 > 60
  auto out = computePresenceWindows(tb, ta, 3, kSlotMin);
  REQUIRE(out.size() == 3);
  // Sorted order is index 1 (tb=0) → index 2 (tb=25) → index 0 (tb=50).
  // total = 70, comp = (20+20+30) scaled by 60/70.
  // The sorted-first leg starts at 0, the sorted-last ends at slot_duration.
  CHECK(out[1].eff_board == doctest::Approx(0.0f).epsilon(kEps));
  CHECK(out[0].eff_alight == doctest::Approx(kSlotMin).epsilon(kEps));
  // No overlap between consecutive sorted legs.
  CHECK(out[1].eff_alight == doctest::Approx(out[2].eff_board).epsilon(kEps));
  CHECK(out[2].eff_alight == doctest::Approx(out[0].eff_board).epsilon(kEps));
}

TEST_CASE("zero-duration leg forces proportional branch, no leg dropped") {
  // A degenerate leg (tb == ta) makes leg_dur <= 0 — flips every_leg_overlaps
  // to false. Both legs must still appear in output.
  float tb[] = {0.0f, 30.0f};
  float ta[] = {0.0f, 50.0f};  // first leg has zero duration
  auto out = computePresenceWindows(tb, ta, 2, kSlotMin);
  REQUIRE(out.size() == 2);
  // total_leg_dur = 20 (only second leg contributes). First leg gets 0
  // compressed duration; second leg absorbs the whole slot.
  CHECK(out[0].eff_board == doctest::Approx(0.0f).epsilon(kEps));
  // First leg's compressed duration = 0/20 * 60 = 0.
  CHECK(out[0].eff_alight == doctest::Approx(0.0f).epsilon(kEps));
  // Second leg ends at slot_duration.
  CHECK(out[1].eff_alight == doctest::Approx(kSlotMin).epsilon(kEps));
}

TEST_CASE("all-zero legs fall back to equal-share slots, no leg dropped") {
  // Pathological input: every leg has zero raw duration. Helper falls
  // back to equal-share slots so every leg still gets a positive window
  // (per D14 no-leg-drop policy).
  float tb[] = {0.0f, 0.0f, 0.0f};
  float ta[] = {0.0f, 0.0f, 0.0f};
  auto out = computePresenceWindows(tb, ta, 3, kSlotMin);
  REQUIRE(out.size() == 3);
  CHECK(out[0].eff_board == doctest::Approx(0.0f).epsilon(kEps));
  // Last leg snaps to slot end; intermediate legs get equal shares.
  CHECK(out[2].eff_alight == doctest::Approx(kSlotMin).epsilon(kEps));
  // Each window has positive duration.
  for (const auto& w : out) {
    CHECK(w.eff_alight >= w.eff_board);
  }
}

TEST_CASE("branch boundary: total exactly equal to slot stays clamped") {
  // total_leg_dur == slot_duration_min is the equality edge of branch 1.
  // The condition is total <= slot → clamped applies; output should be the
  // raw times unchanged.
  float tb[] = {0.0f, 30.0f};
  float ta[] = {30.0f, 60.0f};  // total = 60 == slot
  auto out = computePresenceWindows(tb, ta, 2, kSlotMin);
  REQUIRE(out.size() == 2);
  CHECK(out[0].eff_board == doctest::Approx(0.0f).epsilon(kEps));
  CHECK(out[0].eff_alight == doctest::Approx(30.0f).epsilon(kEps));
  CHECK(out[1].eff_board == doctest::Approx(30.0f).epsilon(kEps));
  CHECK(out[1].eff_alight == doctest::Approx(60.0f).epsilon(kEps));
}

TEST_CASE("branch boundary: one minute over total triggers proportional") {
  // total_leg_dur = 61 > 60 → branch 2.
  float tb[] = {0.0f, 30.0f};
  float ta[] = {30.0f, 61.0f};
  auto out = computePresenceWindows(tb, ta, 2, kSlotMin);
  REQUIRE(out.size() == 2);
  // Branch-2 fingerprint: first leg starts at 0, last ends at slot.
  CHECK(out[0].eff_board == doctest::Approx(0.0f).epsilon(kEps));
  CHECK(out[1].eff_alight == doctest::Approx(kSlotMin).epsilon(kEps));
}

TEST_CASE("monotonicity: each window has eff_alight >= eff_board") {
  // Invariant the FOI loop relies on (sub-interval walk over sorted event
  // times). Tested on a mixed bag of inputs that exercise both branches.
  const std::vector<std::vector<std::pair<float, float>>> scenarios = {
      {{0.0f, 30.0f}},                  // single leg in slot
      {{-5.0f, 20.0f}},                 // leg crossing slot start
      {{50.0f, 70.0f}},                 // leg crossing slot end
      {{0.0f, 15.0f}, {18.0f, 38.0f}},  // clamped multi-leg
      {{0.0f, 30.0f}, {30.0f, 60.0f}, {60.0f, 90.0f}},  // proportional 3-leg
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
    for (const auto& w : out) {
      CHECK(w.eff_alight >= w.eff_board - kEps);
      CHECK(w.eff_board >= 0.0f - kEps);
      CHECK(w.eff_alight <= kSlotMin + kEps);
    }
  }
}
