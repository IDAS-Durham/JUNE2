# june_events

Python library for reading `simulation_events.h5`, the event-log output of the
JUNE2 simulation engine (see [JUNE2 simulation engine](../../docs/CONTEXT.md)).
Provides functions to load, inspect, decode, and join the file's tables;
leaves plotting and analysis logic to the caller.

## Language

**Event type**:
One of the named tables under `events/` in `simulation_events.h5` (e.g.
`infections`, `deaths`, `symptom_changes`, `coordinated_encounters`) — one row
per occurrence, always carrying a `time` field. Not every file has every
event type.
_Avoid_: event group (HDF5 term, not a domain concept here).

**Raw event table**:
The unmodified contents of one `events/*` dataset, loaded as a DataFrame with
exactly the columns the file stores (byte-string columns undecoded, id
columns undecoded). Produced by `io/`. Distinct from an **Enrichment**, which
augments a raw event table with data from elsewhere.
_Avoid_: events dataframe (ambiguous with enriched output).

**Lookup table**:
A `lookups/*` table (`people`, `venues`, `people_properties/*`,
`population_summary`) that maps an id (`person_id`, `venue_id`) to static
metadata about that entity. Loaded by `io/`, joined onto event tables by
`enrich/`.
`people_properties/*` is a special case: each property (`ethnicity`,
`comorbidities`, etc.) is its own same-length dataset with no `person_id`
column of its own — row *i* corresponds to row *i* of `lookups/people` by
construction (both written from the same ordered iteration in the engine's
event writer), not by an id join.

**Registry**:
An ordered list of strings under `metadata/registries/*` (e.g.
`encounter_types`, `symptoms`) that a uint-coded column indexes into. The
column stores the index, not the string, to keep row size small.

**Sentinel value**:
A reserved id/index value meaning "not applicable" rather than a real
reference. Two conventions exist in the current schema: `venue_id == -999`
(`INFECTION_SEED_VENUE_ID` in the engine) marks a seed infection with no
venue; `255` marks an unset uint8 registry index (e.g. `encounter_type_id`).
The `255` registry sentinel must be excluded before a registry decode (see
**Registry decode**), not treated as data. `-999` is different: `lookups/venues`
carries a real row for `venue_id == -999` (`type == "infection_seed"`), so
joining on it is not an error case — it resolves to a meaningful label, not
`NaN`.

**Registry decode**:
Translating a registry-indexed column (e.g. `encounter_type_id`) into its
string values via the matching `metadata/registries/*` list, mapping the
sentinel to a fixed placeholder (e.g. `"unknown"`). Handled generically by
`decode/`, parameterised by column, registry, and sentinel — not one function
per field.

**Enrichment**:
Joining a raw event table against one or more lookup tables (by `person_id`,
`venue_id`, etc.) to attach metadata columns, or against another event table
via a time-aware join (e.g. an infector's symptom state at the moment of
transmission). Produced by `enrich/`; never mutates its inputs.

**Introspection**:
Reading a file's structure (which groups/datasets exist, row counts, byte
sizes, registry contents) without loading any event or lookup data. Exists
because different `simulation_events.h5` files vary — different registries,
different venue types, some missing `coordinated_encounters` or
`people_properties` entirely. Lives in `inspect/`, separate from `io/`
because it answers "what's in this file" rather than "give me this file's
data".

**Seam**:
The one file per subfolder (`io/__init__.py`, `decode/__init__.py`, etc.)
that all calls from outside the subfolder must go through. Internal files
within a subfolder are free to be restructured as long as the seam's
signatures hold.

## Relationships

- A **Raw event table** has zero or more columns that are **Registry**
  indices, decodable via `decode/` against the matching **Registry**.
- An **Enrichment** takes a **Raw event table** plus one or more
  **Lookup tables** (or another enriched event table, for the time-aware
  case) and returns a new, wider DataFrame — the inputs are untouched.
- **Introspection** never touches **Raw event table** or **Lookup table**
  contents — dataset shape/dtype only, read directly from HDF5 metadata.

## Example dialogue

> **Dev:** "Why isn't `load_infections()` returning venue names directly?"
> **Maintainer:** "`io/` only ever returns a **Raw event table** — undecoded,
> unjoined. You call `enrich_with_venues(raw_infections, venues_lookup)`
> yourself. Keeps memory bounded and lets you skip the join entirely for
> files where you don't need it."
>
> **Dev:** "What about `coordinated_encounters` — where's the loader?"
> **Maintainer:** "There isn't one yet. It's the largest **Event type** by
> far (up to ~22GB on the biggest runs) and needs a query interface, not a
> DataFrame loader — that's a later phase, not part of this library yet."

## Flagged ambiguities

- `-999` and `255` are both "sentinel values" but are not interchangeable:
  `-999` is a real `venue_id`'s reserved value (int32), `255` is a reserved
  *registry index* (uint8). A generic decode/sentinel helper must take the
  sentinel as a parameter rather than hardcoding one.
- `coordinated_encounters` is deliberately out of scope for this library's
  current phase (see ADR [0004](../../docs/adr/0004-size-checked-chunking-for-raw-table-loads.md)
  for why the raw-table loader is chunked at all) — do not add a bare
  `load_coordinated_encounters()` without first designing the query
  interface it actually needs.
