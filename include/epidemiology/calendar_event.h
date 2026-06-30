#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/config.h"
#include "core/types.h"

namespace june {

class WorldState;

// A calendar-triggered event. On `start_day`, attendees drawn from the
// catchment rule hop onto the temporary schedule `schedule_type_idx`. Each
// attendee's Venue is selected at resolve time from the candidate pool built
// by `candidate_venue_builder`. Generic: "Fair" appears only in the data
// (category / schedule name), never here.
struct CalendarEvent {
  int32_t calendar_event_id = -1;
  int start_day = -1;              // sim day (0-based) the event triggers on
  int16_t schedule_type_idx = -1;  // index into ScheduleConfig::schedule_types
  float compliance_rate = 1.0f;    // P(an eligible attendee actually hops)
  int16_t duration_days = 1;       // number of schedule-hop days (default 1)
  int32_t catchment_rule_id = -1;
  GeoUnitId hosting_geo_unit_id = -1;
  std::string venue_type_name;
  std::vector<SelectionCriterion> attendee_filters;
  // Optional override of the default candidate pool. The manager's default is
  // getVenuesInGeoUnit(hosting_geo_unit_id, venue_type_name) for catchment
  // events; tests set this to supply arbitrary venue pools instead.
  std::function<std::vector<VenueId>(const WorldState&)> candidate_venue_builder;
  // If set, called at resolve time to pick one venue from the cached pool.
  // Receives (candidates, person_id, seed); seed is pre-mixed from the trigger
  // seed + person_id + event_id, so the selector need not do any seeding itself.
  // Null falls back to the default hash-select.
  std::function<std::pair<VenueId, SubsetIndex>(
      const std::vector<VenueId>&, PersonId, uint64_t)> venue_selector;
  std::string category;  // free text (e.g. "fair"); logging/metrics only
};

// Owns calendar events and the thin per-person active-event-id state.
// Venue resolution is delegated to OnTheFlyVenueAllocator via
// getActiveHostingGeoUnit, which ActivityManager calls at assignment time.
class CalendarEventManager {
 public:
  CalendarEventManager() = default;
  explicit CalendarEventManager(
      std::vector<std::vector<CalendarEvent>> events_by_day);

  // events_by_id_ points into events_by_day_; move only.
  CalendarEventManager(const CalendarEventManager&) = delete;
  CalendarEventManager& operator=(const CalendarEventManager&) = delete;
  CalendarEventManager(CalendarEventManager&&) = default;
  CalendarEventManager& operator=(CalendarEventManager&&) = default;

  // Fire all events scheduled for `day`: for each, resolve attendees via the
  // catchment rule, then for each compliant, non-colliding attendee set the
  // hop fields and record the active calendar_event_id. `base_seed` drives
  // compliance rolls and venue hashing. `catchment_rules` maps
  // catchment_rule_id -> geo_unit list.
  void triggerEventsForDay(
      int day, const WorldState& world, std::vector<Person>& people,
      uint64_t base_seed,
      const std::unordered_map<int32_t, std::vector<GeoUnitId>>& catchment_rules =
          {});

  // Erase active-event entries for persons whose hop has completed (i.e.
  // !schedule_hop.isActive()). Called at the top of triggerEventsForDay so
  // stale entries are cleaned up once per day without coupling to ActivityManager.
  void sweepCompletedHops(const std::vector<Person>& people);

  // Checkpoint seam: serialisable state.
  struct Snapshot {
    std::unordered_map<PersonId, int32_t>  active_event;
  };
  Snapshot snapshot_for_checkpoint() const;
  void restore(Snapshot snapshot, const WorldState& world);

  // Trigger diagnostics.
  struct Stats {
    size_t triggered = 0;
    size_t skipped_collision = 0;
    size_t skipped_compliance = 0;
  };
  const Stats& stats() const { return stats_; }

  bool hasActiveEvent(PersonId person_id) const {
    return active_event_.find(person_id) != active_event_.end();
  }

  // Returns the hosting_geo_unit_id of the person's active event, or nullopt
  // if the person has no active event or the event has no hosting geo-unit.
  std::optional<GeoUnitId> getActiveHostingGeoUnit(PersonId person_id) const;


 private:
  // events_by_day_[day] -> events starting that day.
  std::vector<std::vector<CalendarEvent>> events_by_day_;
  // O(1) lookup from calendar_event_id to its definition (pointer into events_by_day_).
  std::unordered_map<int32_t, const CalendarEvent*> events_by_id_;
  // person -> opaque active calendar_event_id (never a venue).
  std::unordered_map<PersonId, int32_t> active_event_;
  Stats stats_;

  // Attendees for a catchment-rule event: gathered from people_by_geo_unit for
  // each geo_unit in the catchment rule, filtered by event.attendee_filters.
  std::vector<PersonId> attendeesForCatchmentEvent(
      const CalendarEvent& event, const WorldState& world,
      const std::unordered_map<int32_t, std::vector<GeoUnitId>>&
          catchment_rules) const;
};

static_assert(!std::is_copy_constructible_v<CalendarEventManager>);

}  // namespace june
