#pragma once

#include <memory>
#include <random>
#include <string>
#include <vector>

#include "../core/config.h"
#include "../core/world_state.h"
#include "../utils/event_logger.h"
#include "../utils/random.h"
#include "../utils/time_utils.h"
#include "vaccine.h"

namespace june {

class VaccinationCampaign {
 public:
  VaccinationCampaign(const VaccinationCampaignConfig& config,
                      const Config& full_config)
      : config_(config), full_config_(full_config) {
    std::tm sim_start = parseDate(full_config.simulation.start_date);

    if (!config.start_date.empty()) {
      std::tm camp_start = parseDate(config.start_date);
      start_day_offset_ = daysBetween(sim_start, camp_start);
    } else {
      start_day_offset_ = -1.0;  // Active from start
    }

    if (!config.end_date.empty()) {
      std::tm camp_end = parseDate(config.end_date);
      end_day_offset_ = daysBetween(sim_start, camp_end);
    } else {
      end_day_offset_ = 1e9;  // Far future
    }
  }
  bool isActive(double current_day) const {
    return current_day >= start_day_offset_ && current_day <= end_day_offset_;
  }

  // Checks if someone is allowed to get a vaccine from this specific campaign
  bool isEligible(const Person& person, double current_time,
                  const WorldState& world) const {
    // 1. Selection criteria match
    for (const auto& criterion : config_.selection_criteria) {
      if (!criterion.evaluate(person, &world)) {
        return false;
      }
    }

    if (config_.dose_sequence.empty()) return false;

    int current_doses = person.vaccine_trajectory
                            ? person.vaccine_trajectory->getNumDoses()
                            : 0;

    // Check if this campaign provides the next dose the person needs
    bool provides_needed_dose = false;
    for (int dose_num : config_.dose_sequence) {
      if (dose_num == current_doses) {
        provides_needed_dose = true;
        break;
      }
    }

    if (!provides_needed_dose) {
      return false;
    }

    // Booster filter: check previous vaccine type
    if (current_doses > 0 && !config_.last_dose_type_filter.empty()) {
      bool type_match = false;
      for (const auto& filter_type : config_.last_dose_type_filter) {
        if (person.vaccine_trajectory->vaccine_name == filter_type) {
          type_match = true;
          break;
        }
      }
      if (!type_match) return false;
    }

    // If already have some doses, check timing for the next one
    if (current_doses > 0) {
      double last_time = person.vaccine_trajectory->getLastDoseTime();
      double wait_time = 0.0;
      // Use current_doses - 1 as index for days_to_next_dose
      if (current_doses - 1 <
          static_cast<int>(config_.days_to_next_dose.size())) {
        wait_time = config_.days_to_next_dose[current_doses - 1];
      }

      if (current_time < last_time + wait_time) {
        return false;
      }
    }
    return true;
  }

  const VaccinationCampaignConfig& getConfig() const { return config_; }

 private:
  VaccinationCampaignConfig config_;
  const Config& full_config_;
  double start_day_offset_;
  double end_day_offset_;
};

// This class manages all the different vaccination campaigns going on
class VaccinationManager {
 public:
  VaccinationManager(WorldState& world, const Config& config,
                     EventLogger* event_logger = nullptr)
      : world_(world), config_(config), event_logger_(event_logger) {
    if (config.vaccination.enabled) {
      for (const auto& camp_cfg : config.vaccination.campaigns) {
        campaigns_.push_back(
            std::make_unique<VaccinationCampaign>(camp_cfg, config));
      }
    }
  }

  // This is called every simulation day to hand out vaccines
  void update(double current_time) {
    if (!config_.vaccination.enabled) return;

    // 1. Identify active campaigns once for the day
    std::vector<VaccinationCampaign*> active_campaigns;
    for (const auto& campaign : campaigns_) {
      if (campaign->isActive(current_time)) {
        active_campaigns.push_back(campaign.get());
      }
    }

    if (active_campaigns.empty()) return;

    // 2. Iterate through people
    for (auto& person : world_.people) {
      if (person.is_dead) continue;

      for (auto* campaign : active_campaigns) {
        const auto& camp_cfg = campaign->getConfig();

        // Eligibility check
        if (campaign->isEligible(person, current_time, world_)) {
          // Daily probability check using a stable per-person seed
          unsigned int vax_seed =
              static_cast<unsigned int>(person.id) ^
              static_cast<unsigned int>(current_time * 1000) ^ 0xBADBEEF;
          std::mt19937 vax_rng(vax_seed);
          std::uniform_real_distribution<double> vax_dist(0.0, 1.0);

          if (vax_dist(vax_rng) < camp_cfg.daily_coverage) {
            administerVaccine(person, camp_cfg.vaccine_type, current_time);
            break;  // Only one vaccine/campaign per day for a person
          }
        }
      }
    }
  }

 private:
  void administerVaccine(Person& person, const std::string& vaccine_type,
                         double current_time) {
    auto it = config_.vaccination.vaccines.find(vaccine_type);
    if (it == config_.vaccination.vaccines.end()) return;

    if (!person.vaccine_trajectory) {
      person.vaccine_trajectory = std::make_unique<VaccineTrajectory>();
      person.vaccine_trajectory->vaccine_name = vaccine_type;
    }

    // Determine which dose to give
    int dose_idx = person.vaccine_trajectory->getNumDoses();
    if (dose_idx >= static_cast<int>(it->second.doses.size())) return;

    const auto& d_cfg = it->second.doses[dose_idx];

    Dose dose;
    dose.number = d_cfg.number;
    dose.day_administered = current_time;
    dose.days_to_effective = d_cfg.days_to_effective;
    dose.days_to_waning = d_cfg.days_to_waning;
    dose.days_to_finished = d_cfg.days_to_finished;
    dose.waning_factor = d_cfg.waning_factor;
    dose.infection_efficacy = d_cfg.infection_efficacy;
    dose.symptom_efficacy = d_cfg.symptom_efficacy;

    person.vaccine_trajectory->addDose(dose);

    // Log the vaccination event
    if (event_logger_) {
      event_logger_->logVaccination(person.id, vaccine_type, dose_idx,
                                    current_time);
    }
  }

  WorldState& world_;
  const Config& config_;
  EventLogger* event_logger_;
  std::vector<std::unique_ptr<VaccinationCampaign>> campaigns_;
};

}  // namespace june
