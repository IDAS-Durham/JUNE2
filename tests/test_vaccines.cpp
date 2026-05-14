#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <cmath>

#include "core/types.h"
#include "doctest.h"
#include "epidemiology/disease.h"
#include "epidemiology/vaccine.h"

using namespace june;

TEST_CASE("Vaccine - Efficacy Interpolation") {
  Dose dose;
  dose.number = 0;
  dose.day_administered = 10.0;
  dose.days_to_effective = 14.0;
  dose.days_to_waning = 30.0;
  dose.days_to_finished = 60.0;
  dose.waning_factor = 0.5;
  dose.infection_efficacy["TestDisease"] = {{0, 100, 0.8}};

  SUBCASE("Before administration") {
    CHECK(std::abs(dose.getEfficacy(5.0, "TestDisease", 25.0) - 0.0) < 1e-6);
  }

  SUBCASE("Ramp up - halfway to effective") {
    CHECK(std::abs(dose.getEfficacy(17.0, "TestDisease", 25.0) - 0.4) < 1e-6);
  }

  SUBCASE("Peak effective") {
    CHECK(std::abs(dose.getEfficacy(30.0, "TestDisease", 25.0) - 0.8) < 1e-6);
  }

  SUBCASE("Mid-waning") {
    // Progress = (45 - 30) / (60 - 30) = 0.5
    // Efficacy = 0.8 - 0.5 * (0.8 - 0.4) = 0.6
    CHECK(std::abs(dose.getEfficacy(55.0, "TestDisease", 25.0) - 0.6) < 1e-6);
  }

  SUBCASE("Post-waning baseline") {
    CHECK(std::abs(dose.getEfficacy(80.0, "TestDisease", 25.0) - 0.4) < 1e-6);
  }
}

TEST_CASE("Vaccine - Hybrid Immunity Aggregation") {
  Person person;
  person.age = 30;
  person.sex = Sex::MALE;

  SUBCASE("No immunity - full susceptibility") {
    CHECK(std::abs(person.getSusceptibility(0.0, "TestDisease") - 1.0) < 1e-6);
  }

  SUBCASE("Natural immunity only - 50% protection") {
    person.immunity.natural_level = 0.5;
    person.immunity.natural_acquisition_time = 0.0;
    person.immunity.natural_waning_rate = 0.0;
    CHECK(std::abs(person.getSusceptibility(10.0, "TestDisease") - 0.5) < 1e-6);
  }

  SUBCASE("Vaccine immunity only - 80% protection") {
    person.immunity.natural_level = 0.0;
    person.vaccine_trajectory = std::make_unique<VaccineTrajectory>();

    Dose dose;
    dose.day_administered = 0.0;
    dose.days_to_effective = 0.0;
    dose.days_to_waning = 100.0;
    dose.infection_efficacy["TestDisease"] = {{0, 100, 0.8}};
    person.vaccine_trajectory->addDose(dose);

    CHECK(std::abs(person.getSusceptibility(10.0, "TestDisease") - 0.2) < 1e-6);
  }

  SUBCASE("Hybrid immunity - multiplicative") {
    person.immunity.natural_level = 0.5;
    person.immunity.natural_acquisition_time = 0.0;
    person.immunity.natural_waning_rate = 0.0;
    person.vaccine_trajectory = std::make_unique<VaccineTrajectory>();

    Dose dose;
    dose.day_administered = 0.0;
    dose.days_to_effective = 0.0;
    dose.days_to_waning = 100.0;
    dose.infection_efficacy["TestDisease"] = {{0, 100, 0.8}};
    person.vaccine_trajectory->addDose(dose);

    // natural_suscep = 0.5, vaccine_suscep = 0.2 → total = 0.1
    CHECK(std::abs(person.getSusceptibility(10.0, "TestDisease") - 0.1) < 1e-6);
  }
}
