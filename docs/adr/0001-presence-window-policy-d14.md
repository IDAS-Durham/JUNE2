---
status: accepted
---

# Per-leg effective presence windows for partial-presence venues (D14)

> **Note:** this ADR is *reconstructed* from `include/activity/presence_window.h`,
> `src/activity/presence_window.cpp`, and `tests/test_presence_window.cpp`. Both
> source files cite "D14 from COMMUTE_HANDOVER §4" as the origin of this policy,
> but that document does not exist in this repository. This ADR captures the
> policy as currently implemented; if the original COMMUTE_HANDOVER doc
> resurfaces, reconcile against it.

## Context

Partial-presence venues (commute lines: `train_line`, `tube_line`, `bus_line`)
need a per-rider "effective presence window" — when within a time slot a rider
is actually exposed to other riders — so the FOI loop can do sub-interval
transmission. Riders may have multiple legs (interchanges) per slot, and some
riders' raw leg times may not fit inside, or even overlap, the slot window
(e.g. long-distance commuters).

## Decision

`computePresenceWindows` picks one of two policies for a rider's **entire**
leg list at once:

1. **Clamped** — used only if *every* leg overlaps `[0, slot_duration)` AND the
   rider's total raw leg duration fits within the slot. Returns real times,
   clamped to the slot boundary. Preserves exact partial-overlap physics
   (gaps between legs are preserved as zero exposure).
2. **Proportional** — used otherwise (any leg fails to overlap, OR total
   duration exceeds the slot, OR a leg is degenerate). Each leg's duration is
   compressed proportionally (`leg_dur / total_leg_dur * slot_duration`) and
   legs are packed sequentially from `0` to `slot_duration`, in journey order.
   No leg is ever dropped, even ones that sit entirely outside the slot.

The choice is all-or-nothing per rider: if any one leg fails the branch-1
test, *all* of that rider's legs (including ones that fit cleanly) are
proportionally compressed together.

## Consequences

- A rider whose only leg sits entirely outside `[0, slot_duration)` collapses
  to a single proportional segment spanning the *whole* slot
  (`eff_board=0, eff_alight=slot_duration`) — they appear present, and
  contribute their full integrated infectiousness, for the entire commute
  hour, even though their real journey was short and at an unrelated time.
- A rider whose journey mostly fits but has one outlying leg has *all* legs
  compressed, distorting the timing of legs that were physically accurate on
  their own.
- These are open concerns raised during a 2026-06-15 review (see
  conversation); not yet resolved as bugs vs. accepted trade-offs.
