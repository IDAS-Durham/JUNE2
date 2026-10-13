#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

#include "core/world_state.h"
#include "doctest.h"
#include "epidemiology/disease.h"
#include "epidemiology/vaccination_manager.h"
#include "epidemiology/vaccine.h"
#include "test_utils.h"

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

TEST_CASE("vaccination roll seed is a fixed function of its inputs") {
  // Golden values from an independent implementation of mix_seed and
  // SplitMix64; they must hold on every build.
  CHECK(vaccinationRollSeed(12345, 42, 1, 3) == 4225968526751640907ULL);
  SplitMix64 a(vaccinationRollSeed(12345, 42, 1, 3));
  CHECK(uniform01(a) == 0x1.545c55eb46e59p-1);
  CHECK(vaccinationRollSeed(7, 1000000, 0, 59) == 4394853279783811244ULL);
  SplitMix64 b(vaccinationRollSeed(7, 1000000, 0, 59));
  CHECK(uniform01(b) == 0x1.334261ef193dcp-1);
}

TEST_CASE("vaccination roll depends on the run seed") {
  CHECK(vaccinationRollSeed(12345, 42, 0, 3) !=
        vaccinationRollSeed(12346, 42, 0, 3));
}

TEST_CASE(
    "vaccination rolls do not share a draw across people, days or "
    "campaigns") {
  // The old seed was person ^ (day * 1000) ^ a constant, so the person with id
  // 3000 on day 0 and the person with id 0 on day 3 drew the same number.
  auto old_seed = [](unsigned person, unsigned day) {
    return person ^ (day * 1000) ^ 0xBADBEEFu;
  };
  CHECK(old_seed(3000, 0) == old_seed(0, 3));
  CHECK(vaccinationRollSeed(1, 3000, 0, 0) != vaccinationRollSeed(1, 0, 0, 3));

  std::set<uint64_t> seeds;
  size_t total = 0;
  for (PersonId person = 0; person < 3000; ++person)
    for (uint64_t day = 0; day < 40; ++day)
      for (size_t campaign = 0; campaign < 3; ++campaign) {
        seeds.insert(vaccinationRollSeed(99, person, campaign, day));
        ++total;
      }
  CHECK(seeds.size() == total);
}

TEST_CASE("vaccination coverage roll hits the configured fraction") {
  for (double coverage : {0.1, 0.5}) {
    size_t hits = 0;
    const size_t n = 200000;
    for (size_t person = 0; person < n; ++person) {
      SplitMix64 rng(
          vaccinationRollSeed(2024, static_cast<PersonId>(person), 0, 4));
      if (uniform01(rng) < coverage) ++hits;
    }
    // Four standard deviations of a binomial with n = 200000.
    const double tolerance = 4.0 * std::sqrt(coverage * (1 - coverage) / n);
    CHECK(std::abs(static_cast<double>(hits) / n - coverage) < tolerance);
  }
}

TEST_CASE("vaccination day index accepts whole days and names a bad time") {
  CHECK(vaccinationDayIndex(0.0) == 0);
  CHECK(vaccinationDayIndex(59.0) == 59);
  CHECK_THROWS_WITH_AS(vaccinationDayIndex(-1.0), doctest::Contains("-1.0"),
                       std::runtime_error);
  CHECK_THROWS_WITH_AS(vaccinationDayIndex(2.5), doctest::Contains("2.5"),
                       std::runtime_error);
  CHECK_THROWS_WITH_AS(
      vaccinationDayIndex(std::numeric_limits<double>::quiet_NaN()),
      doctest::Contains("not a whole"), std::runtime_error);
  CHECK_THROWS_AS(vaccinationDayIndex(std::numeric_limits<double>::infinity()),
                  std::runtime_error);
}

TEST_CASE("VaccinationManager update rejects a time that is not a whole day") {
  Config config;
  WorldState world;
  VaccinationManager manager(world, config);
  CHECK_NOTHROW(manager.update(3.0));
  CHECK_THROWS_AS(manager.update(3.5), std::runtime_error);
  CHECK_THROWS_AS(manager.update(-1.0), std::runtime_error);
}

namespace {

// Two campaigns active on day 0 for everyone, each with its own vaccine.
Config twoCampaignConfig(uint32_t seed, double a, double b) {
  Config config;
  config.simulation.random_seed = seed;
  config.simulation.start_date = "2024-01-01";
  config.vaccination.enabled = true;
  for (const char* name : {"VaxA", "VaxB"}) {
    VaccineConfig vaccine;
    vaccine.name = name;
    vaccine.doses.push_back({0, 0.0, 100.0, 200.0});
    config.vaccination.vaccines[name] = vaccine;
  }
  double coverage[2] = {a, b};
  const char* vaccine_name[2] = {"VaxA", "VaxB"};
  for (int i = 0; i < 2; ++i) {
    VaccinationCampaignConfig c;
    c.name = vaccine_name[i];
    c.vaccine_type = vaccine_name[i];
    c.start_date = "2024-01-01";
    c.end_date = "2024-02-01";
    c.dose_sequence = {0};
    c.daily_coverage = coverage[i];
    config.vaccination.campaigns.push_back(c);
  }
  return config;
}

std::set<PersonId> vaccinatedOnDayZero(uint32_t seed, double a, double b,
                                       int people) {
  Config config = twoCampaignConfig(seed, a, b);
  WorldState world = TestWorldFactory::createMinimalWorld(people, 0);
  VaccinationManager manager(world, config);
  manager.update(0.0);
  std::set<PersonId> out;
  for (const auto& p : world.people)
    if (p.vaccine_trajectory && p.vaccine_trajectory->getNumDoses() > 0)
      out.insert(p.id);
  return out;
}

}  // namespace

TEST_CASE("overlapping campaigns reach 1 - (1 - a)(1 - b) of the people") {
  const int n = 20000;
  const double a = 0.3, b = 0.4;
  const double expected = 1.0 - (1.0 - a) * (1.0 - b);
  const double tolerance = 4.0 * std::sqrt(expected * (1.0 - expected) / n);

  const auto ab = vaccinatedOnDayZero(7, a, b, n);
  CHECK(std::abs(static_cast<double>(ab.size()) / n - expected) < tolerance);
  // The larger coverage alone would give 0.4, well outside the tolerance.
  CHECK(static_cast<double>(ab.size()) / n > b + 4.0 * tolerance);

  // Config order decides the vaccine, not how many people are reached.
  const auto ba = vaccinatedOnDayZero(7, b, a, n);
  CHECK(std::abs(static_cast<double>(ba.size()) / n - expected) < tolerance);
}

TEST_CASE("a different run seed vaccinates different people") {
  const auto one = vaccinatedOnDayZero(1, 0.3, 0.4, 5000);
  const auto two = vaccinatedOnDayZero(2, 0.3, 0.4, 5000);
  CHECK(one != two);
  CHECK(one == vaccinatedOnDayZero(1, 0.3, 0.4, 5000));
}
