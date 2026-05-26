#include "../../include/activity/activity_manager.h"

#include <iomanip>

#include "../../include/epidemiology/policy.h"
#include "../../include/utils/deterministic_rng.h"
#include "../../include/utils/random.h"

namespace june {

ActivityManager::ActivityManager(WorldState& world, const Config& config)
    : world_(world),
      config_(config),
      base_seed_(config.simulation.random_seed) {
  venues_by_id_buffer_.resize(world_.venue_type_names.size());
}

void ActivityManager::setPolicyManager(PolicyManager* policy_manager) {
  policy_manager_ = policy_manager;
}

void ActivityManager::assignScheduleTypes() {
  if (config_.schedule.schedule_types.empty()) {
    std::cerr
        << "ERROR: No schedule types defined! Please use v2 schedule format."
        << std::endl;
    return;
  }

  for (auto& person : world_.people) {
    if (person.is_dead) continue;
    assignScheduleTypeForPerson(person);
  }
}

const ScheduleType* ActivityManager::findScheduleTypeByName(
    const std::string& name) const {
  for (const auto& sched_type : config_.schedule.schedule_types) {
    if (sched_type.name == name) return &sched_type;
  }
  return nullptr;
}

void ActivityManager::assignScheduleTypeForPerson(Person& person) {
  // Skip if person already has a schedule type assigned (e.g., from HDF5)
  if (person.schedule_type_id != 0xFFFF) {
    std::string type_name =
        person.schedule_type_id < world_.schedule_type_names.size()
            ? world_.schedule_type_names[person.schedule_type_id]
            : "unknown";
    // BUG FIX 1: Set cached schedule type pointer correctly when loaded from
    // state
    person.cached_schedule_type_ = findScheduleTypeByName(type_name);
    return;
  }

  // 1. Try CSV-based probabilistic assignment first
  //    Use a per-person seed for reproducible assignment across MPI ranks.
  std::mt19937 csv_rng(mix_seed(base_seed_, person.id, 0xC5A0ULL));
  const ScheduleType* matched_type =
      config_.schedule.tryCSVAssignment(person, world_, csv_rng);

  // 2. Fall back to YAML selection_criteria if CSV returned nothing
  if (!matched_type)
    matched_type = config_.schedule.getScheduleTypeForPerson(person, &world_);

  if (matched_type) {
    // Find ID for the name
    int type_id = world_.getScheduleTypeIndex(matched_type->name);
    if (type_id >= 0) {
      person.schedule_type_id = static_cast<uint16_t>(type_id);
    }
    person.cached_schedule_type_ = matched_type;
  } else {
    // Fallback: use default schedule type
    const std::string& def_type = config_.schedule.default_schedule_type;
    int type_id = world_.getScheduleTypeIndex(def_type);
    if (type_id >= 0) {
      person.schedule_type_id = static_cast<uint16_t>(type_id);
    }
    // Cache the default schedule type pointer
    person.cached_schedule_type_ = findScheduleTypeByName(def_type);
  }
}

void ActivityManager::setDeadLocation(PersonLocation& loc) const {
  loc.venue_id = -1;
  loc.subset_index = -1;
  loc.activity_index = dead_act_idx_;
  loc.encounter_type_id = 255;
}

bool ActivityManager::applyPolicyOverride(PersonLocation& loc, Person& person,
                                          int16_t activity, VenueId venue,
                                          SubsetIndex subset,
                                          int time_slot_index) {
  if (policy_manager_ == nullptr) return false;
  auto override = policy_manager_->getOverride(
      person, activity, venue, subset, current_simulation_time_,
      time_slot_index);
  if (!override.has_value()) return false;
  loc = override.value();
  return true;
}

void ActivityManager::setResidenceOrNoneLocation(PersonLocation& loc,
                                                 const Person& person) {
  auto residence = world_.getActivityVenues(person, residence_act_idx_);
  if (!residence.empty()) {
    auto [venue_id, subset_idx] = residence[0];
    loc.venue_id = venue_id;
    loc.subset_index = subset_idx;
    loc.activity_index = residence_act_idx_;
  } else {
    loc.venue_id = -1;
    loc.subset_index = -1;
    loc.activity_index = none_act_idx_;
  }
  loc.encounter_type_id = 255;
}

void ActivityManager::ensureIndicesCached() {
  if (dead_act_idx_ == -1) {
    dead_act_idx_ = static_cast<int16_t>(world_.getActivityIndex("dead"));
    residence_act_idx_ =
        static_cast<int16_t>(world_.getActivityIndex("residence"));
    none_act_idx_ = static_cast<int16_t>(world_.getActivityIndex("none"));
    no_venue_act_idx_ =
        static_cast<int16_t>(world_.getActivityIndex("no_venue"));
  }
}

void ActivityManager::initializeLocations(
    std::vector<PersonLocation>& locations) {
  ensureIndicesCached();
  locations.resize(world_.people.size());

  for (size_t i = 0; i < world_.people.size(); ++i) {
    const Person& p = world_.people[i];
    locations[i].person_id = p.id;

    // Skip dead people - they don't have a location
    if (p.is_dead) {
      setDeadLocation(locations[i]);
      continue;
    }

    // Find their residence (using cached index)
    setResidenceOrNoneLocation(locations[i], p);
    locations[i].person_array_index = i;
  }
}

bool ActivityManager::advanceHoppedSchedule(Person& person, PersonLocation& loc,
                                            size_t person_array_idx,
                                            int day_type_idx) {
  const ScheduleType& hopped =
      config_.schedule.schedule_types[person.hopped_schedule_id];
  if (!hopped.is_temporary ||
      person.temp_slot_progress >=
          static_cast<int16_t>(hopped.flat_slots.size())) {
    return false;
  }

  const TimeSlot& slot = hopped.flat_slots[person.temp_slot_progress];
  uint64_t hop_key = mix_seed(base_seed_, person.id,
                              static_cast<uint64_t>(person.temp_slot_progress));
  int16_t act = selectActivity(person, slot, person.temp_slot_progress, &hopped,
                               day_type_idx, hop_key);
  auto [v, s] = selectVenue(person, act, slot, hop_key);
  loc.venue_id = v;
  loc.subset_index = s;
  loc.activity_index = act;
  loc.encounter_type_id = 255;
  loc.person_id = person.id;
  loc.person_array_index = person_array_idx;
  person.temp_slot_progress++;

  // If all flat_slots exhausted, return to original (or specified) schedule
  if (person.temp_slot_progress >=
      static_cast<int16_t>(hopped.flat_slots.size())) {
    int16_t return_to = (person.return_schedule_id != -1)
                            ? person.return_schedule_id
                            : static_cast<int16_t>(person.schedule_type_id);
    person.cached_schedule_type_ = &config_.schedule.schedule_types[return_to];
    person.hopped_schedule_id = -1;
    person.return_schedule_id = -1;
    person.temp_slot_progress = 0;
  }
  return true;
}

void ActivityManager::assignActivities(const TimeSlot& slot, int day_type_idx,
                                       std::vector<PersonLocation>& locations) {
  ensureIndicesCached();
  // Assign each person to an activity
  for (size_t i = 0; i < world_.people.size(); ++i) {
    const Person& person = world_.people[i];

    // Hopped person: bypass normal slot
    if (person.hopped_schedule_id != -1) {
      assignHoppedSingleSlot(person, i, slot, day_type_idx, locations);
      continue;
    }

    // Skip dead people - they don't move
    if (person.is_dead) {
      setDeadLocation(locations[i]);
      continue;
    }

    assignSingleSlotForLivePerson(person, i, slot, day_type_idx, locations);
  }
}

void ActivityManager::assignSingleSlotForLivePerson(
    const Person& person, size_t person_array_idx, const TimeSlot& slot,
    int day_type_idx, std::vector<PersonLocation>& locations) {
  const size_t i = person_array_idx;
  // Get person's schedule type
  const ScheduleType* schedule_type = person.cached_schedule_type_;
  if (!schedule_type) {
    schedule_type = config_.schedule.getScheduleTypeForPerson(person);
    const_cast<Person&>(person).cached_schedule_type_ = schedule_type;
  }

  if (!schedule_type) {
    std::cerr << "ERROR: No schedule type for person " << person.id
              << std::endl;
    return;
  }

  // Select activity for this person (returns int16_t index)
  uint64_t time_key = static_cast<uint64_t>(current_simulation_time_ * 1000);
  int16_t scheduled_activity_index =
      selectActivity(person, slot, -1, schedule_type, day_type_idx, time_key);

  // Select specific venue for that activity
  auto [venue_id, subset_idx] =
      selectVenue(person, scheduled_activity_index, slot, time_key);

  // Update location
  locations[i].venue_id = venue_id;
  locations[i].subset_index = subset_idx;
  locations[i].activity_index = scheduled_activity_index;
  locations[i].encounter_type_id = 255;  // Default for normal activities
  locations[i].person_id = person.id;
  locations[i].person_array_index = i;

  // Check for policy overrides (symptom-based, lockdowns, etc.)
  if (applyPolicyOverride(locations[i], const_cast<Person&>(person),
                          scheduled_activity_index, locations[i].venue_id,
                          locations[i].subset_index, -1)) {
    locations[i].person_id = person.id;
    locations[i].person_array_index = i;
  }
}

int16_t ActivityManager::selectActivity(const Person& person,
                                        const TimeSlot& slot, size_t slot_idx,
                                        const ScheduleType* schedule_type,
                                        int day_type_idx, uint64_t time_key) {
  static thread_local std::vector<int16_t> available_indices;
  filterAvailableActivities(person, slot, available_indices);

  if (available_indices.empty()) {
    return residence_act_idx_;  // Fallback
  }

  // Per-person deterministic RNG for MPI reproducibility
  SplitMix64 rng(mix_seed(base_seed_, person.id, time_key));

  // Try each activity in order based on participation probability (using
  // pre-resolved vectors)
  const std::vector<double>* participation_ptr = nullptr;
  if (day_type_idx >= 0 &&
      day_type_idx < static_cast<int>(
                         schedule_type->participation_by_day_type_id.size())) {
    participation_ptr =
        &schedule_type->participation_by_day_type_id[day_type_idx];
  }
  static const std::vector<double> empty_participation;
  const auto& participation_by_id =
      participation_ptr ? *participation_ptr : empty_participation;

  maybeRollLinkedActivitiesDay(person, slot, *schedule_type,
                               participation_by_id);

  return pickActivityByRate(person, *schedule_type, participation_by_id,
                            available_indices, rng);
}

std::pair<VenueId, SubsetIndex> ActivityManager::selectVenue(
    const Person& person, int16_t activity_idx, const TimeSlot& slot,
    uint64_t time_key) {
  if (activity_idx == no_venue_act_idx_) {
    return {-1, -1};
  }
  auto venues = world_.getActivityVenues(person, activity_idx);
  if (venues.empty()) {
    return {-1, -1};
  }

  // Check if this time slot specifies a particular activity and index
  if (auto specified = tryPickSpecifiedVenue(slot, activity_idx, venues)) {
    return *specified;
  }

  // Per-person deterministic RNG for MPI reproducibility
  SplitMix64 rng(mix_seed(base_seed_, person.id, activity_idx, time_key));

  // Default: Hierarchical Selection (Category -> Venue)
  // 1. Group available venues by their category (type ID)
  groupVenuesByType(venues);

  if (venue_type_ids_buffer_.empty()) {
    // No local venues found – fall back to cross-rank venues if any
    if (!cross_rank_venues_buffer_.empty()) {
      std::uniform_int_distribution<size_t> dist(
          0, cross_rank_venues_buffer_.size() - 1);
      return cross_rank_venues_buffer_[dist(rng)];
    }
    return {-1, -1};
  }

  // 2. Select a category weighted by activity preferences
  uint8_t chosen_type_id = pickWeightedVenueType(person, activity_idx, rng);

  // 3. Select a specific venue within that category
  const auto& filtered_venues = venues_by_id_buffer_[chosen_type_id];
  std::uniform_int_distribution<size_t> venue_dist(0,
                                                   filtered_venues.size() - 1);
  return filtered_venues[venue_dist(rng)];
}

void ActivityManager::precomputeSchedules() {
  if (!config_.performance.precompute_schedules) {
    std::cout << "Schedule pre-computation disabled by configuration"
              << std::endl;
    return;
  }

  int num_day_types = static_cast<int>(config_.schedule.day_type_names.size());
  world_.num_day_types = static_cast<size_t>(num_day_types);
  world_.precomputed_schedules.resize(num_day_types);
  world_.schedule_starts.assign(world_.people.size() * num_day_types, 0);
  world_.schedule_counts.assign(world_.people.size() * num_day_types, 0);

  for (size_t person_idx = 0; person_idx < world_.people.size(); ++person_idx) {
    auto& person = world_.people[person_idx];
    if (person.is_dead) continue;

    // Use cached schedule type pointer if available
    const ScheduleType* schedule_type = person.cached_schedule_type_;
    if (!schedule_type) {
      schedule_type = config_.schedule.getScheduleTypeForPerson(person);
      person.cached_schedule_type_ = schedule_type;
    }
    if (!schedule_type) {
      std::cerr << "ERROR: No schedule type found for person " << person.id
                << std::endl;
      continue;
    }

    for (int dt_idx = 0; dt_idx < num_day_types; ++dt_idx) {
      precomputePersonDayType(person, person_idx, dt_idx, num_day_types,
                              schedule_type);
    }

    person.schedule_computed = true;
  }
}

void ActivityManager::precomputePersonDayType(
    Person& person, size_t person_idx, int dt_idx, int num_day_types,
    const ScheduleType* schedule_type) {
  const std::vector<TimeSlot>* slots =
      (dt_idx < static_cast<int>(schedule_type->slots_by_day_type_idx.size()))
          ? schedule_type->slots_by_day_type_idx[dt_idx]
          : nullptr;
  if (!slots) return;

  auto& dt_schedules = world_.precomputed_schedules[dt_idx];
  uint32_t start = static_cast<uint32_t>(dt_schedules.size());
  world_.schedule_starts[person_idx * num_day_types + dt_idx] = start;
  world_.schedule_counts[person_idx * num_day_types + dt_idx] =
      static_cast<uint16_t>(slots->size());

  for (size_t slot_idx = 0; slot_idx < slots->size(); ++slot_idx) {
    const auto& slot = (*slots)[slot_idx];
    uint64_t precomp_key = mix_seed(0xBE00ULL, slot_idx, dt_idx);
    precomputeOneSlot(person, slot, slot_idx, dt_idx, schedule_type,
                      precomp_key, dt_schedules);
  }
}

void ActivityManager::assignActivitiesFromSchedule(
    int time_slot_index, int day_type_idx,
    std::vector<PersonLocation>& locations) {
  ensureIndicesCached();

  // Compute time_key for per-person deterministic RNG
  uint64_t time_key =
      mix_seed(static_cast<uint64_t>(current_simulation_time_ * 1000),
               time_slot_index, static_cast<uint64_t>(day_type_idx));

  for (size_t i = 0; i < world_.people.size(); ++i) {
    Person& person = world_.people[i];  // Non-const for potential modifications

    // Hopped person: bypass precomputed schedule
    if (person.hopped_schedule_id != -1) {
      assignHoppedScheduleSlot(person, i, time_slot_index, day_type_idx,
                               time_key, locations);
      continue;
    }

    // Skip dead people
    if (person.is_dead) {
      setDeadLocation(locations[i]);
      locations[i].person_id = person.id;
      continue;
    }

    // Fallback to residence if schedule not computed
    if (!person.schedule_computed) {
      setResidenceOrNoneLocation(locations[i], person);
      locations[i].person_id = person.id;
      continue;
    }

    assignFromPrecomputedSchedule(i, person, time_slot_index, day_type_idx,
                                  time_key, locations);
  }
}

void ActivityManager::assignFromPrecomputedSchedule(
    size_t person_array_idx, Person& person, int time_slot_index,
    int day_type_idx, uint64_t time_key,
    std::vector<PersonLocation>& locations) {
  const size_t i = person_array_idx;
  auto schedule = world_.getSchedule(person, day_type_idx);

  if (time_slot_index >= 0 &&
      time_slot_index < static_cast<int>(schedule.size())) {
    const ScheduleEntry& entry = schedule[time_slot_index];

    // Get the current time slot definition for specified_activity
    const ScheduleType* schedule_type = person.cached_schedule_type_;
    const TimeSlot* current_slot =
        lookupCurrentSlot(schedule_type, day_type_idx, time_slot_index);

    // Determine scheduled activity
    VenueId scheduled_venue_id = entry.venue_id;
    SubsetIndex scheduled_subset_idx = entry.subset_index;
    int16_t scheduled_activity_index = entry.activity_index;

    if (!entry.is_deterministic) {
      const ScheduleType* sched_type = schedule_type;
      resolveStochasticEntry(person, entry, time_slot_index, day_type_idx,
                             time_key, sched_type, current_slot,
                             scheduled_activity_index, scheduled_venue_id,
                             scheduled_subset_idx);
    }

    // Check for schedule hop trigger
    maybeTriggerScheduleHop(person, current_slot, day_type_idx, time_key,
                            scheduled_activity_index, scheduled_venue_id,
                            scheduled_subset_idx);

    // Check for policy overrides (symptom-based, lockdowns, etc.)
    if (applyPolicyOverride(locations[i], person, scheduled_activity_index,
                            scheduled_venue_id, scheduled_subset_idx,
                            time_slot_index)) {
      locations[i].person_id = person.id;
      locations[i].person_array_index = i;
      return;
    }

    // SUCCESS: Assign pre-computed or runtime-selected activity/venue
    locations[i].venue_id = scheduled_venue_id;
    locations[i].subset_index = scheduled_subset_idx;
    locations[i].activity_index = scheduled_activity_index;
    locations[i].encounter_type_id = 255;

  } else {
    // Fallback to residence if slot index invalid
    setResidenceOrNoneLocation(locations[i], person);
  }

  locations[i].person_id = person.id;
  locations[i].person_array_index = i;
}

void ActivityManager::resolveStochasticEntry(
    Person& person, const ScheduleEntry& entry, int time_slot_index,
    int day_type_idx, uint64_t time_key,
    const ScheduleType*& sched_type, const TimeSlot*& current_slot,
    int16_t& scheduled_activity_index, VenueId& scheduled_venue_id,
    SubsetIndex& scheduled_subset_idx) {
  // Check if this is a hybrid activity using bitmask (no string
  // comparison). Also honour the per-schedule force_hybrid_mask so the
  // cached venue is reused on participation pass.
  bool is_hybrid_entry =
      (entry.venue_id != -1) &&
      (config_.performance.isHybridIdx(scheduled_activity_index) ||
       (sched_type && scheduled_activity_index >= 0 &&
        (sched_type->force_hybrid_mask &
         (ActivityMask(1) << scheduled_activity_index)) != 0));

  // Ensure we have valid slot/schedule_type pointers
  if (!sched_type) {
    sched_type = config_.schedule.getScheduleTypeForPerson(person);
    person.cached_schedule_type_ = sched_type;
  }
  if (!current_slot) {
    current_slot =
        lookupCurrentSlot(sched_type, day_type_idx, time_slot_index);
  }

  if (is_hybrid_entry) {
    // HYBRID: Re-evaluate participation, use precomputed venue if passed
    int16_t runtime_activity_idx =
        selectActivity(person, *current_slot, time_slot_index, sched_type,
                       day_type_idx, time_key);

    if (runtime_activity_idx == scheduled_activity_index) {
      // Passed participation! Use precomputed venue
      scheduled_venue_id = entry.venue_id;
      scheduled_subset_idx = entry.subset_index;
      scheduled_activity_index = entry.activity_index;
    } else {
      // Failed participation or chose different activity
      auto [venue_id, subset_idx] =
          selectVenue(person, runtime_activity_idx, *current_slot, time_key);
      scheduled_venue_id = venue_id;
      scheduled_subset_idx = subset_idx;
      scheduled_activity_index = runtime_activity_idx;
    }
  } else {
    // FULLY STOCHASTIC: select both activity and venue at runtime
    int16_t runtime_activity_idx =
        selectActivity(person, *current_slot, time_slot_index, sched_type,
                       day_type_idx, time_key);
    auto [venue_id, subset_idx] =
        selectVenue(person, runtime_activity_idx, *current_slot, time_key);
    scheduled_venue_id = venue_id;
    scheduled_subset_idx = subset_idx;
    scheduled_activity_index = runtime_activity_idx;
  }
}

void ActivityManager::maybeTriggerScheduleHop(
    Person& person, const TimeSlot* current_slot, int day_type_idx,
    uint64_t time_key, int16_t& scheduled_activity_index,
    VenueId& scheduled_venue_id, SubsetIndex& scheduled_subset_idx) {
  int16_t hop_idx = -1;
  if (current_slot && scheduled_activity_index >= 0 &&
      scheduled_activity_index <
          static_cast<int16_t>(
              current_slot->hop_schedule_by_activity_idx.size())) {
    hop_idx =
        current_slot->hop_schedule_by_activity_idx[scheduled_activity_index];
  }
  // Generic property-dispatched hop: activity, property name, and schedule
  // name template are all specified in YAML hop_on_activity; no activity
  // names or property names are hard-coded here.
  if (hop_idx == -1 && current_slot) {
    hop_idx = resolvePropertyDispatchedHopIdx(person, *current_slot,
                                              scheduled_activity_index);
  }
  if (hop_idx == -1) return;

  const ScheduleType& target = config_.schedule.schedule_types[hop_idx];
  if (target.is_temporary && !target.flat_slots.empty()) {
    person.hopped_schedule_id = hop_idx;
    person.return_schedule_id =
        (target.return_schedule_idx != -1)
            ? target.return_schedule_idx
            : static_cast<int16_t>(person.schedule_type_id);
    person.temp_slot_progress = 0;
    // Immediate: slot N of old = slot 0 of new schedule
    const TimeSlot& slot0 = target.flat_slots[0];
    uint64_t hop_key = mix_seed(base_seed_, person.id, time_key);

    int16_t new_act =
        selectActivity(person, slot0, 0, &target, day_type_idx, hop_key);

    auto [v, s] = selectVenue(person, new_act, slot0, hop_key);
    scheduled_activity_index = new_act;
    scheduled_venue_id = v;
    scheduled_subset_idx = s;
    person.temp_slot_progress = 1;
  } else {
    // Permanent hop: update schedule pointer, no auto-return
    person.hopped_schedule_id = hop_idx;
    person.return_schedule_id = -1;
    person.cached_schedule_type_ = &config_.schedule.schedule_types[hop_idx];
  }
}

void ActivityManager::groupVenuesByType(
    std::span<const std::pair<VenueId, SubsetIndex>> venues) {
  for (auto& buffer : venues_by_id_buffer_) buffer.clear();
  venue_type_ids_buffer_.clear();
  cross_rank_venues_buffer_.clear();

  for (const auto& v_entry : venues) {
    // Use getVenueTypeId which works for both local and cross-rank venues
    // (falls back to global_venue_type_map for cross-rank). This ensures
    // the same hierarchical selection regardless of MPI partitioning.
    uint8_t v_type_id = world_.getVenueTypeId(v_entry.first);
    if (v_type_id == 255) {
      // Unknown venue type — collect for fallback
      cross_rank_venues_buffer_.push_back(v_entry);
      continue;
    }

    if (v_type_id < venues_by_id_buffer_.size()) {
      if (venues_by_id_buffer_[v_type_id].empty()) {
        venue_type_ids_buffer_.push_back(v_type_id);
      }
      venues_by_id_buffer_[v_type_id].push_back(v_entry);
    }
  }
}

uint8_t ActivityManager::pickWeightedVenueType(const Person& person,
                                               int16_t activity_idx,
                                               SplitMix64& rng) {
  weights_buffer_.clear();
  for (uint8_t type_id : venue_type_ids_buffer_) {
    weights_buffer_.push_back(config_.activity_preferences.getWeight(
        person, activity_idx, type_id, &world_));
    stats_.weights_cached++;
  }

  // Build cumulative weights and sample (replaces std::discrete_distribution
  // — same complexity, no per-call allocation).
  double total_w = buildCumulative(weights_buffer_, cumulative_buffer_);
  size_t chosen_idx = 0;
  if (total_w > 0.0) {
    int s = sampleFromCumulative(cumulative_buffer_, rng);
    if (s >= 0) chosen_idx = static_cast<size_t>(s);
  }
  return venue_type_ids_buffer_[chosen_idx];
}

std::optional<std::pair<VenueId, SubsetIndex>>
ActivityManager::tryPickSpecifiedVenue(
    const TimeSlot& slot, int16_t activity_idx,
    std::span<const std::pair<VenueId, SubsetIndex>> venues) const {
  if (!slot.specified_activity.has_value()) return std::nullopt;
  const auto& spec_act = slot.specified_activity.value();
  if (spec_act.cached_activity_idx != activity_idx) return std::nullopt;

  std::vector<std::pair<VenueId, SubsetIndex>> filtered_venues;
  if (spec_act.cached_venue_type_idx >= 0) {
    // Filter by pre-resolved venue type ID
    for (const auto& [venue_id, subset_idx] : venues) {
      uint8_t v_type_id = world_.getVenueTypeId(venue_id);
      if (v_type_id == spec_act.cached_venue_type_idx) {
        filtered_venues.push_back({venue_id, subset_idx});
      }
    }
  } else {
    filtered_venues.assign(venues.begin(), venues.end());
  }

  if (filtered_venues.empty()) return std::nullopt;
  if (spec_act.index >= 0 &&
      spec_act.index < static_cast<int>(filtered_venues.size())) {
    return filtered_venues[spec_act.index];
  }
  return filtered_venues.back();
}

int16_t ActivityManager::pickActivityByRate(
    const Person& person, const ScheduleType& schedule_type,
    const std::vector<double>& participation_by_id,
    const std::vector<int16_t>& available_indices, SplitMix64& rng) const {
  for (int16_t act_idx : available_indices) {
    if (act_idx < 0) continue;

    // Linked activity: outcome already decided for today.
    if (schedule_type.linked_activities_mask != 0 &&
        (schedule_type.linked_activities_mask & (ActivityMask(1) << act_idx)) !=
            0) {
      if (person.linked_activities_pass) {
        return act_idx;  // attending today — pick this activity if available
      }
      continue;  // skipping today — fall through without consuming rng
    }

    if (act_idx < static_cast<int16_t>(participation_by_id.size())) {
      double rate = participation_by_id[act_idx];
      if (rate > 0.0) {
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        if (dist(rng) < rate) {
          return act_idx;  // Participate in this activity
        }
      }
    }
  }

  // Default to last available activity (usually residence)
  return available_indices.back();
}

void ActivityManager::precomputeOneSlot(
    Person& person, const TimeSlot& slot, size_t slot_idx, int dt_idx,
    const ScheduleType* schedule_type, uint64_t precomp_key,
    std::vector<ScheduleEntry>& dt_schedules) {
  int16_t act_idx = selectActivity(person, slot, slot_idx, schedule_type,
                                   dt_idx, precomp_key);

  bool is_det = config_.performance.isDeterministicIdx(act_idx, &slot);
  bool is_hyb = config_.performance.isHybridIdx(act_idx);

  // Per-schedule override: demote DETERMINISTIC -> HYBRID for activities
  // listed in this schedule type's force_hybrid_activities.
  if (is_det && act_idx >= 0 &&
      (schedule_type->force_hybrid_mask & (ActivityMask(1) << act_idx)) != 0) {
    is_det = false;
    is_hyb = true;
  }
  // Linked-activities coupling: if this slot's allowed activities
  // include any linked activity, force the entry to be re-rolled at
  // runtime regardless of which outcome precompute chose. Otherwise
  // the "skip" outcome (e.g. residence) would be stored
  // deterministically and the person would never re-roll on later
  // days, freezing their decision.
  if (is_det && schedule_type->linked_activities_mask != 0 &&
      (slot.allowed_activity_mask & schedule_type->linked_activities_mask) !=
          0) {
    is_det = false;
    is_hyb = true;
  }

  if (is_det) {
    auto [venue_id, subset_idx] =
        selectVenue(person, act_idx, slot, precomp_key);
    dt_schedules.emplace_back(act_idx, venue_id, subset_idx, true);
  } else if (is_hyb) {
    auto [venue_id, subset_idx] =
        selectVenue(person, act_idx, slot, precomp_key);
    dt_schedules.emplace_back(act_idx, venue_id, subset_idx, false);
  } else {
    dt_schedules.emplace_back(act_idx, -1, -1, false);
  }
}

void ActivityManager::filterAvailableActivities(
    const Person& person, const TimeSlot& slot,
    std::vector<int16_t>& available) const {
  available.clear();
  for (int16_t act_idx : slot.allowed_activity_indices) {
    if (act_idx == no_venue_act_idx_ ||
        slot.property_hop_dispatch_by_activity_idx.count(act_idx)) {
      available.push_back(act_idx);
      continue;
    }
    bool has_venues = !world_.getActivityVenues(person, act_idx).empty();
    if (has_venues) {
      available.push_back(act_idx);
    }
  }
}

// Linked activities: if the schedule type opts in via `linked_activities`
// AND this slot's allowed activities touch the linked set, roll ONCE per
// (person, sim_day) and reuse the cached decision across every listed
// activity. Couples e.g. an outbound route, a primary activity at the
// destination, and a return route into a single per-day attendance
// decision. Slots that don't touch linked activities (e.g. weekend slots
// or non-routing slots) are unaffected. Fully generic — driven by YAML,
// no hardcoded activity names.
void ActivityManager::maybeRollLinkedActivitiesDay(
    const Person& person, const TimeSlot& slot,
    const ScheduleType& schedule_type,
    const std::vector<double>& participation_by_id) {
  const bool slot_touches_linked =
      schedule_type.linked_activities_mask != 0 &&
      (slot.allowed_activity_mask & schedule_type.linked_activities_mask) != 0;
  if (!slot_touches_linked) return;
  int day_now = static_cast<int>(current_simulation_time_);
  if (person.linked_activities_day == day_now) return;
  // Use the rate of any linked activity from the participation table.
  // We deliberately do NOT filter by available_indices — the rate is a
  // schedule-level configuration that shouldn't depend on whether THIS
  // person happens to have the venue (e.g. a person without a route
  // leg should still re-roll the same attendance decision used by the
  // primary-activity slot later in the day). All linked activities
  // should be configured with the same rate.
  double linked_rate = 0.0;
  for (size_t a = 0; a < participation_by_id.size(); ++a) {
    if ((schedule_type.linked_activities_mask & (ActivityMask(1) << a)) != 0) {
      linked_rate = participation_by_id[a];
      break;
    }
  }
  SplitMix64 day_rng(
      mix_seed(base_seed_, person.id, mix_seed(0xDA17ULL, day_now, 0ULL)));
  std::uniform_real_distribution<double> day_dist(0.0, 1.0);
  bool pass = day_dist(day_rng) < linked_rate;
  const_cast<Person&>(person).linked_activities_day = day_now;
  const_cast<Person&>(person).linked_activities_pass = pass;
}

int16_t ActivityManager::resolvePropertyDispatchedHopIdx(
    const Person& person, const TimeSlot& slot, int16_t activity_idx) const {
  auto dispatch_it =
      slot.property_hop_dispatch_by_activity_idx.find(activity_idx);
  if (dispatch_it == slot.property_hop_dispatch_by_activity_idx.end()) {
    return -1;
  }
  const auto& dispatch = dispatch_it->second;
  auto prop = world_.getPersonProperty(person, dispatch.property_name);
  if (!prop) return -1;
  int32_t value = getOr<int32_t>(*prop, 0);
  if (value <= 0) return -1;
  std::string sched_name = dispatch.schedule_name_template;
  const std::string placeholder = "{value}";
  size_t pos = sched_name.find(placeholder);
  if (pos != std::string::npos) {
    sched_name.replace(pos, placeholder.size(), std::to_string(value));
  }
  return static_cast<int16_t>(world_.getScheduleTypeIndex(sched_name));
}

const TimeSlot* ActivityManager::lookupCurrentSlot(
    const ScheduleType* schedule_type, int day_type_idx,
    int time_slot_index) const {
  if (!schedule_type) return nullptr;
  if (day_type_idx >=
      static_cast<int>(schedule_type->slots_by_day_type_idx.size())) {
    return nullptr;
  }
  if (schedule_type->slots_by_day_type_idx[day_type_idx] == nullptr) {
    return nullptr;
  }
  const auto& slots = *schedule_type->slots_by_day_type_idx[day_type_idx];
  if (time_slot_index < 0 ||
      time_slot_index >= static_cast<int>(slots.size())) {
    return nullptr;
  }
  return &slots[time_slot_index];
}

void ActivityManager::assignHoppedSingleSlot(
    const Person& person, size_t person_array_idx, const TimeSlot& slot,
    int day_type_idx, std::vector<PersonLocation>& locations) {
  const size_t i = person_array_idx;
  Person& mutable_person = const_cast<Person&>(person);
  const ScheduleType& hopped_sched =
      config_.schedule.schedule_types[person.hopped_schedule_id];
  uint64_t time_key_hop =
      static_cast<uint64_t>(current_simulation_time_ * 1000);

  if (hopped_sched.is_temporary) {
    advanceHoppedSchedule(mutable_person, locations[i], i, day_type_idx);
  } else {
    // Non-temporary hop: execute normal day-type slots
    if (day_type_idx <
            static_cast<int>(hopped_sched.slots_by_day_type_idx.size()) &&
        hopped_sched.slots_by_day_type_idx[day_type_idx] != nullptr) {
      // assignActivities is called with a single slot; use it directly
      int16_t act = selectActivity(person, slot, 0, &hopped_sched,
                                   day_type_idx, time_key_hop);
      auto [v, s] = selectVenue(person, act, slot, time_key_hop);
      locations[i].venue_id = v;
      locations[i].subset_index = s;
      locations[i].activity_index = act;
      locations[i].encounter_type_id = 255;
    }
  }

  if (policy_manager_ != nullptr) {
    VenueId effective_venue = locations[i].venue_id;
    SubsetIndex effective_subset = locations[i].subset_index;
    if (effective_venue < 0 && hopped_sched.is_temporary) {
      auto [lv, ls] = findLastNonNullVenueOnHop(mutable_person);
      effective_venue = lv;
      effective_subset = ls;
    }
    applyPolicyOverride(locations[i], mutable_person,
                        locations[i].activity_index, effective_venue,
                        effective_subset, -1);
  }

  locations[i].person_id = person.id;
  locations[i].person_array_index = i;
}

void ActivityManager::assignHoppedScheduleSlot(
    Person& person, size_t person_array_idx, int time_slot_index,
    int day_type_idx, uint64_t time_key,
    std::vector<PersonLocation>& locations) {
  const size_t i = person_array_idx;
  const ScheduleType& hopped_sched =
      config_.schedule.schedule_types[person.hopped_schedule_id];

  if (hopped_sched.is_temporary) {
    advanceHoppedSchedule(person, locations[i], i, day_type_idx);
  } else {
    // Non-temporary hop (e.g. freeze_in_place): execute normal day-type
    // slots
    if (day_type_idx <
            static_cast<int>(hopped_sched.slots_by_day_type_idx.size()) &&
        hopped_sched.slots_by_day_type_idx[day_type_idx] != nullptr) {
      const auto& slots = *hopped_sched.slots_by_day_type_idx[day_type_idx];
      if (time_slot_index >= 0 &&
          time_slot_index < static_cast<int>(slots.size())) {
        const TimeSlot& hop_slot = slots[time_slot_index];
        int16_t act = selectActivity(person, hop_slot, time_slot_index,
                                     &hopped_sched, day_type_idx, time_key);
        auto [v, s] = selectVenue(person, act, hop_slot, time_key);
        locations[i].venue_id = v;
        locations[i].subset_index = s;
        locations[i].activity_index = act;
        locations[i].encounter_type_id = 255;
      }
    }
  }

  // Apply policy overrides (e.g. sick traveller freeze / unfreeze)
  if (policy_manager_ != nullptr) {
    VenueId effective_venue = locations[i].venue_id;
    SubsetIndex effective_subset = locations[i].subset_index;
    // If in transit (no_venue), resolve last real overnight venue so the
    // policy can pin the person there instead of at home
    if (effective_venue < 0 && hopped_sched.is_temporary) {
      auto [lv, ls] = findLastNonNullVenueOnHop(person);
      effective_venue = lv;
      effective_subset = ls;
    }
    applyPolicyOverride(locations[i], person, locations[i].activity_index,
                        effective_venue, effective_subset, time_slot_index);
  }

  locations[i].person_id = person.id;
  locations[i].person_array_index = i;
}

std::pair<VenueId, SubsetIndex> ActivityManager::findLastNonNullVenueOnHop(
    const Person& person) {
  ensureIndicesCached();
  // temp_slot_progress was already incremented by advanceHoppedSchedule, so
  // the last executed slot was (temp_slot_progress - 2) after the increment,
  // since the current call incremented it to temp_slot_progress.
  // Scan from (temp_slot_progress - 2) downward for the last real venue.
  const ScheduleType& hopped =
      config_.schedule.schedule_types[person.hopped_schedule_id];
  for (int16_t s = person.temp_slot_progress - 2; s >= 0; --s) {
    const TimeSlot& prev_slot = hopped.flat_slots[s];
    uint64_t hop_key =
        mix_seed(base_seed_, person.id, static_cast<uint64_t>(s));
    int16_t prev_act =
        selectActivity(person, prev_slot, s, &hopped, -1, hop_key);
    auto [v, sub] = selectVenue(person, prev_act, prev_slot, hop_key);
    if (v >= 0) return {v, sub};
  }
  // No prior overnight venue — fall back to home residence
  auto home = world_.getActivityVenues(person, residence_act_idx_);
  if (!home.empty()) return home[0];
  return {-1, -1};
}

}  // namespace june
