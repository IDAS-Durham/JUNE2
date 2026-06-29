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
_Avoid_: activity type (ambiguous with venue type), event (see **Feast**).

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
holds even when a Venue hosts multiple Subsets serving different Fairs
(see **Fair**).
_Avoid_: group (too generic), cohort.

**Fair**:
A one-off scheduled event (not an Activity) that some people attend on a
specific date, requiring temporary accommodation away from residence.
Identified by a `fair_id`. Attendee selection uses one of two paths:
(a) **catchment-rule** — eligible people are drawn at trigger time from
`people_by_geo_unit` using a **Catchment rule**, requiring no pre-baked
membership. Current production path for MAY-generated worlds; or
(b) **membership-field scan** — person already has a `fair_id`
membership field on their `Fair_accommodation` venue, pre-baked at
world-build time. Legacy path only.
_Avoid_: feast, event (use **Fair** specifically for this recurring
scheduled-attendance concept; matches the source dataset's own term).

**Membership field**:
A sparse, named per-(person, Venue-membership) metadata value (e.g.
boarding/alighting time for a route leg, `fair_id` for a
`Fair_accommodation` membership). Used when a person's candidate Venue
list for an Activity needs disambiguating by something other than venue
identity.

**Catchment rule**:
A geo-unit eligibility list (`catchment_rule_id → [geo_unit_id, …]`)
resolved against `WorldState::people_by_geo_unit` at
calendar-event-trigger time to select Fair attendees. Not a Subset:
membership is never pre-baked; it is recomputed each time the Fair fires.
_Avoid_: Subset (pre-baked, world-build-time — the opposite of this).

## Relationships

- A **Person** has, per **Activity**, a list of candidate **Venue**
  (+ **Subset**) references — populated at world-load, never grown during
  the simulation.
- A **Fair** is not itself an **Activity** — it is calendar data that
  triggers a schedule hop into the (constant) `Fair_accommodation`
  **Activity**. The specific **Venue** is resolved at trigger time via
  one of two paths:
  - **Catchment-rule path**: attendees drawn from `people_by_geo_unit`
    using a **Catchment rule**; no pre-baked membership required. Used
    for MAY-generated worlds.
  - **Membership-field path**: person has a `fair_id` **Membership
    field** on their `Fair_accommodation` venue, pre-baked at world-build
    time. Used for legacy worlds.
- A **Person** using the membership-field path may have multiple candidate
  accommodation Venues under `Fair_accommodation` — one per **Fair**
  attended, disambiguated via `fair_id`.

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

## Flagged ambiguities

- An earlier session used the term "Feast" for this concept before
  settling on **Fair** (2026-06-19) — if "Feast" appears in older notes
  or branches, read it as **Fair**.
