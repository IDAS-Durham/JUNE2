# Default contact matrix fails loud instead of falling back to a hardcoded scalar

`default_contacts_matrix` in `contact_matrices.yaml` was silently inert for the
`modes:` format used by every production config: the parser only read flat
top-level keys, so the matrix resolved empty and every lookup fell through to
a hardcoded scalar (`ContactMatrixConfig::default_contacts`, `2.0`) with no
error. We removed `default_contacts` entirely and require
`default_contacts_matrix` to be explicitly present in `contact_matrices.yaml`
(flat format, or `modes:` format matching the per-venue schema), failing
config load if it is missing. A venue/mode combination with no matrix
anywhere is now a load-time error, not a silent stand-in value — a scenario's
simulated infection numbers must always be traceable to a value the user
actually configured.
