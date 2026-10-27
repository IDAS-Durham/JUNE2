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

namespace {

// A dose given at the start of the run. Peak infection efficacy is 0.8,
// held until day 100, then waning to half the peak by day 200.
Dose makeOnsetDose(double onset, double effective, double administered = 0.0) {
  Dose dose;
  dose.number = 0;
  dose.day_administered = administered;
  dose.days_to_onset = onset;
  dose.days_to_effective = effective;
  dose.days_to_waning = 100.0;
  dose.days_to_finished = 200.0;
  dose.waning_factor = 0.5;
  dose.infection_efficacy["TestDisease"] = {{0, 100, 0.8}};
  dose.symptom_efficacy["TestDisease"] = {{0, 100, 0.6}};
  return dose;
}

}  // namespace

TEST_CASE("Vaccine - Onset Delay") {
  SUBCASE("Onset 0 is the plain ramp from the day of the dose") {
    const Dose dose = makeOnsetDose(0.0, 14.0);
    for (double day : {0.0, 3.5, 7.0, 13.999}) {
      // The ramp as it was before onset existed, to the last bit
      CHECK(dose.getEfficacy(day, "TestDisease", 25.0) == (day / 14.0) * 0.8);
    }
    CHECK(dose.getEfficacy(14.0, "TestDisease", 25.0) == 0.8);
  }

  SUBCASE("Onset equal to days_to_effective is a step") {
    const Dose dose = makeOnsetDose(14.0, 14.0);
    CHECK(dose.getEfficacy(0.0, "TestDisease", 25.0) == 0.0);
    CHECK(dose.getEfficacy(7.0, "TestDisease", 25.0) == 0.0);
    CHECK(dose.getEfficacy(13.999, "TestDisease", 25.0) == 0.0);
    CHECK(dose.getEfficacy(14.0, "TestDisease", 25.0) == 0.8);
    // The peak is held right up to the start of waning
    CHECK(dose.getEfficacy(50.0, "TestDisease", 25.0) == 0.8);
    CHECK(dose.getEfficacy(99.999, "TestDisease", 25.0) == 0.8);
  }

  SUBCASE("A delay and then a rise") {
    const Dose dose = makeOnsetDose(7.0, 14.0);
    CHECK(dose.getEfficacy(0.0, "TestDisease", 25.0) == 0.0);
    CHECK(dose.getEfficacy(3.0, "TestDisease", 25.0) == 0.0);
    CHECK(dose.getEfficacy(7.0, "TestDisease", 25.0) == 0.0);
    CHECK(dose.getEfficacy(10.5, "TestDisease", 25.0) ==
          doctest::Approx(0.4).epsilon(1e-12));
    CHECK(dose.getEfficacy(14.0, "TestDisease", 25.0) == 0.8);
  }

  SUBCASE("Waning after the peak does not depend on onset") {
    const Dose step = makeOnsetDose(14.0, 14.0);
    const Dose ramp = makeOnsetDose(0.0, 14.0);
    const Dose delayed = makeOnsetDose(7.0, 14.0);
    for (double day : {14.0, 50.0, 100.0, 125.0, 150.0, 199.0, 200.0, 300.0}) {
      CHECK(step.getEfficacy(day, "TestDisease", 25.0) ==
            ramp.getEfficacy(day, "TestDisease", 25.0));
      CHECK(delayed.getEfficacy(day, "TestDisease", 25.0) ==
            ramp.getEfficacy(day, "TestDisease", 25.0));
    }
    // Halfway through waning: 0.8 - 0.5 * (0.8 - 0.4)
    CHECK(step.getEfficacy(150.0, "TestDisease", 25.0) ==
          doctest::Approx(0.6).epsilon(1e-12));
    CHECK(step.getEfficacy(300.0, "TestDisease", 25.0) ==
          doctest::Approx(0.4).epsilon(1e-12));
  }

  SUBCASE("Onset counts from the day of the dose") {
    const Dose dose = makeOnsetDose(14.0, 14.0, /*administered=*/10.0);
    CHECK(dose.getEfficacy(9.0, "TestDisease", 25.0) == 0.0);
    CHECK(dose.getEfficacy(23.999, "TestDisease", 25.0) == 0.0);
    CHECK(dose.getEfficacy(24.0, "TestDisease", 25.0) == 0.8);
  }

  SUBCASE("Symptom efficacy follows the same onset") {
    const Dose dose = makeOnsetDose(14.0, 14.0);
    CHECK(dose.getEfficacy(13.0, "TestDisease", 25.0, true) == 0.0);
    CHECK(dose.getEfficacy(14.0, "TestDisease", 25.0, true) == 0.6);
  }
}

TEST_CASE("Vaccine - Onset Delay Through Susceptibility") {
  Person person;
  person.age = 30;
  person.sex = Sex::MALE;
  person.immunity.natural_level = 0.0;
  person.vaccine_trajectory = std::make_unique<VaccineTrajectory>();
  person.vaccine_trajectory->addDose(makeOnsetDose(14.0, 14.0));

  // Fully susceptible the day before onset, protected from the day of onset
  CHECK(person.getSusceptibility(13.0, "TestDisease") == 1.0);
  CHECK(person.getSusceptibility(14.0, "TestDisease") ==
        doctest::Approx(1.0 - 0.8));
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
