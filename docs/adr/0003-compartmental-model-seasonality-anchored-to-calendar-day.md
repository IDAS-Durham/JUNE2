---
status: accepted
---

# Compartmental-model seasonality is anchored to calendar day-of-year, not simulation start_date

The rat-flea plague plugin's seasonal rate multipliers (`seasonal_multiplier` in
`plague_rats/ode.cpp`) use `sin(2π(t-phase)/365)`, with phase constants tuned
assuming `t=0 == Jan 1`. June was passing `current_simulation_time_` (days
elapsed since `config.time.start_date`) straight through to the plugin's
`advance()`. Since `config_plague_fitted`'s `start_date` is `1348-09-01`, this
silently shifted the seasonal curve ~243 days out of phase from what the
constants were calibrated for.

Fixed by computing a Jan-1-anchored day count at the one call site
(`simulator_timeslot.cpp`, where `compartmental_model_manager_->advance()` is
called) instead of introducing a second persistent time member alongside
`current_simulation_time_`. `current_date_` holds pre-1970 (medieval) dates, so
`std::tm::tm_yday`/`mktime` normalization is unavailable (see `isPreEpoch` in
`time_utils.h`); day-of-year is instead computed via the existing
`toJulianDay`/`tmToJulianDay` helpers, which already handle pre-epoch dates.

Phase constants were also recalibrated so rates peak/trough at the solstices
(day 172 = Jun 21 summer, day 355 = Dec 21 winter) rather than arbitrary
offsets:
- Birth group (peak summer, trough winter) — `rat_birth`, `flea_birth`,
  `larval_competition` — phase = 80.75 (`peak_day - 91.25`)
- Death group (peak winter, trough summer) — `rat_natural_death`,
  `free_flea_death`, `infectious_flea_death`, `larval_death` — phase = 263.75

`infectious_flea_death_seasonality_amplitude` and
`larval_death_seasonality_amplitude` were also raised from 0.0 to 0.3 — they
were previously dormant (any phase value was a no-op).
