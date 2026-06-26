#include "epidemiology/calendar_event.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <stdexcept>

#include "core/world_state.h"
#include "utils/deterministic_rng.h"
#include "utils/filtering.h"

namespace june {

namespace {

// Compare a float membership value to an int32 calendar_event_id, treating the
// absent-field sentinel as "no match". Ids are small ints, exact in float.
bool membershipMatches(float value, int32_t calendar_event_id) {
  if (value == WorldState::kMembershipFieldAbsent) return false;
  return static_cast<int32_t>(std::lround(value)) == calendar_event_id;
}

}  // namespace

CalendarEventManager::CalendarEventManager(
    std::vector<std::vector<CalendarEvent>> events_by_day)
    : events_by_day_(std::move(events_by_day)) {
  for (const auto& day_events : events_by_day_)
    for (const auto& ev : day_events)
      events_by_id_[ev.calendar_event_id] = &ev;
}

const std::vector<PersonId>& CalendarEventManager::attendeesForMembershipEvent(
    int32_t calendar_event_id, const WorldState& world, int cei_field) const {
  auto cached = attendees_by_event_.find(calendar_event_id);
  if (cached != attendees_by_event_.end()) return cached->second;

  std::vector<PersonId> attendees;
  for (const auto& person : world.people) {
    bool is_attendee = false;
    for (const auto& meta : world.getActivityMetas(person)) {
      for (uint16_t offset = 0; offset < meta.venue_count; ++offset) {
        uint32_t flat_idx = meta.venue_start + offset;
        if (membershipMatches(world.getMembershipField(flat_idx, cei_field),
                              calendar_event_id)) {
          is_attendee = true;
          break;
        }
      }
      if (is_attendee) break;
    }
    if (is_attendee) attendees.push_back(person.id);
  }
  return attendees_by_event_.emplace(calendar_event_id, std::move(attendees))
      .first->second;
}

std::vector<PersonId> CalendarEventManager::attendeesForCatchmentEvent(
    const CalendarEvent& event, const WorldState& world,
    const std::unordered_map<int32_t, std::vector<GeoUnitId>>&
        catchment_rules) const {
  std::vector<PersonId> attendees;
  auto rule_it = catchment_rules.find(event.catchment_rule_id);
  if (rule_it == catchment_rules.end()) return attendees;

  for (GeoUnitId geo_unit_id : rule_it->second) {
    auto ppl_it = world.people_by_geo_unit.find(geo_unit_id);
    if (ppl_it == world.people_by_geo_unit.end()) continue;
    for (uint32_t person_idx : ppl_it->second) {
      const Person& person = world.people[person_idx];
      if (event.attendee_filters.empty() ||
          filtering::matchesCriteria(person, &world, event.attendee_filters)) {
        attendees.push_back(person.id);
      }
    }
  }
  return attendees;
}

void CalendarEventManager::triggerEventsForDay(
    int day, const WorldState& world, std::vector<Person>& people,
    uint64_t base_seed,
    const std::unordered_map<int32_t, std::vector<GeoUnitId>>& catchment_rules) {
  if (day < 0 || day >= static_cast<int>(events_by_day_.size())) return;
  const auto& todays_events = events_by_day_[day];
  if (todays_events.empty()) return;

  base_seed_ = base_seed;

  // Only check for membership field when there are non-catchment events.
  int cei_field = -1;
  auto needs_membership = [](const CalendarEvent& e) {
    return e.catchment_rule_id < 0;
  };
  bool any_membership = std::any_of(todays_events.begin(), todays_events.end(),
                                    needs_membership);
  if (any_membership) {
    cei_field = world.getMembershipFieldIndex(kCalendarEventIdField);
    if (cei_field < 0) {
      throw std::runtime_error(
          std::string("CalendarEventManager: world has no '") +
          kCalendarEventIdField +
          "' membership field, but calendar events are configured");
    }
  }

  for (const auto& event : todays_events) {
    // Pre-populate venue candidates via the event's builder (if set), so
    // resolveCalendarEventVenue never does an O(N_venues) scan per person.
    if (event.candidate_venue_builder &&
        venue_candidates_cache_.find(event.calendar_event_id) ==
            venue_candidates_cache_.end()) {
      venue_candidates_cache_[event.calendar_event_id] =
          event.candidate_venue_builder(world);
    } else if (!event.candidate_venue_builder && event.catchment_rule_id >= 0 &&
               venue_candidates_cache_.find(event.calendar_event_id) ==
                   venue_candidates_cache_.end()) {
      venue_candidates_cache_[event.calendar_event_id] =
          world.getVenuesInGeoUnit(event.hosting_geo_unit_id,
                                   event.venue_type_name);
    }

    // Resolve attendee list: geography path or membership-field scan.
    std::vector<PersonId> catchment_attendees;
    const std::vector<PersonId>* attendee_ptr = nullptr;
    if (event.catchment_rule_id >= 0) {
      catchment_attendees =
          attendeesForCatchmentEvent(event, world, catchment_rules);
      attendee_ptr = &catchment_attendees;
    } else {
      attendee_ptr = &attendeesForMembershipEvent(event.calendar_event_id,
                                                  world, cei_field);
    }

    for (PersonId pid : *attendee_ptr) {
      auto idx_it = world.person_index.find(pid);
      if (idx_it == world.person_index.end()) continue;  // not on this rank
      Person& person = people[idx_it->second];

      // Collision: never override a person already mid-hop.
      if (person.hopped_schedule_id != -1) {
        ++stats_.skipped_collision;
        continue;
      }

      // Deterministic per-(person, event) compliance roll.
      SplitMix64 rng(mix_seed(base_seed, person.id,
                              static_cast<uint64_t>(event.calendar_event_id)));
      double roll = std::uniform_real_distribution<double>(0.0, 1.0)(rng);
      if (roll >= event.compliance_rate) {
        ++stats_.skipped_compliance;
        continue;
      }

      person.hopped_schedule_id = event.schedule_type_idx;
      person.return_schedule_id = -1;  // return to original schedule
      person.temp_slot_progress = 0;
      person.hop_repeats_remaining = event.duration_days;
      active_event_[person.id] = event.calendar_event_id;
      if (event.catchment_rule_id >= 0)
        active_catchment_persons_.insert(person.id);
      ++stats_.triggered;
    }
  }
}

std::pair<VenueId, SubsetIndex> CalendarEventManager::resolveCalendarEventVenue(
    const WorldState& world, const Person& person, int16_t activity_idx) const {
  auto active_it = active_event_.find(person.id);
  if (active_it == active_event_.end()) return {-1, -1};
  int32_t active_id = active_it->second;

  // Catchment-rule path: use pre-cached candidate venues.
  if (active_catchment_persons_.count(person.id)) {
    auto cache_it = venue_candidates_cache_.find(active_id);
    if (cache_it == venue_candidates_cache_.end() || cache_it->second.empty())
      return {-1, -1};
    const auto& candidates = cache_it->second;
    uint64_t seed = mix_seed(base_seed_, static_cast<uint64_t>(person.id),
                             static_cast<uint64_t>(active_id));
    auto ev_it = events_by_id_.find(active_id);
    if (ev_it != events_by_id_.end() && ev_it->second->venue_selector)
      return ev_it->second->venue_selector(candidates, person.id, seed);
    uint64_t h = SplitMix64(seed)();
    return {candidates[h % candidates.size()], 0};
  }

  // Membership-field path (non-catchment events or stale data).
  int cei_field = world.getMembershipFieldIndex(kCalendarEventIdField);
  if (cei_field < 0) return {-1, -1};

  for (const auto& meta : world.getActivityMetas(person)) {
    if (meta.activity_index != activity_idx) continue;
    for (uint16_t offset = 0; offset < meta.venue_count; ++offset) {
      uint32_t flat_idx = meta.venue_start + offset;
      if (membershipMatches(world.getMembershipField(flat_idx, cei_field),
                            active_id)) {
        return world.activity_venues[flat_idx];
      }
    }
  }
  return {-1, -1};
}

void CalendarEventManager::rebuildVenueCachesAfterRestore(
    const WorldState& world) {
  for (const auto& [pid, event_id] : active_event_) {
    if (venue_candidates_cache_.count(event_id)) continue;
    auto ev_it = events_by_id_.find(event_id);
    if (ev_it == events_by_id_.end()) continue;
    const CalendarEvent* ev = ev_it->second;
    if (ev->candidate_venue_builder)
      venue_candidates_cache_[event_id] = ev->candidate_venue_builder(world);
  }
}

void CalendarEventManager::onHopCompleted(PersonId person_id) {
  active_event_.erase(person_id);
  active_catchment_persons_.erase(person_id);
}

const std::vector<CalendarEvent>& CalendarEventManager::eventsForDay(
    int day) const {
  static const std::vector<CalendarEvent> kEmpty;
  if (day < 0 || day >= static_cast<int>(events_by_day_.size())) return kEmpty;
  return events_by_day_[day];
}

}  // namespace june
