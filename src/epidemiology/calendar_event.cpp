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

std::vector<VenueId> CalendarEventManager::buildVenueCandidates(
    const CalendarEvent& event, const WorldState& world) const {
  if (event.candidate_venue_builder) return event.candidate_venue_builder(world);
  if (event.catchment_rule_id >= 0)
    return world.getVenuesInGeoUnit(event.hosting_geo_unit_id,
                                    event.venue_type_name);
  return {};
}

void CalendarEventManager::sweepCompletedHops(
    const std::vector<Person>& people) {
  if (active_event_.empty()) return;
  for (const Person& person : people) {
    if (!person.schedule_hop.isActive())
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

  for (const auto& event : todays_events) {
    // Record trigger seed once per event (first firing wins; stable for multi-day hops).
    event_trigger_seed_.emplace(event.calendar_event_id, base_seed);

    if ((event.candidate_venue_builder || event.catchment_rule_id >= 0) &&
        venue_candidates_cache_.find(event.calendar_event_id) ==
            venue_candidates_cache_.end()) {
      venue_candidates_cache_[event.calendar_event_id] =
          buildVenueCandidates(event, world);
    }

    const std::vector<PersonId> attendees =
        attendeesForCatchmentEvent(event, world, catchment_rules);

    for (PersonId pid : attendees) {
      auto idx_it = world.person_index.find(pid);
      if (idx_it == world.person_index.end()) continue;  // not on this rank
      Person& person = people[idx_it->second];

      // Collision: never override a person already mid-hop.
      if (person.schedule_hop.isActive()) {
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

      // Deferred onset: no advance here; slot 0 runs on the first
      // advanceAndCheckComplete.
      person.schedule_hop = ScheduleHop::begin(
          event.schedule_type_idx, -1, event.duration_days);
      active_event_[person.id] = event.calendar_event_id;
      ++stats_.triggered;
    }
  }
}

std::pair<VenueId, SubsetIndex> CalendarEventManager::resolveCalendarEventVenue(
    const Person& person) const {
  auto active_it = active_event_.find(person.id);
  if (active_it == active_event_.end()) return {-1, -1};
  int32_t active_id = active_it->second;

  auto cache_it = venue_candidates_cache_.find(active_id);
  if (cache_it == venue_candidates_cache_.end() || cache_it->second.empty())
    return {-1, -1};

  const auto& candidates = cache_it->second;
  auto seed_it = event_trigger_seed_.find(active_id);
  if (seed_it == event_trigger_seed_.end()) return {-1, -1};
  uint64_t seed = mix_seed(seed_it->second, static_cast<uint64_t>(person.id),
                           static_cast<uint64_t>(active_id));
  auto ev_it = events_by_id_.find(active_id);
  if (ev_it != events_by_id_.end() && ev_it->second->venue_selector)
    return ev_it->second->venue_selector(candidates, person.id, seed);
  uint64_t h = SplitMix64(seed)();
  return {candidates[h % candidates.size()], 0};
}

CalendarEventManager::Snapshot CalendarEventManager::snapshot_for_checkpoint()
    const {
  return {active_event_, event_trigger_seed_};
}

void CalendarEventManager::restore(Snapshot snapshot, const WorldState& world) {
  active_event_       = std::move(snapshot.active_event);
  event_trigger_seed_ = std::move(snapshot.event_trigger_seed);
  rebuildVenueCachesAfterRestore(world);
}

void CalendarEventManager::rebuildVenueCachesAfterRestore(
    const WorldState& world) {
  for (const auto& [pid, event_id] : active_event_) {
    if (venue_candidates_cache_.count(event_id)) continue;
    auto ev_it = events_by_id_.find(event_id);
    if (ev_it == events_by_id_.end()) continue;
    const CalendarEvent* ev = ev_it->second;
    if (ev->candidate_venue_builder || ev->catchment_rule_id >= 0)
      venue_candidates_cache_[event_id] = buildVenueCandidates(*ev, world);
  }
}

}  // namespace june
