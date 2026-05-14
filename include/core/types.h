#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "../epidemiology/vaccine.h"
#include "variant.h"

namespace june {

// Forward declarations
struct Person;
struct Venue;
struct GeographicalUnit;
struct Subset;
struct ScheduleType;
class Infection;

// Type aliases for IDs
using PersonId = int32_t;
using VenueId = int32_t;

// Bitmask type for activity and venue-type index sets. __uint128_t supports up
// to 128 distinct names, which is well above the 32-name limit of uint32_t.
using ActivityMask = __uint128_t;
using GeoUnitId = int32_t;
using SubsetIndex = int32_t;

// =============================================================================
// PendingInfection - Tracks infections that should be created later
// =============================================================================
struct PendingInfection {
  PersonId person_id;
  PersonId infector_id = -1;
  double infection_time;
  uint8_t venue_type_id;
  uint8_t encounter_type_id = 255;
  VenueId venue_id;
  int32_t home_array_index = -1;  // For lookup on home rank
  uint16_t infector_symptom_id =
      0;  // Symptom ID of the infector at time of transmission
  uint8_t transmission_mode_index = 0;  // Transmission mode index
};

// =============================================================================
// VisitorInfo - Lightweight visitor data for transmission calculations
// =============================================================================
struct VisitorInfo {
  PersonId person_id;
  bool is_infected;
  bool is_infectious;
  float immunity_level;
  int32_t home_array_index = -1;  // For lookup on home rank
  uint16_t symptom_id = 0;
  double time_in_stage = 0.0;

  // Pre-computed integrated infectiousness per mode (from sending rank)
  static constexpr int MAX_MODES = 2;
  double integrated_infectiousness[MAX_MODES] = {};
};

// Special venue ID for infection seed events
constexpr VenueId INFECTION_SEED_VENUE_ID = -999;

// =============================================================================
// GeographicalUnit - Represents a geographical region (SGU, MGU, LGU, etc.)
// =============================================================================
struct GeographicalUnit {
  GeoUnitId id;
  std::string name;
  uint8_t level_id;     // ID into geo_level_names
  GeoUnitId parent_id;  // -1 if root
  float latitude;
  float longitude;

  // Dynamic properties: start index into WorldState::geo_unit_properties
  uint32_t properties_start = 0;
  uint8_t properties_count = 0;

  // Regional risk factors
  float transmission_factor = 1.0f;
  float severity_factor = 1.0f;
};

// =============================================================================
// Immunity - Tracks immunity from past infections or vaccination
// =============================================================================

class VaccineTrajectory;  // Forward declaration

struct Immunity {
  // Natural immunity level (from infection, 0.0 to 1.0)
  double natural_level = 0.0;
  double natural_acquisition_time = -1.0;
  double natural_waning_rate = 0.0;

  // Get natural immunity level at given time
  double getNaturalLevel(double current_time) const {
    if (natural_acquisition_time < 0) return 0.0;
    double time_since = current_time - natural_acquisition_time;
    double waned_level =
        natural_level * std::exp(-natural_waning_rate * time_since);
    return waned_level > 0.0 ? waned_level : 0.0;
  }
};

// =============================================================================
// Schedule Entry - Represents a pre-computed or runtime activity
// =============================================================================
struct ScheduleEntry {
  int16_t activity_index;  // Index into Person::activity_names
  VenueId venue_id;  // Pre-computed venue (-1 if runtime selection needed)
  SubsetIndex
      subset_index;  // Pre-computed subset (-1 if runtime selection needed)
  bool is_deterministic;  // true = pre-computed, false = select at runtime

  ScheduleEntry()
      : activity_index(-1),
        venue_id(-1),
        subset_index(-1),
        is_deterministic(false) {}

  ScheduleEntry(int16_t act_idx, VenueId vid, SubsetIndex sid,
                bool deterministic)
      : activity_index(act_idx),
        venue_id(vid),
        subset_index(sid),
        is_deterministic(deterministic) {}
};

// =============================================================================
// Person - Represents an individual in the simulation
// =============================================================================
enum class Sex : uint8_t { MALE = 0, FEMALE = 1, UNKNOWN = 2 };

struct Person {
  PersonId id;
  float age;
  Sex sex;                // Enum
  GeoUnitId geo_unit_id;  // Where they live (SGU level)

  // Schedule type - determines daily routine pattern
  uint16_t schedule_type_id = 0xFFFF;  // ID into schedule_type_names

  // +Cache schedule type pointer
  const ScheduleType* cached_schedule_type_ = nullptr;

  // Cache which policies can apply to this person (based on selection criteria)
  uint32_t applicable_symptom_policy_mask = 0;
  uint32_t applicable_temporal_policy_mask = 0;

  // Track which policies the person is currently complying/participating in
  // (sticky compliance)
  uint32_t active_symptom_policy_participation = 0;
  uint32_t active_temporal_policy_participation = 0;

  // Track whether a decision (compliance roll) has already been made for a
  // policy This allows both Participation and Refusal to be "sticky"
  uint32_t symptom_policy_decisions = 0;
  uint32_t temporal_policy_decisions = 0;

  // Reset symptom-based policy state (e.g., on recovery)
  void resetPolicyState() {
    active_symptom_policy_participation = 0;
    symptom_policy_decisions = 0;
  }

  // Dynamic properties (ethnicity, comorbidities, work_mode, etc.)
  // Index into WorldState::person_properties flat table
  uint32_t properties_start = 0;
  uint8_t properties_count = 0;

  // Activity list: sparse flat storage to save memory
  // Only stores activities the person actually participates in
  struct ActivityMeta {
    int16_t activity_index;
    uint32_t venue_start;
    uint16_t venue_count;
  };
  uint32_t activity_meta_start = 0;
  uint16_t activity_meta_count = 0;

  // Pre-computed schedule flag (indices live in
  // WorldState::schedule_starts/counts)
  bool schedule_computed = false;

  // Schedule hop state. hopped_schedule_id == -1 means no active hop.
  int16_t hopped_schedule_id = -1;  // index into ScheduleConfig::schedule_types
  int16_t return_schedule_id =
      -1;  // schedule to restore after temp; -1 = original
  int16_t temp_slot_progress = 0;  // next flat_slots index to execute

  // Disease/infection tracking
  std::unique_ptr<Infection> infection;  // nullptr if not infected
  Immunity immunity;                     // Natural immunity tracking
  std::unique_ptr<VaccineTrajectory>
      vaccine_trajectory;  // Vaccine-induced immunity tracking

  // Figure out how susceptible someone is based on both natural and vaccine
  // immunity
  double getSusceptibility(double current_time,
                           const std::string& disease_name) const {
    double natural_suscep = 1.0 - immunity.getNaturalLevel(current_time);
    double vaccine_efficacy = 0.0;
    if (vaccine_trajectory) {
      vaccine_efficacy =
          vaccine_trajectory->getEfficacy(current_time, disease_name, age);
    }
    return natural_suscep * (1.0 - vaccine_efficacy);
  }

  // Death tracking
  bool is_dead = false;
  double death_time = -1.0;  // Simulation time when person died

  // Networks
  // Flat storage ranges: index into WorldState::network_partners and
  // network_meta
  struct NetworkMeta {
    uint16_t network_type_id;
    uint32_t partner_start;
    uint32_t partner_count;
  };
  uint32_t network_meta_start = 0;
  uint16_t network_meta_count = 0;

  uint32_t encounter_meta_start = 0;
  uint16_t encounter_meta_count = 0;
};

// =============================================================================
// Subset - A group of people/roles within a venue (e.g., kids or teachers in a
// classroom)
// =============================================================================
struct Subset {
  VenueId venue_id;
  SubsetIndex subset_index;
  uint16_t subset_type_id;  // ID into Subset::type_names

  // Flattened member storage: index into WorldState::subset_members
  uint32_t member_start = 0;
  uint32_t member_count = 0;
};

// =============================================================================
// Venue - A location where people gather (household, classroom, office, etc.)
// =============================================================================
struct Venue {
  VenueId id;
  uint8_t type_id;        // ID into type_names
  GeoUnitId geo_unit_id;  // Where venue is located
  VenueId
      parent_id;  // -1 if no parent (e.g., school has classrooms as children)
  float latitude;
  float longitude;
  bool is_residence;

  // Dynamic properties: start index into WorldState::venue_properties
  uint32_t properties_start = 0;
  uint16_t properties_count = 0;

  // Subsets: start index into WorldState::subsets
  uint32_t subset_start = 0;
  uint16_t subset_count = 0;

  // Regional risk factors (performance cache)
  float transmission_factor = 1.0f;

  // Fomite transmission state: deposition history per fomite mode.
  // Outer index = local fomite mode index (0, 1, ... matching order of
  //   TransmissionModeType::Fomite entries in TransmissionParams::modes).
  // Each entry records when a batch of material was deposited and how much.
  // Entries older than FomiteConfig::max_age are pruned by
  // updateVenueFomites().
  struct DepositEvent {
    double time;
    double amount;
  };
  std::vector<std::deque<DepositEvent>> fomite_history;
};

// =============================================================================
// ActivityEntry - Raw activity map entry (person -> venue/subset mapping)
// =============================================================================
struct ActivityEntry {
  PersonId person_id;
  int32_t activity_index;  // Index into activity_names
  VenueId venue_id;
  SubsetIndex subset_index;
};

// =============================================================================
// PersonLocation - Tracks where a person is at current simulation time
// =============================================================================
struct PersonLocation {
  PersonId person_id = -1;
  VenueId venue_id = -1;
  SubsetIndex subset_index = -1;
  int16_t activity_index = -1;      // activity_names index
  uint8_t encounter_type_id = 255;  // encounter_type_names index
  size_t person_array_index =
      static_cast<size_t>(-1);  // Direct access to world.people[idx]
};

}  // namespace june
