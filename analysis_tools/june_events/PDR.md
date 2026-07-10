# june_events — PDR

Library of composable Python functions for reading `simulation_events.h5`.
Plotting/cosmetics are left to the caller; example notebooks are written
separately by the user. See [CONTEXT.md](./CONTEXT.md) for terminology.

## Goal

Give notebooks a small set of reliable building blocks — load a table, load
a lookup, decode a registry column, join event to lookup, inspect a file's
shape — that work across `simulation_events.h5` files that vary in size and
schema (registries, venue types, which optional tables exist).

## Scope (this phase)

- `io/`, `decode/`, `enrich/`, `inspect/` subpackages, each with a seam
  `__init__.py` that all outside callers go through.
- Event types: `infections`, `deaths`, `symptom_changes`.
- Lookup tables: `people`, `venues` (dedicated wrappers with auto byte-decode);
  any other `lookups/*` dataset (`people_properties/*`, `population_summary`,
  `population_networks/*`) reachable via the generic raw-table loader.
- Registry decode: generic, parameterised by column/registry/sentinel — not
  bespoke per field.
- Enrichment: id-based joins (event → people, event → venues) and one
  generalised time-aware join (state-at-event-time via `merge_asof`).
- Introspection: `inspect_file()` — structure, sizes, registries — with no
  data materialised.

## Out of scope (this phase)

- `coordinated_encounters` — any access at all, including metadata peek.
  Up to ~22GB / 691M rows on observed runs; needs a query interface, not a
  DataFrame loader. Revisit as its own phase.
- `world_state.h5` / geo enrichment (venue and person coordinates, names,
  levels). Separate concern from `simulation_events.h5`.
- Plotting.
- pip packaging (`pyproject.toml`) — plain importable package for now.
- Automated tests — deferred to implementation time.

## Package layout

```
analysis_tools/june_events/
  __init__.py
  CONTEXT.md
  PDR.md
  io/
    __init__.py        # seam: load_raw_table, load_people_lookup, load_venues_lookup
    raw_tables.py
    lookups.py
  decode/
    __init__.py         # seam: decode_registry_column, load_registry
    registries.py
    sentinels.py
  enrich/
    __init__.py          # seam: enrich_with_people, enrich_with_venues, enrich_with_state_at_time
    joins.py
    temporal.py
  inspect/
    __init__.py           # seam: inspect_file
    types.py               # FileSummary, DatasetSummary dataclasses
    scan.py
```

## Function sketches

`io/raw_tables.py`
```python
def load_raw_table(
    path: str,
    dataset_path: str,             # e.g. "events/infections"
    chunk_threshold_bytes: int = 500_000_000,
    chunk_rows: int = 5_000_000,
) -> Optional[pd.DataFrame]:
    """None + logged warning if dataset_path absent. Single read below
    threshold, row-chunked read+concat above it."""
```

`io/lookups.py`
```python
def load_people_lookup(path: str, include_properties: bool = True) -> Optional[pd.DataFrame]
def load_venues_lookup(path: str) -> Optional[pd.DataFrame]
    # both auto-decode dtype.kind == 'S' columns to str
```

`decode/sentinels.py`
```python
SEED_VENUE_ID = -999          # INFECTION_SEED_VENUE_ID in the engine
UNSET_REGISTRY_INDEX = 255    # unset uint8 registry index
```

`decode/registries.py`
```python
def load_registry(path: str, registry_name: str) -> Optional[list[str]]
def decode_registry_column(
    df: pd.DataFrame,
    id_column: str,
    registry: list[str],
    unset_value: int = UNSET_REGISTRY_INDEX,
    unset_label: str = "unknown",
) -> pd.Series
```

`enrich/joins.py`
```python
def enrich_with_people(event_df, people_lookup, prefix="person_") -> pd.DataFrame
def enrich_with_venues(event_df, venues_lookup, prefix="venue_") -> pd.DataFrame
    # left join; seed rows (venue_id == SEED_VENUE_ID) keep NaN venue_* columns by design
```

`enrich/temporal.py`
```python
def enrich_with_state_at_time(
    event_df: pd.DataFrame,
    state_change_df: pd.DataFrame,
    id_column: str,             # e.g. "infector_id"
    time_column: str = "time",
    state_column: str = "new_symptom_id",
) -> pd.DataFrame
    # backward merge_asof; generalises the infector-symptom-at-infection
    # pattern to any (id, time, state) triple
```

`inspect/types.py`
```python
@dataclass
class DatasetSummary:
    path: str
    n_rows: int
    dtype: str
    nbytes: int

@dataclass
class FileSummary:
    path: str
    datasets: list[DatasetSummary]
    registries: dict[str, list[str]]
```

`inspect/scan.py`
```python
def inspect_file(path: str) -> FileSummary
```

## Data model notes (from inspecting 5 real runs, 2026-07-10)

- `events/{infections,deaths,symptom_changes}` stay well under the chunking
  threshold even on multi-GB runs (e.g. 14.7M-row `symptom_changes` ≈ 0.3GB).
- `events/coordinated_encounters` ranges 227M–691M rows (~7–22GB) across the
  five runs inspected — confirms it needs its own phase, not a DataFrame load.
- `lookups/people` and `lookups/venues` are fixed-schema; `people_properties/*`
  and `population_networks/*` are dynamic per config (rat-related properties
  only appear in plague configs) — the generic raw-table loader handles these
  without needing per-field code.
- Root-level HDF5 attrs are empty on every file inspected — there is no
  schema-version marker in the file itself. `inspect_file()` is the only way
  to discover what a given file actually contains.
- Sentinels confirmed against `include/core/types.h`: `INFECTION_SEED_VENUE_ID
  = -999` (int32 `venue_id`), `255` (uint8 registry-index unset, e.g.
  `encounter_type_id`). `symptom_id`/`infector_symptom_id` (`uint16`) default
  to `0` — a real registry index (`symptoms[0]`), not a sentinel — so
  `decode_registry_column` is never called with an unset value on those
  columns; `UNSET_REGISTRY_INDEX` only applies to uint8 registry indices like
  `encounter_type_id`.
- `chunk_threshold_bytes = 500_000_000` / `chunk_rows = 5_000_000` confirmed as
  final defaults (Phase 2) — kept as a safety net per ADR-0004 even though no
  in-scope table on any inspected run reaches the threshold.
- `include_properties=True` on `load_people_lookup` attaches each
  `lookups/people_properties/*` dataset as a column by row position, not by an
  id join — confirmed against `src/utils/event_logging/event_writer_lookups.cpp`
  (`writePersonLookupTable`), which writes `lookups/people` and every
  `people_properties/*` dataset from the same ordered `world.people` iteration.

## Open questions

- `enrich_with_state_at_time`'s name/signature is a new generalisation (no
  existing precedent covers it) — worth re-checking once a second call site
  (e.g. deaths × infections) is written against it.
