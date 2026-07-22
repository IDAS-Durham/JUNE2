# Context Map

## Contexts

- [JUNE2 simulation engine](./CONTEXT.md) — C++ engine that runs the disease-transmission/scheduling loop over a serialized world and writes `simulation_events.h5`
- [june_events analysis library](../analysis_tools/june_events/CONTEXT.md) — Python library for reading and querying a `simulation_events.h5` output file

## Relationships

- **JUNE2 simulation engine → june_events**: the engine writes `simulation_events.h5`; june_events only ever reads it. The file's on-disk schema (event tables, lookup tables, registries, sentinels) is the sole contract between the two contexts — june_events has no knowledge of `WorldState`, `ScheduleHop`, or any other in-process engine concept.
