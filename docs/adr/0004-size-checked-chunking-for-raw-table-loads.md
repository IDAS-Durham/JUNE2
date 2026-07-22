# Size-checked chunking for june_events raw table loads

`june_events`' raw-table loader inspects `dataset.nbytes` before deciding how
to read: below a threshold it does a single `dset[:]` read, above it reads in
row-chunks and concatenates. This deviates from the existing
`analysis_tools/analyse_simulation_events.ipynb` precedent, which always
chunks regardless of table size. We chose size-checking because most tables
in `simulation_events.h5` (`infections`, `deaths`, `symptom_changes`,
`lookups/*`) are well under the threshold on all observed runs, and paying
chunking overhead unconditionally has no benefit there — `coordinated_encounters`,
the one table where chunking matters (up to ~22GB on the largest observed
run), is out of scope for this library's current phase (see the `june_events`
[CONTEXT.md](../../analysis_tools/june_events/CONTEXT.md)), so the threshold
mainly exists as a safety net for future/unusual files rather than a hot
path.
