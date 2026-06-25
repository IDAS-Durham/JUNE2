#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/config.h"
#include "core/types.h"

namespace june {

class WorldState;

// Membership-field key carrying the calendar-event id on a (person, Venue)
// accommodation membership row. MAY writes this; the resolver reads it. The
// only datum tying an attendee's candidate Venue to a specific event.
inline constexpr const char* kCalendarEventIdField = "calendar_event_id";

// A calendar-triggered event. On `start_day`, designated attendees hop onto
// the temporary schedule `schedule_type_idx`. The Venue each attendee occupies
// during the hop is re-derived at run-time from the `calendar_event_id`
// membership field — never cached (see ADR 0002). Generic: "Fair" appears only
// in the data (category / schedule name), never here.
struct CalendarEvent {
  int32_t calendar_event_id = -1;
  int start_day = -1;              // sim day (0-based) the event triggers on
  int16_t schedule_type_idx = -1;  // index into ScheduleConfig::schedule_types
  float compliance_rate = 1.0f;    // P(an eligible attendee actually hops)
  int16_t duration_days = 1;       // number of schedule-hop days (default 1)
  // Catchment-rule fields (catchment_rule_id == -1 → membership-field scan path)
  int32_t catchment_rule_id = -1;
  GeoUnitId hosting_geo_unit_id = -1;
  std::string venue_type_name;
  std::vector<SelectionCriterion> attendee_filters;
  // Diagnostics only — never used to pick attendees or resolve venues:
  VenueId venue_id = -1;
  SubsetIndex subset_index = -1;
  std::string category;  // free text (e.g. "fair"); logging/metrics only
};

// Owns calendar events, the thin per-person active-event-id state, and the
// run-time venue resolver. Holds no resolved venues — venue identity is always
// re-derived from membership_field_values (ADR 0002).
class CalendarEventManager {
 public:
  CalendarEventManager() = default;
  explicit CalendarEventManager(
      std::vector<std::vector<CalendarEvent>> events_by_day);

  // Merge an additional day-indexed table (e.g. a second category's CSV) into
  // the existing table. Grows the table if the new one is longer.
  void mergeEvents(const std::vector<std::vector<CalendarEvent>>& events_by_day);

  // Fire all events scheduled for `day`: for each, find its attendees (via the
  // membership reverse-scan or catchment-rule geo_unit lookup), and for each
  // compliant, non-colliding attendee set the hop fields and record the active
  // calendar_event_id. `base_seed` drives compliance rolls and venue hashing.
  // `catchment_rules` maps catchment_rule_id -> geo_unit list; may be empty
  // (falls back to membership-field scan for all events).
  void triggerEventsForDay(
      int day, const WorldState& world, std::vector<Person>& people,
      uint64_t base_seed,
      const std::unordered_map<int32_t, std::vector<GeoUnitId>>& catchment_rules =
          {});

  // Resolve the Venue for `person`'s active calendar event under `activity_idx`.
  // For catchment-rule events: hashes person+event id to pick among
  // getVenuesInGeoUnit candidates. For membership-field events: scans candidate
  // venues for a matching calendar_event_id membership value.
  // Returns {-1,-1} if the person has no active event or no venue can be found.
  std::pair<VenueId, SubsetIndex> resolveCalendarEventVenue(
      const WorldState& world, const Person& person, int16_t activity_idx) const;

  // Clear a person's active calendar event (called when their hop completes).
  void onHopCompleted(PersonId person_id);

  // Checkpoint accessors for the active-event-id map.
  const std::unordered_map<PersonId, int32_t>& getActiveEvents() const {
    return active_event_;
  }
  void setActiveEvents(std::unordered_map<PersonId, int32_t> active) {
    active_event_ = std::move(active);
  }

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

  // Events starting on `day` (empty if none / out of range). Read-only view for
  // diagnostics and the simulator's per-day trigger hook.
  const std::vector<CalendarEvent>& eventsForDay(int day) const;

  int numDays() const { return static_cast<int>(events_by_day_.size()); }

 private:
  // events_by_day_[day] -> events starting that day.
  std::vector<std::vector<CalendarEvent>> events_by_day_;
  // person -> opaque active calendar_event_id (never a venue).
  std::unordered_map<PersonId, int32_t> active_event_;
  // person -> pointer to the event that triggered their current hop.
  mutable std::unordered_map<PersonId, const CalendarEvent*> active_event_data_;
  // Lazily-built attendee cache: calendar_event_id -> attendee PersonIds.
  mutable std::unordered_map<int32_t, std::vector<PersonId>> attendees_by_event_;
  Stats stats_;
  uint64_t base_seed_ = 0;  // set by triggerEventsForDay, used by resolver

  // Attendees for a catchment-rule event: gathered from people_by_geo_unit for
  // each geo_unit in the catchment rule, filtered by event.attendee_filters.
  std::vector<PersonId> attendeesForCatchmentEvent(
      const CalendarEvent& event, const WorldState& world,
      const std::unordered_map<int32_t, std::vector<GeoUnitId>>&
          catchment_rules) const;

  // Attendees of `calendar_event_id`, derived by scanning membership rows for a
  // matching value (cached). `cei_field` is the resolved membership-field index.
  const std::vector<PersonId>& attendeesForMembershipEvent(
      int32_t calendar_event_id, const WorldState& world, int cei_field) const;
};

}  // namespace june
