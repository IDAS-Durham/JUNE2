---
status: accepted
---

# OTF venue allocation fires only as a fallback for unmapped activities

`ActivityManager::selectVenue` checks `getActivityVenues` first and only enters the OTF branch when that returns empty. This means OTF fires only for activities that have no pre-baked venue map.

The ordering is deliberate. Calendar-event hop schedules mix event-specific activities (e.g. `Fair_accommodation`, intentionally unmapped) with ordinary activities (e.g. `residence`, `social_contacts`, which have normal venue maps). If the OTF branch fired unconditionally at the top, ordinary activities during a hop would resolve via the OTF rule instead of the person's normal venues. The absence of a pre-baked venue map is the correct signal that an activity needs OTF resolution; an explicit allowlist of OTF-eligible activities was rejected as unnecessary coupling.

`filterAvailableActivities` gates on `hasRule` before calling `resolve`, so unmapped activities with no OTF rule are excluded before `selectVenue` is reached. `selectVenue` therefore does not need a redundant `hasRule` guard.
