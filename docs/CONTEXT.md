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
holds even when a Venue hosts multiple Subsets serving different Feasts
(see **Feast**).
_Avoid_: group (too generic), cohort.

**Feast**:
A one-off scheduled event (not an Activity) that some people attend on a
specific date, requiring temporary accommodation away from residence.
Identified by a `feast_id`. A Feast has its own pool of candidate
accommodation Venues (Subsets), which is reused across many different
Feasts over the course of a simulation — venue identity alone does not
identify which Feast an accommodation assignment belongs to.
_Avoid_: fair, event (use **Feast** specifically for this recurring
scheduled-attendance concept).

**Membership field**:
A sparse, named per-(person, Venue-membership) metadata value (e.g.
boarding/alighting time for a route leg, `feast_id` for a
`Fair_accommodation` membership). Used when a person's candidate Venue
list for an Activity needs disambiguating by something other than venue
identity.

## Relationships

- A **Person** has, per **Activity**, a list of candidate **Venue**
  (+ **Subset**) references — populated at world-load, never grown during
  the simulation.
- A **Person** may have multiple candidate accommodation Venues under the
  single `Fair_accommodation` **Activity** — one per **Feast** attended.
  These are disambiguated via the `feast_id` **Membership field**, not by
  Venue identity.
- A **Feast** is not itself an **Activity** — it is calendar data that
  triggers a schedule hop into the (constant) `Fair_accommodation`
  **Activity**, with the specific **Venue** resolved via the `feast_id`
  **Membership field** match.

## Flagged ambiguities

- "dynamic allocation" was used to mean both (a) resolving a Venue at
  simulation runtime that wasn't known at world-build time, and (b)
  disambiguating among several already-known candidate Venues at the
  moment an event fires. JUNE2 only supports (b) — see ADR
  `0002-fair-accommodation-venue-resolution.md`.
