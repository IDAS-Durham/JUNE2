# Default-mode contact matrix finalised in Simulator constructor, not ConfigLoader

`ContactMatrixConfig::resolve()` only built `default_mode_matrices_by_id`
(the array `getMatrix` consults for the per-mode default fallback) when
`mode_names` was non-empty. A config with `default_contacts_matrix.modes` but
no `contact_matrices:` venue `modes:` block left `mode_names` empty, so the
array was never built and the per-mode default was silently unreachable —
surfacing later as a runtime `std::runtime_error` from
`InteractionManager::lookupContactsForBinPair`, the exact failure mode ADR
0006 was meant to prevent.

Fix: added `ContactMatrixConfig::finalizeDefaultModeMatrices(world,
disease_mode_names)`, which rebuilds `default_mode_matrices_by_id` keyed to
an explicitly-supplied mode-name list rather than `mode_names`, and throws if
a disease mode has neither a per-mode default entry nor a flat
`default_matrix`. It is called from `Simulator::Simulator`, immediately after
`disease_` is loaded — the first point both `Disease` (source of the
canonical mode list) and `ContactMatrixConfig` co-exist. This required
widening `Simulator::config_` from `const Config&` to `Config&`.

Rejected: changing `ContactMatrixConfig::resolve(world)`'s signature to also
take the disease's mode list. Rejected because `resolve()` is called from
12+ test files with the current signature, and `resolve()` runs before
`Disease` is loaded in `Simulator`'s constructor order — threading the mode
list through would mean either reordering disease load earlier (larger,
riskier change) or passing it in separately anyway, which is what
`finalizeDefaultModeMatrices` already does as a distinct, later step.

Consequence: a disease mode missing from `contact_matrices.mode_names` and
every per-venue matrix is fine (falls back to the default). A disease mode
missing from the default itself is now always a fatal, loud error, but only
detected once `Disease` is loaded (`Simulator` construction), not at raw
config load — config load alone can no longer fully validate contact-matrix
coverage for configs that reach `ConfigLoader::loadContactMatrices` without
also loading the paired `Disease`.
