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
    for (const auto& ev : day_events) events_by_id_[ev.calendar_event_id] = &ev;
}

std::vector<PersonId> CalendarEventManager::attendeesForCatchmentEvent(
    const CalendarEvent& event, const WorldState& world,
    const std::unordered_map<int32_t, std::vector<GeoUnitId>>& catchment_rules)
    const {
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
    const std::unordered_map<PersonId, size_t>& person_index,
    const std::vector<Person>& people) {
  if (active_event_.empty()) return;
  for (auto it = active_event_.begin(); it != active_event_.end();) {
    auto idx_it = person_index.find(it->first);
    if (idx_it == person_index.end() ||
        !people[idx_it->second].schedule_hop.isActive())
      it = active_event_.erase(it);
    else
      ++it;
  }
}

void CalendarEventManager::triggerEventsForDay(
    int day, const WorldState& world, std::vector<Person>& people,
    uint64_t base_seed,
    const std::unordered_map<int32_t, std::vector<GeoUnitId>>&
        catchment_rules) {
  sweepCompletedHops(world.person_index, people);

  if (day < 0 || day >= static_cast<int>(events_by_day_.size())) return;
  const auto& todays_events = events_by_day_[day];
  if (todays_events.empty()) return;

  for (const auto& event : todays_events) {
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
      person.schedule_hop = ScheduleHop::begin(event.schedule_type_idx, -1,
                                               event.duration_days - 1);
      active_event_[person.id] = event.calendar_event_id;
      ++stats_.triggered;
    }
  }
}

std::optional<GeoUnitId> CalendarEventManager::getActiveHostingGeoUnit(
    PersonId person_id) const {
  auto active_it = active_event_.find(person_id);
  if (active_it == active_event_.end()) return std::nullopt;
  auto ev_it = events_by_id_.find(active_it->second);
  if (ev_it == events_by_id_.end()) return std::nullopt;
  GeoUnitId gid = ev_it->second->hosting_geo_unit_id;
  if (gid < 0) return std::nullopt;
  return gid;
}

std::vector<GeoUnitId> CalendarEventManager::hostingGeoUnits() const {
  std::vector<GeoUnitId> gs;
  for (const auto& [id, ev] : events_by_id_)
    if (ev->hosting_geo_unit_id >= 0) gs.push_back(ev->hosting_geo_unit_id);
  return gs;
}

CalendarEventManager::Snapshot CalendarEventManager::snapshot_for_checkpoint()
    const {
  return {active_event_};
}

void CalendarEventManager::restore(Snapshot snapshot) {
  active_event_ = std::move(snapshot.active_event);
}

}  // namespace june
