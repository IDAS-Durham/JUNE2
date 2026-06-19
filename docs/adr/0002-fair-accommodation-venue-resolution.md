---
status: accepted
---

> **Terminology note (2026-06-19):** this ADR was written using "feast"/
> `feast_id`; the project has since settled on **Fair**/`fair_id` (see
> `docs/CONTEXT.md`) to match the source dataset's own term. Read "feast"
> below as "Fair" — the decision is unchanged, only the name.

# Fair accommodation: static per-fair Subsets, disambiguated by a `fair_id` membership field, not dynamic runtime venue resolution

## Context
Feast accommodation needed a representation buildable into JUNE's
per-person schedules. Three options were considered: (A) constant
`Fair_accommodation` activity with a per-feast `Subset` reference baked
into each person's `activity_map`; (B) a distinct `activity_name` per
feast (~2000 values); (C) MAY only designates guest-houses per geo_unit,
and JUNE resolves a person's specific guest-house dynamically at
simulation runtime.

## Decision
Rejected C: JUNE's only "runtime" venue mechanisms (`ActivityManager::
selectVenue`, `PolicyManager::getReplacementLocation`) only choose among
a person's candidate venues already loaded from MAY's serialized
`activity_map` — there is no mechanism, and no precedent, for resolving a
venue against an external calendar at runtime. The motivation for C was
avoiding a large lookup table, not a genuine modelling need, so building
new runtime-resolution architecture for it was not worth the cost.

Rejected B: fair accommodation must interact with policy overrides
(quarantine/lockdown), but `PolicyAction::override_activity_mask` is a
`uint64_t` — only 64 distinct activities are policy-addressable
system-wide. ~2000 per-feast activity indices would leave nearly all of
them permanently unreachable by policy.

Accepted A, amended: constant `Fair_accommodation` activity, per-feast
`Subset`, direct reference in `activity_map`. A person attending multiple
feasts ends up with several candidate venues under that one activity
index; since venue identity is reused across feasts and the existing
runtime selection mechanisms aren't date-aware, disambiguation at
event-fire time uses the existing sparse membership-metadata side-table
(`WorldState::membership_field_values`) with a new `feast_id` field,
rather than `selectVenue()`/`specified_activity`.

## Consequences
- MAY must write one new membership field (`feast_id`) per accommodation
  membership row, in addition to the per-feast Subset.
- The calendar-event-trigger work must resolve accommodation venues via a
  dedicated `feast_id` lookup, not the general venue-selection path.
- Fair accommodation stays within the 64-slot policy-addressable budget
  and the 256-slot global `venue.type` budget.
