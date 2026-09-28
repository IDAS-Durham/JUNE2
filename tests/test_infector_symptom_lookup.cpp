#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <string>
#include <unordered_map>

#include "core/types.h"
#include "core/world_state.h"
#include "doctest.h"
#include "epidemiology/transmission/infector_symptom_lookup.h"

using namespace june;

TEST_CASE(
    "a Person infector with no Infection is an absent symptom and a gap") {
  WorldState world;
  Person& infector = world.people.emplace_back();
  infector.id = 7;
  world.buildIndices();
  InfectorSymptomLookup lookup(world);

  CHECK(lookup.resolve(InfectionSource::Person, 7, 1.0, nullptr) ==
        kNoSymptomId);
  CHECK(lookup.gapCount() == 1);
}

TEST_CASE(
    "a visitor missing from the visitor data is an absent symptom and a gap") {
  WorldState world;
  world.buildIndices();
  std::unordered_map<PersonId, VisitorInfo> visitor_data;
  VisitorInfo other_visitor;
  other_visitor.symptom_id = 2;
  visitor_data[8] = other_visitor;
  InfectorSymptomLookup lookup(world);

  CHECK(lookup.resolve(InfectionSource::Person, 8, 1.0, &visitor_data) == 2);
  CHECK(lookup.gapCount() == 0);
  CHECK(lookup.resolve(InfectionSource::Person, 9, 1.0, &visitor_data) ==
        kNoSymptomId);
  CHECK(lookup.gapCount() == 1);
}

TEST_CASE(
    "a Person source with no infector sampled is an absent symptom and a gap") {
  WorldState world;
  world.buildIndices();
  InfectorSymptomLookup lookup(world);

  CHECK(lookup.resolve(InfectionSource::Person, -1, 1.0, nullptr) ==
        kNoSymptomId);
  CHECK(lookup.gapCount() == 1);
}

TEST_CASE("a Fomite or compartmental source is an absent symptom, not a gap") {
  WorldState world;
  world.buildIndices();
  InfectorSymptomLookup lookup(world);

  CHECK(lookup.resolve(InfectionSource::Fomite, -1, 1.0, nullptr) ==
        kNoSymptomId);
  CHECK(lookup.resolve(InfectionSource::Compartmental, -1, 1.0, nullptr) ==
        kNoSymptomId);
  CHECK(lookup.gapCount() == 0);
}

TEST_CASE("the lookup-gap warning names the count and is silent at zero") {
  CHECK(formatInfectorLookupGapWarning(0).empty());
  const std::string warning = formatInfectorLookupGapWarning(3);
  CHECK(warning.rfind("[WARNING]", 0) == 0);
  CHECK(warning.find(" 3 ") != std::string::npos);
  CHECK(warning.find("no infector sampled") != std::string::npos);
}
