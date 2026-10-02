// Unit tests for Visitor payload building (include/parallel/visitor_payload.h):
// a packed record's emission tails equal EmissionCalculator::emit and its
// symptom id equals InfectionTrajectory::getCurrentSymptomId, as for a
// local infector. No MPI runtime, so this runs as a plain ctest binary, not
// under mpirun.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#ifdef USE_MPI
#include "emission_fixtures.h"
#include "epidemiology/emission/emission.h"
#include "parallel/visitor_payload.h"

using namespace june;
using namespace emission_fixtures;

namespace {

constexpr double kSlotStart = 10.0;
constexpr double kSlotHours = 6.0;

PersonLocation makeLocation() {
  PersonLocation location;
  location.person_id = 4242;
  location.venue_id = 777;
  location.subset_index = 3;
  location.encounter_type_id = 5;
  return location;
}

Domain::VisitorData pack(const Person& person, const Disease& disease,
                         const EmissionCalculator& calculator) {
  return buildVisitorPayload(makeLocation(), person, /*home_rank=*/1,
                             kSlotStart, disease, /*policy_manager=*/nullptr,
                             calculator);
}

void checkMatchesEmit(const Domain::VisitorData& visitor, const Person& person,
                      const EmissionCalculator& calculator) {
  Emission expected;
  calculator.emit(person, kSlotStart, expected);
  CHECK(visitor.emission == expected);
}

}  // namespace

TEST_CASE("Uninfected Person packs empty tails and symptom 0") {
  Disease disease = makeDisease(2.0);
  EmissionCalculator calculator(disease, kSlotHours);
  Person person{};

  Domain::VisitorData visitor = pack(person, disease, calculator);

  CHECK(visitor.person_id == 4242);
  CHECK(visitor.home_rank == 1);
  CHECK(visitor.venue_id == 777);
  CHECK(visitor.subset_idx == 3);
  CHECK(visitor.encounter_type_id == 5);
  CHECK_FALSE(visitor.is_infected);
  CHECK(visitor.symptom_id == 0);
  CHECK(visitor.emission.infectiousness_by_mode.empty());
  CHECK(visitor.emission.fomite_deposits.empty());
  checkMatchesEmit(visitor, person, calculator);
}

TEST_CASE("Incubating: zero per mode, deposits as usual") {
  Disease disease = makeDisease(2.0);
  EmissionCalculator calculator(disease, kSlotHours);
  Person person =
      makeInfectedPerson(disease, 8.0, {{9.0, kHealthy}, {11.0, kMild}});

  Domain::VisitorData visitor = pack(person, disease, calculator);

  CHECK(visitor.is_infected);
  CHECK(visitor.symptom_id ==
        person.infection->getTrajectory().getCurrentSymptomId(kSlotStart));
  CHECK(visitor.emission.infectiousness_by_mode ==
        std::vector<double>{0.0, 0.0, 0.0});
  CHECK(visitor.emission.fomite_deposits.size() == 3);
  checkMatchesEmit(visitor, person, calculator);
}

TEST_CASE("Infectious: per-mode integrals and deposits") {
  Disease disease = makeDisease(2.0);
  EmissionCalculator calculator(disease, kSlotHours);
  Person person =
      makeInfectedPerson(disease, 8.0, {{9.0, kExposed}, {9.5, kMild}});

  Domain::VisitorData visitor = pack(person, disease, calculator);

  CHECK(visitor.is_infected);
  CHECK(visitor.symptom_id == kMild);
  CHECK(visitor.symptom_id ==
        person.infection->getTrajectory().getCurrentSymptomId(kSlotStart));
  CHECK(visitor.emission.infectiousness_by_mode.size() == 3);
  checkMatchesEmit(visitor, person, calculator);
}

TEST_CASE("Before the first transition the symptom id is the first stage") {
  Disease disease = makeDisease(2.0);
  EmissionCalculator calculator(disease, kSlotHours);
  Person person = makeInfectedPerson(disease, 9.0, {{11.0, kMild}});

  Domain::VisitorData visitor = pack(person, disease, calculator);

  CHECK(visitor.symptom_id == kMild);
  CHECK(visitor.symptom_id ==
        person.infection->getTrajectory().getCurrentSymptomId(kSlotStart));
}

#endif  // USE_MPI
