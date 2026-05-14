#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "core/world_state.h"
#include "doctest.h"
#include "epidemiology/disease.h"
#include "epidemiology/vaccination_manager.h"
#include "epidemiology/vaccine.h"

using namespace june;

TEST_CASE("VaccinationCampaign - Activity Window") {
  VaccinationCampaignConfig config;
  config.start_date = "2024-01-01";
  config.end_date = "2024-01-10";

  Config full_config;
  full_config.simulation.start_date = "2024-01-01";
  full_config.vaccination.enabled = true;
  full_config.vaccination.campaigns.push_back(config);

  WorldState world;
  VaccinationCampaign campaign(config, full_config);

  SUBCASE("Day 0 - start date is active") {
    CHECK(campaign.isActive(0.0) == true);
  }
  SUBCASE("Day 5 - mid-campaign is active") {
    CHECK(campaign.isActive(5.0) == true);
  }
  SUBCASE("Day 9 - last day is active") {
    CHECK(campaign.isActive(9.0) == true);
  }
  SUBCASE("Day 10 - past end date is inactive") {
    CHECK(campaign.isActive(10.0) == false);
  }
}

TEST_CASE("VaccinationCampaign - Eligibility") {
  VaccinationCampaignConfig config;
  config.vaccine_type = "VaxA";
  config.dose_sequence = {0};

  SelectionCriterion crit;
  crit.property_path = "age";
  crit.operator_type = ">=";
  crit.value = 18;
  config.selection_criteria.push_back(crit);

  Config full_config;
  full_config.simulation.start_date = "2024-01-01";
  full_config.vaccination.campaigns.push_back(config);

  WorldState world;
  VaccinationCampaign campaign(config, full_config);

  Person adult;
  adult.age = 25;
  Person child;
  child.age = 10;

  SUBCASE("Adult is eligible, child is not") {
    CHECK(campaign.isEligible(adult, 0.0, world) == true);
    CHECK(campaign.isEligible(child, 0.0, world) == false);
  }

  SUBCASE("Already vaccinated person is not eligible for same dose") {
    adult.vaccine_trajectory = std::make_unique<VaccineTrajectory>();
    Dose d;
    d.number = 0;
    d.day_administered = 0.0;
    adult.vaccine_trajectory->addDose(d);

    CHECK(campaign.isEligible(adult, 5.0, world) == false);
  }

  SUBCASE("Second dose - respects waiting period") {
    adult.vaccine_trajectory = std::make_unique<VaccineTrajectory>();
    Dose d;
    d.number = 0;
    d.day_administered = 0.0;
    adult.vaccine_trajectory->addDose(d);

    VaccinationCampaignConfig config2 = config;
    config2.dose_sequence = {1};
    config2.days_to_next_dose = {28.0};
    VaccinationCampaign campaign2(config2, full_config);

    CHECK(campaign2.isEligible(adult, 5.0, world) == false);  // Too early
    CHECK(campaign2.isEligible(adult, 30.0, world) ==
          true);  // Past waiting period
  }
}
