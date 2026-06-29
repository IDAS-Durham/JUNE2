#pragma once

#include <cstdint>
#include <functional>
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
  // If set, called once at trigger time to build the candidate venue pool for
  // this event. The pool is cached and used by venue_selector at resolve time.
  std::function<std::vector<VenueId>(const WorldState&)> candidate_venue_builder;
  // If set, called at resolve time to pick one venue from the cached pool.
  // Receives (candidates, person_id, seed); seed is pre-mixed from the trigger
  // seed + person_id + event_id, so the selector need not do any seeding itself.
  // Null falls back to the default hash-select.
  std::function<std::pair<VenueId, SubsetIndex>(
      const std::vector<VenueId>&, PersonId, uint64_t)> venue_selector;
  // Diagnostics only — never used to pick attendees or resolve venues:
  VenueId venue_id = -1;
  SubsetIndex subset_index = -1;
  std::string category;  // free text (e.g. "fair"); logging/metrics only
};

// Owns calendar events, the thin per-person active-event-id state, and the
// run-time venue resolver. The candidate venue list per event is cached eagerly
// at trigger time (see venue_candidates_cache_); the specific venue each person
// attends is hash-selected per-person, so ADR 0002 is respected.
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

  // Resolve the Venue for `person`'s active calendar event. Hashes person+event
  // id to pick from the cached candidate pool built at trigger time.
  // Returns {-1,-1} if the person has no active event or the candidate pool is
  // empty.
  std::pair<VenueId, SubsetIndex> resolveCalendarEventVenue(
      const Person& person) const;

  // Erase active-event entries for persons whose hop has completed (i.e.
  // hopped_schedule_id == -1). Called at the top of triggerEventsForDay so
  // stale entries are cleaned up once per day without coupling to ActivityManager.
  void sweepCompletedHops(const std::vector<Person>& people);

  // Checkpoint accessors for the active-event-id map.
  const std::unordered_map<PersonId, int32_t>& getActiveEvents() const {
    return active_event_;
  }
  const std::unordered_map<int32_t, uint64_t>& getEventTriggerSeeds() const {
    return event_trigger_seed_;
  }
  void setEventTriggerSeeds(std::unordered_map<int32_t, uint64_t> seeds) {
    event_trigger_seed_ = std::move(seeds);
  }
  // Rebuild venue_candidates_cache_ for all persons mid-hop after a checkpoint
  // restore. Must be called after setActiveEvents so the cache is consistent
  // with active_event_. Fixes the latent bug where catchment-path persons
  // would get {-1,-1} from the resolver because the cache was never populated.
  void rebuildVenueCachesAfterRestore(const WorldState& world);

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
  // O(1) lookup from calendar_event_id to its definition (pointer into events_by_day_).
  std::unordered_map<int32_t, const CalendarEvent*> events_by_id_;
  // person -> opaque active calendar_event_id (never a venue).
  std::unordered_map<PersonId, int32_t> active_event_;
  // Candidate venues per event, populated eagerly in triggerEventsForDay.
  // Avoids O(N_venues) scan per hopped person per slot.
  std::unordered_map<int32_t, std::vector<VenueId>> venue_candidates_cache_;
  Stats stats_;
  // Seed recorded when each event first fires; stable across days so that a
  // person's venue assignment is identical throughout a multi-day hop.
  std::unordered_map<int32_t, uint64_t> event_trigger_seed_;

  // Attendees for a catchment-rule event: gathered from people_by_geo_unit for
  // each geo_unit in the catchment rule, filtered by event.attendee_filters.
  std::vector<PersonId> attendeesForCatchmentEvent(
      const CalendarEvent& event, const WorldState& world,
      const std::unordered_map<int32_t, std::vector<GeoUnitId>>&
          catchment_rules) const;
};

static_assert(!std::is_copy_constructible_v<CalendarEventManager>);

}  // namespace june
