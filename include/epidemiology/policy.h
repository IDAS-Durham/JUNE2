#pragma once

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include "../core/config.h"
#include "../core/types.h"
#include "../core/world_state.h"
#include "../utils/deterministic_rng.h"
#include "../utils/random.h"

namespace june {

// Forward declarations
class Disease;

// =============================================================================
// Activity Exemption - Specific conditions to exempt an activity
// =============================================================================

struct ActivityExemption {
  std::string activity_name;
  int16_t activity_index = -1;
  std::vector<SelectionCriterion> criteria;

  bool appliesTo(const Person& person, const WorldState* world = nullptr,
                 const Person* partner = nullptr) const {
    for (const auto& criterion : criteria) {
      if (!criterion.evaluate(person, world, partner)) {
        return false;
      }
    }
    return true;
  }

  void resolve(const WorldState& world) {
    activity_index =
        static_cast<int16_t>(world.getActivityIndex(activity_name));
    for (auto& criterion : criteria) {
      criterion.resolve(world);
    }
  }
};

struct PolicyAction {
  // Activities to override (empty = override all activities with "*")
  std::unordered_set<std::string> override_activities;
  uint64_t override_activity_mask = 0;  // BITMASK: support up to 64 activities
  bool override_all = false;

  // Generic exemptions
  std::vector<ActivityExemption> exemptions;

  // What activity to do instead
  std::string replacement_activity;  // e.g., "residence", "medical_facility"
  int16_t replacement_activity_index = -1;

  // Optional: hop to a schedule instead of replacing the activity.
  // When set, getOverride triggers a schedule hop rather than calling
  // getReplacementLocation. Only effective for persons already on a hop.
  std::string replacement_schedule;
  int16_t replacement_schedule_idx = -1;

  // Compliance rate (0.0 = no one complies, 1.0 = everyone complies)
  double compliance_rate = 1.0;

  // Check if this action should override a given activity
  bool shouldOverride(const std::string& activity_name) const {
    if (override_all) return true;
    return override_activities.count(activity_name) > 0;
  }

  // Check by index
  bool shouldOverride(int16_t activity_index) const {
    if (override_all) return true;
    if (activity_index < 0 || activity_index >= 64) return false;
    return (override_activity_mask & (1ULL << activity_index));
  }

  // Check if this action has an exemption for a given activity
  bool isExempt(const Person& person, int16_t activity_index,
                const WorldState* world = nullptr,
                const Person* partner = nullptr) const {
    for (const auto& exemption : exemptions) {
      if (exemption.activity_index == activity_index) {
        if (exemption.appliesTo(person, world, partner)) {
          return true;
        }
      }
    }
    return false;
  }

  void resolve(const WorldState& world) {
    if (override_activities.empty() || override_activities.count("*") > 0) {
      override_all = true;
    } else {
      override_activity_mask = 0;
      for (const auto& act : override_activities) {
        int index = world.getActivityIndex(act);
        if (index >= 0 && index < 64) {
          override_activity_mask |= (1ULL << index);
        } else {
          std::cerr << "  [Policy Warning] Activity '" << act
                    << "' not found or index out of range for bitmask."
                    << std::endl;
        }
      }
    }

    // Resolve exemptions
    for (auto& exemption : exemptions) {
      exemption.resolve(world);
    }

    replacement_activity_index =
        static_cast<int16_t>(world.getActivityIndex(replacement_activity));

    if (!replacement_schedule.empty()) {
      replacement_schedule_idx = static_cast<int16_t>(
          world.getScheduleTypeIndex(replacement_schedule));
      if (replacement_schedule_idx < 0) {
        std::cerr << "  [Policy Warning] replacement_schedule '"
                  << replacement_schedule << "' not found." << std::endl;
      }
    }
  }
};

// =============================================================================
// Symptom-Based Policy - Override behavior based on disease symptoms
// =============================================================================

struct SymptomPolicy {
  std::string name;

  // Symptoms that trigger this policy
  std::vector<std::string> trigger_symptoms;
  uint32_t trigger_symptom_mask = 0;  // BITMASK: support up to 32 symptoms

  // Link to another policy to follow up if this one ends
  std::string follow_up_policy_name;
  int16_t follow_up_policy_index = -1;

  // Behavioral inheritance
  bool inherit_compliance = true;
  bool inherit_refusal = false;

  // What to do
  PolicyAction action;

  // Optional: selection criteria (only apply to certain people)
  std::vector<SelectionCriterion> applies_to;

  // Check if this policy applies to a person's current symptom
  bool triggeredBy(const std::string& symptom) const {
    return std::find(trigger_symptoms.begin(), trigger_symptoms.end(),
                     symptom) != trigger_symptoms.end();
  }

  // Check by symptom ID
  bool triggeredBy(uint16_t symptom_id) const {
    if (symptom_id >= 32) return false;
    return (trigger_symptom_mask & (1u << symptom_id));
  }

  // Check if policy applies to this person (based on selection criteria)
  bool appliesTo(const Person& person,
                 const WorldState* world = nullptr) const {
    // Empty criteria = applies to everyone
    if (applies_to.empty()) {
      return true;
    }

    // All criteria must match
    for (const auto& criterion : applies_to) {
      if (!criterion.evaluate(person, world)) {
        return false;
      }
    }
    return true;
  }

  void resolve(const WorldState& world, const Disease& disease) {
    for (auto& crit : applies_to) {
      crit.resolve(world);
    }

    // Intern action
    action.resolve(world);

    // Intern symptoms
    trigger_symptom_mask = 0;
    for (const auto& sym : trigger_symptoms) {
      uint16_t id = disease.getSymptomId(sym);
      if (id < 32) {
        trigger_symptom_mask |= (1u << id);
      }
    }
  }
};

// =============================================================================
// Temporal Policy - Override behavior during a time period (lockdowns, etc.)
// =============================================================================

struct TemporalPolicy {
  std::string name;

  // Time range (in simulation time, days from start)
  double start_time = 0.0;
  double end_time = -1.0;  // -1 = no end

  // What to do
  PolicyAction action;

  // Optional: selection criteria (only apply to certain people)
  std::vector<SelectionCriterion> applies_to;

  // Check if policy is active at given time
  bool isActive(double current_time) const {
    // Policy hasn't started yet
    if (current_time < start_time) {
      return false;
    }
    // Policy has ended (check if end_time is set, regardless of sign)
    // -1.0 means no end time (policy runs indefinitely)
    if (end_time != -1.0 && current_time > end_time) {
      return false;
    }
    // Policy is active
    return true;
  }

  // Check if policy applies to this person (based on selection criteria)
  bool appliesTo(const Person& person,
                 const WorldState* world = nullptr) const {
    // Empty criteria = applies to everyone
    if (applies_to.empty()) {
      return true;
    }

    // All criteria must match
    for (const auto& criterion : applies_to) {
      if (!criterion.evaluate(person, world)) {
        return false;
      }
    }
    return true;
  }

  void resolve(const WorldState& world) {
    for (auto& crit : applies_to) {
      crit.resolve(world);
    }
    action.resolve(world);
  }
};

// =============================================================================
// FrozenPersonState - Sparse storage for persons frozen by a policy hop
// =============================================================================

struct FrozenPersonState {
  uint8_t triggering_policy_index;
  int16_t paused_hopped_schedule_id;  // travel schedule to resume on recovery
  int16_t paused_return_schedule_id;  // saved return_schedule_id
  VenueId pin_venue_id;
  SubsetIndex pin_subset_index;
  // temp_slot_progress NOT saved: preserved automatically (non-temporary hops
  // never touch it, so it holds the correct travel-schedule resume position)
};

// =============================================================================
// PolicyManager - Manages all policies and determines activity overrides
// =============================================================================

class PolicyManager {
 public:
  PolicyManager(WorldState& world);

  // Set base seed for deterministic RNG (MPI reproducibility)
  void setBaseSeed(uint64_t seed) { base_seed_ = seed; }

  // Register policies
  void addSymptomPolicy(const SymptomPolicy& policy);
  void addTemporalPolicy(const TemporalPolicy& policy);

  // Get all policies (for inspection/debugging)
  const std::vector<SymptomPolicy>& getSymptomPolicies() const {
    return symptom_policies_;
  }
  const std::vector<TemporalPolicy>& getTemporalPolicies() const {
    return temporal_policies_;
  }

  // Main function: Check if a person's scheduled activity should be overridden
  // Returns std::nullopt if no override applies, otherwise returns the override
  // location
  std::optional<PersonLocation> getOverride(Person& person,
                                            int16_t scheduled_activity_index,
                                            VenueId scheduled_venue_id,
                                            SubsetIndex scheduled_subset_index,
                                            double current_time,
                                            int time_slot_index,
                                            const Person* partner = nullptr);

  // Clear all policies
  void clear() {
    symptom_policies_.clear();
    temporal_policies_.clear();
  }

  // Statistics
  size_t getSymptomPolicyCount() const { return symptom_policies_.size(); }
  size_t getTemporalPolicyCount() const { return temporal_policies_.size(); }

  // Precompute which policies can apply to each person (based on selection
  // criteria) This caches the results in person.applicable_*_policy_mask
  void precomputePolicyApplicability(std::vector<Person>& people);

  // Resolve all policy criteria and intern activities/symptoms
  void resolveAll(const Disease& disease) {
    for (auto& p : symptom_policies_) p.resolve(world_, disease);
    for (auto& p : temporal_policies_) p.resolve(world_);

    // Resolve follow-up policy indices
    for (auto& p : symptom_policies_) {
      if (!p.follow_up_policy_name.empty()) {
        for (size_t i = 0; i < symptom_policies_.size(); ++i) {
          if (symptom_policies_[i].name == p.follow_up_policy_name) {
            p.follow_up_policy_index = static_cast<int16_t>(i);
            break;
          }
        }
      }
    }
  }

 private:
  WorldState& world_;
  uint64_t base_seed_ = 0;

  std::vector<SymptomPolicy> symptom_policies_;
  std::vector<TemporalPolicy> temporal_policies_;

  // Sparse map: persons currently frozen by a policy-triggered schedule hop.
  // Only populated for the small minority of persons who are both travelling
  // and sick simultaneously — avoids touching the Person struct.
  std::unordered_map<PersonId, FrozenPersonState> frozen_states_;

  // Cached residence activity index (resolved on first use)
  int16_t residence_act_idx_ = -1;
  void ensureResidenceIndexCached() {
    if (residence_act_idx_ < 0) {
      residence_act_idx_ =
          static_cast<int16_t>(world_.getActivityIndex("residence"));
    }
  }

  // Helper: Apply compliance rate (returns true if person complies)
  bool checkCompliance(double compliance_rate, PersonId person_id,
                       uint32_t policy_index);

  // Helper: Get replacement location for a given activity name
  std::optional<PersonLocation> getReplacementLocation(
      const Person& person, const std::string& replacement_activity,
      int16_t replacement_activity_index = -1);
};

// =============================================================================
// PolicyManager Implementation
// =============================================================================

inline PolicyManager::PolicyManager(WorldState& world)
    : world_(world), base_seed_(0) {}

inline void PolicyManager::addSymptomPolicy(const SymptomPolicy& policy) {
  symptom_policies_.push_back(policy);
}

inline void PolicyManager::addTemporalPolicy(const TemporalPolicy& policy) {
  temporal_policies_.push_back(policy);
}

inline bool PolicyManager::checkCompliance(double compliance_rate,
                                           PersonId person_id,
                                           uint32_t policy_index) {
  SplitMix64 rng(mix_seed(base_seed_, person_id, policy_index, 0xC0E1A9ULL));
  std::uniform_real_distribution<double> dist(0.0, 1.0);
  return dist(rng) < compliance_rate;
}

inline std::optional<PersonLocation> PolicyManager::getReplacementLocation(
    const Person& person, const std::string& replacement_activity,
    int16_t replacement_activity_index) {
  // Determine activity index
  int16_t act_idx = replacement_activity_index;
  if (act_idx < 0) {
    act_idx =
        static_cast<int16_t>(world_.getActivityIndex(replacement_activity));
  }

  // Look up the replacement activity in person's activity map
  auto venues = world_.getActivityVenues(person, act_idx);
  if (venues.empty()) {
    // Fallback: try residence if not already tried
    int16_t residence_idx =
        static_cast<int16_t>(world_.getActivityIndex("residence"));
    if (act_idx != residence_idx) {
      return getReplacementLocation(person, "residence", residence_idx);
    }
    return std::nullopt;
  }

  // Use first venue/subset for this activity
  auto [venue_id, subset_idx] = venues[0];

  PersonLocation loc;
  loc.person_id = person.id;
  loc.venue_id = venue_id;
  loc.subset_index = subset_idx;
  loc.activity_index = act_idx;
  loc.encounter_type_id = 255;  // None

  return loc;
}

inline std::optional<PersonLocation> PolicyManager::getOverride(
    Person& person, int16_t scheduled_activity_index,
    VenueId scheduled_venue_id, SubsetIndex scheduled_subset_index,
    double current_time, int time_slot_index, const Person* partner) {
  // Priority 1: Check symptom-based policies
  // Only check policies that can apply to this person (bitmask check)
  if (person.infection != nullptr) {
    // Get symptom ID directly from trajectory
    uint16_t current_symptom_id =
        person.infection->getTrajectory().getCurrentSymptomId(current_time);

    uint32_t symptom_mask = person.applicable_symptom_policy_mask;
    for (size_t i = 0; symptom_mask && i < symptom_policies_.size(); ++i) {
      if (!(symptom_mask & (1u << i))) continue;

      const auto& policy = symptom_policies_[i];

      // Check if symptom ID triggers this policy
      bool is_triggered = policy.triggeredBy(current_symptom_id);

      // STICKY COMPLIANCE: If not triggered, clear participation, decision, and
      // continue
      if (!is_triggered) {
        // Check for follow-up policy inheritance before clearing
        if (policy.follow_up_policy_index >= 0) {
          bool decision_made = (person.symptom_policy_decisions & (1u << i));
          bool participating =
              (person.active_symptom_policy_participation & (1u << i));

          if (decision_made) {
            bool should_inherit = false;
            if (participating && policy.inherit_compliance) {
              person.active_symptom_policy_participation |=
                  (1u << policy.follow_up_policy_index);
              should_inherit = true;
            }
            if (!participating && policy.inherit_refusal) {
              should_inherit = true;
            }

            if (should_inherit) {
              person.symptom_policy_decisions |=
                  (1u << policy.follow_up_policy_index);
            }
          }
        }

        // Restore paused hop state if this policy was responsible for freezing
        auto frozen_it = frozen_states_.find(person.id);
        if (frozen_it != frozen_states_.end() &&
            frozen_it->second.triggering_policy_index ==
                static_cast<uint8_t>(i)) {
          person.hopped_schedule_id =
              frozen_it->second.paused_hopped_schedule_id;
          person.return_schedule_id =
              frozen_it->second.paused_return_schedule_id;
          frozen_states_.erase(frozen_it);
          // temp_slot_progress is already at the correct resume position
        }

        person.active_symptom_policy_participation &= ~(1u << i);
        person.symptom_policy_decisions &= ~(1u << i);
        continue;
      }

      // Check if already made a decision
      bool has_made_decision = (person.symptom_policy_decisions & (1u << i));
      if (!has_made_decision) {
        // Perform first-time compliance roll
        if (checkCompliance(policy.action.compliance_rate, person.id,
                            static_cast<uint32_t>(i))) {
          person.active_symptom_policy_participation |= (1u << i);
        }
        // Record that a decision has been made
        person.symptom_policy_decisions |= (1u << i);
      }

      bool is_participating =
          (person.active_symptom_policy_participation & (1u << i));
      if (!is_participating) continue;

      // Check if this activity index should be overridden
      if (!policy.action.shouldOverride(scheduled_activity_index)) {
        continue;
      }

      // Check for generic exemption (symptomatic isolation is hard to exempt,
      // but let's allow it if configured)
      if (policy.action.isExempt(person, scheduled_activity_index, &world_,
                                 partner)) {
        continue;
      }

      // Apply override!

      // Freeze-hop path: policy triggers a schedule hop to pin the person at
      // their current (or last overnight) venue. Only applies to hopped
      // persons.
      if (policy.action.replacement_schedule_idx >= 0 &&
          person.hopped_schedule_id >= 0) {
        ensureResidenceIndexCached();

        auto frozen_it = frozen_states_.find(person.id);
        if (frozen_it != frozen_states_.end() &&
            frozen_it->second.triggering_policy_index ==
                static_cast<uint8_t>(i)) {
          // Already frozen by this policy — return the stored pin venue
          PersonLocation loc;
          loc.person_id = person.id;
          loc.venue_id = frozen_it->second.pin_venue_id;
          loc.subset_index = frozen_it->second.pin_subset_index;
          loc.activity_index = residence_act_idx_;
          loc.encounter_type_id = 255;
          return loc;
        }

        // First freeze: determine pin venue. ActivityManager pre-resolves
        // no_venue transit slots to the last overnight venue before calling
        // getOverride, so scheduled_venue_id is already the best candidate.
        VenueId pin_venue = scheduled_venue_id;
        SubsetIndex pin_subset = scheduled_subset_index;
        if (pin_venue < 0) {
          // Fallback: home residence (person froze before first overnight)
          auto home = world_.getActivityVenues(person, residence_act_idx_);
          if (!home.empty()) {
            pin_venue = home[0].first;
            pin_subset = home[0].second;
          }
        }

        // Save paused hop state and trigger freeze hop
        int16_t saved_hop = person.hopped_schedule_id;
        int16_t saved_return = person.return_schedule_id;
        frozen_states_[person.id] =
            FrozenPersonState{static_cast<uint8_t>(i), saved_hop, saved_return,
                              pin_venue, pin_subset};
        person.hopped_schedule_id = policy.action.replacement_schedule_idx;
        // return_schedule_id is not used for non-temporary freeze schedule

        PersonLocation loc;
        loc.person_id = person.id;
        loc.venue_id = pin_venue;
        loc.subset_index = pin_subset;
        loc.activity_index = residence_act_idx_;
        loc.encounter_type_id = 255;
        return loc;
      }

      // Standard override: replace with a residence (or configured) activity
      return getReplacementLocation(person, policy.action.replacement_activity,
                                    policy.action.replacement_activity_index);
    }
  } else {
    // Not infected - clear all symptom policy participation and decisions
    if (person.symptom_policy_decisions != 0) {
      person.resetPolicyState();
    }
  }

  // Priority 2: Check temporal policies (lockdowns, etc.)
  // Only check policies that can apply to this person
  uint32_t temporal_mask = person.applicable_temporal_policy_mask;
  for (size_t i = 0; temporal_mask && i < temporal_policies_.size(); ++i) {
    if (!(temporal_mask & (1u << i))) continue;

    const auto& policy = temporal_policies_[i];

    // Check if policy is active at current time (dynamic check)
    bool is_active = policy.isActive(current_time);

    // STICKY COMPLIANCE
    if (!is_active) {
      person.active_temporal_policy_participation &= ~(1u << i);
      person.temporal_policy_decisions &= ~(1u << i);
      continue;
    }

    // Check if already made a decision
    bool has_made_decision = (person.temporal_policy_decisions & (1u << i));
    if (!has_made_decision) {
      // Perform first-time compliance roll
      if (checkCompliance(policy.action.compliance_rate, person.id,
                          static_cast<uint32_t>(i + 100))) {
        person.active_temporal_policy_participation |= (1u << i);
      }
      // Record that a decision has been made
      person.temporal_policy_decisions |= (1u << i);
    }

    bool is_participating =
        (person.active_temporal_policy_participation & (1u << i));

    if (!is_participating) continue;

    // Check if this activity index should be overridden
    if (!policy.action.shouldOverride(scheduled_activity_index)) {
      continue;
    }

    // Check for generic exemption
    if (policy.action.isExempt(person, scheduled_activity_index, &world_,
                               partner)) {
      continue;
    }

    // Apply override!
    return getReplacementLocation(person, policy.action.replacement_activity,
                                  policy.action.replacement_activity_index);
  }

  return std::nullopt;
}

inline void PolicyManager::precomputePolicyApplicability(
    std::vector<Person>& people) {
  size_t total_symptom_checks = 0;
  size_t total_temporal_checks = 0;

  for (auto& person : people) {
    if (person.is_dead) continue;

    // Reset masks
    person.applicable_symptom_policy_mask = 0;
    person.applicable_temporal_policy_mask = 0;

    // Check which symptom policies CAN apply to this person (based on selection
    // criteria)
    for (size_t i = 0; i < std::min(symptom_policies_.size(), size_t(32));
         ++i) {
      const auto& policy = symptom_policies_[i];

      // Check if policy's selection criteria match this person
      if (policy.appliesTo(person, &world_)) {
        person.applicable_symptom_policy_mask |= (1u << i);
        total_symptom_checks++;
      }
    }

    // Check which temporal policies CAN apply to this person (based on
    // selection criteria)
    for (size_t i = 0; i < std::min(temporal_policies_.size(), size_t(32));
         ++i) {
      const auto& policy = temporal_policies_[i];

      // Check if policy's selection criteria match this person
      if (policy.appliesTo(person, &world_)) {
        person.applicable_temporal_policy_mask |= (1u << i);
        total_temporal_checks++;
      }
    }
  }

  // Print statistics
  double avg_symptom =
      people.size() > 0 ? (double)total_symptom_checks / people.size() : 0.0;
  double avg_temporal =
      people.size() > 0 ? (double)total_temporal_checks / people.size() : 0.0;
}

}  // namespace june
