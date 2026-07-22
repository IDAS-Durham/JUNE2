# infector_symptom_id: 0 means "recovered", 255 (kNoSymptomId) means "not applicable"

`Disease::getSymptomId` previously returned `0` both as the real registry id
for "recovered" and as its own error/unrecognised-name fallback, so a
seed-infection event (no real infector) was indistinguishable on disk from an
event where the infector had recovered. Commit `dbccfd1` narrowed
`infector_symptom_id`/`old_symptom_id`/`new_symptom_id` to `uint8_t` and
introduced `kNoSymptomId = 255` as the dedicated "not applicable" sentinel,
freeing `0` to mean only "recovered". Seed-infection rows in
`/events/infections` therefore now persist `255`, not `0`, for
`infector_symptom_id` — this is deliberate, not a regression, and any
downstream reader treating `0` as the "no infector / seed event" marker
should switch to checking for `255` (`kNoSymptomId`) instead.
