#include "epidemiology/calendar_event.h"

#include <random>

#include "core/world_state.h"
#include "utils/deterministic_rng.h"
#include "utils/filtering.h"

namespace june {

CalendarEventManager::CalendarEventManager(
    std::vector<std::vector<CalendarEvent>> events_by_day)
    : events_by_day_(std::move(events_by_day)) {
  for (const auto& day_events : events_by_day_)
    for (const auto& ev : day_events)
      events_by_id_[ev.calendar_event_id] = &ev;
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

void CalendarEventManager::sweepCompletedHops(
    const std::vector<Person>& people) {
  for (const Person& person : people) {
    if (person.hopped_schedule_id == -1)
      active_event_.erase(person.id);
  }
}

void CalendarEventManager::triggerEventsForDay(
    int day, const WorldState& world, std::vector<Person>& people,
    uint64_t base_seed,
    const std::unordered_map<int32_t, std::vector<GeoUnitId>>& catchment_rules) {
  sweepCompletedHops(people);

  if (day < 0 || day >= static_cast<int>(events_by_day_.size())) return;
  const auto& todays_events = events_by_day_[day];
  if (todays_events.empty()) return;

  base_seed_ = base_seed;

  for (const auto& event : todays_events) {
    if (event.candidate_venue_builder &&
        venue_candidates_cache_.find(event.calendar_event_id) ==
            venue_candidates_cache_.end()) {
      venue_candidates_cache_[event.calendar_event_id] =
          event.candidate_venue_builder(world);
    }

    const std::vector<PersonId> attendees =
        attendeesForCatchmentEvent(event, world, catchment_rules);

    for (PersonId pid : attendees) {
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
      ++stats_.triggered;
    }
  }
}

std::pair<VenueId, SubsetIndex> CalendarEventManager::resolveCalendarEventVenue(
    const WorldState& /*world*/, const Person& person,
    int16_t /*activity_idx*/) const {
  auto active_it = active_event_.find(person.id);
  if (active_it == active_event_.end()) return {-1, -1};
  int32_t active_id = active_it->second;

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

const std::vector<CalendarEvent>& CalendarEventManager::eventsForDay(
    int day) const {
  static const std::vector<CalendarEvent> kEmpty;
  if (day < 0 || day >= static_cast<int>(events_by_day_.size())) return kEmpty;
  return events_by_day_[day];
}

}  // namespace june
