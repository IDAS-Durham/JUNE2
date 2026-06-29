# JUNE2

C++ epidemiological simulation engine. Consumes a serialized world (people,
venues, activities) produced by the MAY world-builder and runs the
disease-transmission/scheduling loop over it.

## Language

**Activity**:
A named category of thing a person can do (e.g. work, school, residence,
`Fair_accommodation`). Identified by a constant `activity_name`/
`activity_index`, shared across all people and all occurrences of that
category.
_Avoid_: activity type (ambiguous with venue type), event (see **Calendar Event**).

**Venue**:
A physical place a person can be assigned to for an activity (e.g. a
specific school, a specific guest-house). Has a `venue_id` and a
`venue.type` drawn from a single global registry capped at 256 distinct
types.

**Subset**:
A fixed group/role within a Venue (e.g. a class within a school, a room's
occupants within a guest-house). Membership is pre-baked at world-build
time and not modified during the simulation run. `subset_index` is local
to its Venue, so the pair `(venue_id, subset_index)` identifies a Subset
uniquely and globally — regardless of which Activity reaches it. This
holds even when a Venue hosts multiple Subsets serving different Calendar
Events (see **Calendar Event**).
_Avoid_: group (too generic), cohort.

**Calendar Event**:
A scheduled occurrence (not an Activity) that some people attend on a
specific date, triggering a temporary schedule hop. Examples include fairs
requiring accommodation away from residence. Identified by a
`calendar_event_id`. Attendee selection uses one of two paths:
(a) **catchment-rule** — eligible people are drawn at trigger time from
`people_by_geo_unit` using a **Catchment rule**, requiring no pre-baked
membership. Current production path for MAY-generated worlds; or
(b) **membership-field scan** — person already has a `calendar_event_id`
membership field on their accommodation venue, pre-baked at world-build
time. Legacy path only.
_Avoid_: feast, fair (too narrow — Calendar Event is the general concept).

**Membership field**:
A sparse, named per-(person, Venue-membership) metadata value (e.g.
boarding/alighting time for a route leg, `calendar_event_id` for an
accommodation membership). Used when a person's candidate Venue list for
an Activity needs disambiguating by something other than venue identity.

**Catchment rule**:
A geo-unit eligibility list (`catchment_rule_id → [geo_unit_id, …]`)
resolved against `WorldState::people_by_geo_unit` at
calendar-event-trigger time to select Calendar Event attendees. Not a
Subset: membership is never pre-baked; it is recomputed each time the
Calendar Event fires.
_Avoid_: Subset (pre-baked, world-build-time — the opposite of this).

**Schedule Hop**:
A temporary or permanent departure from a Person's assigned schedule type,
owned by the `ScheduleHop` struct on Person; active when
`hopped_schedule_id != -1`. A _temporary_ hop advances monotonically through
the hopped schedule's `flat_slots` (the counter is never reset on day-boundary
wrap, so backward scans reach earlier repeats via `% n`); `repeats_remaining`
counts full-cycle repeats still to run (0 = final/only). Auto-return fires when
a full cycle completes with none remaining. Whether a hop is temporary is a
property of its ScheduleType (`is_temporary`), not of the hop fields. All
temporary hops begin at progress = 0, via two call-site onset patterns:
_immediate_ (`maybeTriggerScheduleHop` runs slot 0 then advances, leaving
progress = 1) and _deferred_ (`triggerEventsForDay` stops after `begin()`; slot
0 runs on the first advance). _Permanent_ hops (a property-dispatched permanent
schedule, or a policy freeze-in-place swap) set `hopped_schedule_id` without
auto-return.
_Avoid_: "temp schedule", "hopped state".

## Relationships

- A **Person** has, per **Activity**, a list of candidate **Venue**
  (+ **Subset**) references — populated at world-load, never grown during
  the simulation.
- A **Calendar Event** is not itself an **Activity** — it is calendar data
  that triggers a schedule hop into a designated **Activity** (e.g.
  `Fair_accommodation`). The specific **Venue** is resolved at trigger
  time via one of two paths:
  - **Catchment-rule path**: attendees drawn from `people_by_geo_unit`
    using a **Catchment rule**; no pre-baked membership required. Used
    for MAY-generated worlds.
  - **Membership-field path**: person has a `calendar_event_id`
    **Membership field** on their accommodation venue, pre-baked at
    world-build time. Used for legacy worlds.
- A **Person** using the membership-field path may have multiple candidate
  accommodation Venues under the same Activity — one per **Calendar Event**
  attended, disambiguated via `calendar_event_id`.

**Venue assignment strategy**:
What determines which Venue an attendee occupies during a calendar-triggered
schedule hop. By default the eligible Venue pool is the venues of
`venue_type_name` located in `hosting_geo_unit_id` (`getVenuesInGeoUnit`); one
Venue is then chosen from that pool for a specific Person by deterministic
hash-select. An optional `candidate_venue_builder` overrides the default pool
(used by tests to supply arbitrary venues); an optional `venue_selector`
overrides the hash-select. Omitting both (the normal case) leaves the
struct-derived pool + hash-select. Only applicable to catchment-rule events;
membership-field events re-derive Venue from membership data instead.
_Avoid_: venue resolution strategy (resolution is the act of calling the
strategy, not the strategy itself).

**Visitor**:
A person attending a venue owned by a different MPI rank for a given time
slot. The person's disease state is sent to the venue's owning rank via MPI
exchange; transmission is computed there. If the visitor is infected, a
pending infection is routed back to the person's home rank and applied. The
person's `active_event_` entry (if any) and all simulation state are held
exclusively on their home rank.
_Avoid_: ghost, halo (no replication of person state across ranks).

## Flagged ambiguities

- An earlier session used the term "Feast" for this concept before
  settling on **Fair** (2026-06-19) — if "Feast" appears in older notes
  or branches, read it as **Calendar Event**.
- "Fair" was used as the canonical term from 2026-06-19 until 2026-06-29,
  when it was replaced by **Calendar Event** to reflect the broader concept
  (Fair is one example). If "Fair" appears in older notes, ADRs, or
  branches, read it as **Calendar Event**.
