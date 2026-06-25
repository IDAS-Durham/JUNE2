# Calendar event: simulator wiring notes

## What needs to happen

`CalendarEventManager` and its two CSV inputs exist and are fully unit-tested
but are not yet connected to the simulator. The simulator currently has no
reference to them.

Two things must fire during a simulation run:

1. **At startup** — the two CSV files must be loaded and a
   `CalendarEventManager` constructed from them.

2. **Once per simulation day** — `CalendarEventManager::triggerEventsForDay`
   must be called with the current day, the world, the people array, the
   random seed for that day, and the loaded catchment rules map.

The `active_event_` map inside `CalendarEventManager` is live state that
accumulates across days (it records which person is mid-hop on which event).
It needs to be saved and restored if the simulation is checkpointed and
resumed.

The schedules named in `calendar_events.csv` (`Fair_day_trip`,
`Fair_lodging`) must exist in `schedules.yaml` before the loader will accept
the file.

---

## Files already created

### Loaders (in `june_core`)

**`include/loaders/catchment_rule_loader.h`**  
**`src/loaders/catchment_rule_loader.cpp`**  
Parses `calendar_event_catchment_rules.csv` (long format:
`catchment_rule_id, geo_unit_id`) into
`std::unordered_map<int32_t, std::vector<GeoUnitId>>`.
Takes only a file path or istream — no `WorldState` needed.

**`include/loaders/calendar_event_loader.h`**  
**`src/loaders/calendar_event_loader.cpp`**  
Parses `calendar_events.csv` (filter.*-convention) into a day-indexed
`std::vector<std::vector<CalendarEvent>>`.
Requires a `WorldState` (to resolve `schedule_name` → index) and the
simulation start date and day count.
CSV columns: `calendar_event_id, date, schedule_name, hosting_geo_unit_id,
venue_type_name, catchment_rule_id, duration_days, compliance_rate, category`.
Optional `filter.*` columns are parsed into `CalendarEvent::attendee_filters`.
`duration_days` may be blank (defaults to 1).

### Manager

**`include/epidemiology/calendar_event.h`**  
**`src/epidemiology/calendar_event.cpp`**  
`CalendarEventManager` holds the day-indexed event table and the
per-person active-event map. Key method:

```cpp
void triggerEventsForDay(
    int day,
    const WorldState& world,
    std::vector<Person>& people,
    uint64_t base_seed,
    const std::unordered_map<int32_t, std::vector<GeoUnitId>>& catchment_rules = {});
```

Checkpoint accessors:
```cpp
const std::unordered_map<PersonId, int32_t>& getActiveEvents() const;
void setActiveEvents(std::unordered_map<PersonId, int32_t> active);
```

### World

**`WorldState::getVenuesInGeoUnit(GeoUnitId, std::string)`** —
added to `include/core/world_state.h` / `src/core/world_state.cpp`.
Used internally by the resolver; no wiring needed beyond what is already done.

### Person

**`Person::hop_repeats_remaining`** (int16_t, default 0) —
added to `include/core/types.h`.
`ActivityManager::advanceHoppedSchedule` already reads and decrements it
(multi-day looping is implemented). No further changes needed there.

---

## Example input files

Real-world CSVs produced by MAY are at:

```
~/Documents/MAY2/MAY/output/Medieval/calendar_events.csv
~/Documents/MAY2/MAY/output/Medieval/calendar_event_catchment_rules.csv
```

These are in the exact format the loaders expect.
