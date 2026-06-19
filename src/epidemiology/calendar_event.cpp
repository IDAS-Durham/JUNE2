#include "epidemiology/calendar_event.h"

#include <cmath>
#include <random>
#include <stdexcept>

#include "core/world_state.h"
#include "utils/deterministic_rng.h"

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
    : events_by_day_(std::move(events_by_day)) {}

void CalendarEventManager::mergeEvents(
    const std::vector<std::vector<CalendarEvent>>& events_by_day) {
  if (events_by_day.size() > events_by_day_.size())
    events_by_day_.resize(events_by_day.size());
  for (size_t day = 0; day < events_by_day.size(); ++day) {
    auto& dst = events_by_day_[day];
    dst.insert(dst.end(), events_by_day[day].begin(), events_by_day[day].end());
  }
}

const std::vector<PersonId>& CalendarEventManager::attendeesForEvent(
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

void CalendarEventManager::triggerEventsForDay(int day, const WorldState& world,
                                               std::vector<Person>& people,
                                               uint64_t base_seed) {
  if (day < 0 || day >= static_cast<int>(events_by_day_.size())) return;
  const auto& todays_events = events_by_day_[day];
  if (todays_events.empty()) return;

  int cei_field = world.getMembershipFieldIndex(kCalendarEventIdField);
  if (cei_field < 0) {
    throw std::runtime_error(
        std::string("CalendarEventManager: world has no '") +
        kCalendarEventIdField +
        "' membership field, but calendar events are configured");
  }

  for (const auto& event : todays_events) {
    const std::vector<PersonId>& attendees =
        attendeesForEvent(event.calendar_event_id, world, cei_field);
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
      active_event_[person.id] = event.calendar_event_id;
      ++stats_.triggered;
    }
  }
}

std::pair<VenueId, SubsetIndex> CalendarEventManager::resolveCalendarEventVenue(
    const WorldState& world, const Person& person, int16_t activity_idx) const {
  auto active_it = active_event_.find(person.id);
  if (active_it == active_event_.end()) return {-1, -1};
  int32_t active_id = active_it->second;

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

void CalendarEventManager::onHopCompleted(PersonId person_id) {
  active_event_.erase(person_id);
}

const std::vector<CalendarEvent>& CalendarEventManager::eventsForDay(
    int day) const {
  static const std::vector<CalendarEvent> kEmpty;
  if (day < 0 || day >= static_cast<int>(events_by_day_.size())) return kEmpty;
  return events_by_day_[day];
}

}  // namespace june
