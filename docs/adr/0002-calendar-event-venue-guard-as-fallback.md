---
status: accepted
---

# Calendar-event venue resolver fires only as a fallback for unmapped activities

The `CalendarEventManager::resolveCalendarEventVenue` call in `ActivityManager::selectVenue` sits *after* the normal `getActivityVenues` lookup, not at the top of the function. It fires only when (a) the activity has no per-person venue map and (b) the person is explicitly on a calendar-event hop (`hasActiveEvent`).

This ordering is deliberate. Calendar-event hop schedules can mix event-specific activities (e.g. `fair_attendance`, which is intentionally unmapped) with ordinary activities (e.g. `residence`, `social_contacts`). If the guard fired unconditionally at the top, ordinary activities during a hop would resolve to the event venue instead of the person's normal home or contact venues. The fallback position lets `fair_attendance` reach the resolver (no venue map exists) while `residence` and `social_contacts` resolve normally (venue map exists, guard never reached). `no_venue` / transit slots are caught before either path with an early return of `{-1, -1}`.

The alternative — an explicit allowlist of activities that delegate to the resolver — was rejected as unnecessary coupling: the absence of a venue map is already the correct signal that an activity needs event-pool resolution.
