#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_set>

#include "core/types.h"
#include "core/world_state.h"
#include "epidemiology/disease.h"
#include "epidemiology/policy.h"
#include "epidemiology/transmission_modifiers.h"
#include "loaders/policy_loader.h"

using namespace june;

namespace {

struct Fixture {
  WorldState world;
  Disease disease;

  Fixture()
      : disease("modifier-test", {SymptomTag{"healthy", -1, 0}},
                DiseaseStageSettings{}, {}, OutcomeRates{}, makeParams()) {
    world.venue_type_names = {"office"};
    world.person_property_names = {"role"};
    world.person_property_value_registries["role"] = {"worker", "retired"};
    world.venue_property_names = {"cleanliness"};
    world.venue_property_value_registries["cleanliness"] = {"clean", "dirty"};

    Person& worker = world.people.emplace_back();
    worker.id = 0;
    worker.age = 30.0f;
    worker.sex = Sex::MALE;
    worker.geo_unit_id = 0;
    worker.properties_start = 0;
    worker.properties_count = 1;
    world.person_properties.push_back(0);  // role=worker

    Person& retired = world.people.emplace_back();
    retired.id = 1;
    retired.age = 70.0f;
    retired.sex = Sex::FEMALE;
    retired.geo_unit_id = 0;
    retired.properties_start = 1;
    retired.properties_count = 1;
    world.person_properties.push_back(1);  // role=retired

    Venue office;
    office.id = 0;
    office.type_id = 0;
    office.geo_unit_id = 0;
    office.properties_start = 0;
    office.properties_count = 1;
    world.venues.push_back(office);
    world.venue_properties.push_back(1);  // cleanliness=dirty
    world.buildIndices();
  }

  static TransmissionParams makeParams() {
    TransmissionParams params;
    params.modes.push_back(TransmissionMode{"direct"});
    params.modes.push_back(TransmissionMode{"fomite"});
    return params;
  }
};

PolicyTransmissionEffect personEffect(uint16_t policy_index,
                                      const std::string& property,
                                      const std::string& value, double factor) {
  PolicyTransmissionEffect effect;
  effect.policy_index = policy_index;
  effect.scope = TransmissionEffectScope::Person;
  effect.mode_name = "direct";
  effect.channel = TransmissionEffectChannel::TargetSusceptibility;
  effect.multiplier = factor;
  SelectionCriterion criterion;
  criterion.property_path = property;
  criterion.operator_type = "==";
  criterion.value = value;
  effect.criteria.push_back(std::move(criterion));
  return effect;
}

std::filesystem::path writeTempFile(const std::string& suffix,
                                    const std::string& contents) {
  static int serial = 0;
  const auto path =
      std::filesystem::temp_directory_path() /
      ("june2_transmission_modifier_" + std::to_string(++serial) + suffix);
  std::ofstream file(path);
  REQUIRE(file.is_open());
  file << contents;
  file.close();
  return path;
}

struct TempFiles {
  std::filesystem::path csv;
  std::filesystem::path yaml;
  ~TempFiles() {
    std::error_code ec;
    std::filesystem::remove(csv, ec);
    std::filesystem::remove(yaml, ec);
  }
};

}  // namespace

TEST_CASE(
    "transmission modifier CSV filters arbitrary person and venue properties") {
  Fixture f;
  TempFiles files;
  files.csv = writeTempFile(
      ".csv",
      "filter.properties.role,filter.properties.cleanliness,scope,"
      "transmission_mode,effect_channel,multiplier\n"
      "worker,,person,direct,target_susceptibility,0.60\n"
      ",dirty,venue,fomite,environmental_risk,0.50\n");
  files.yaml = writeTempFile(".yaml",
                             "policies:\n"
                             "  temporal_policies:\n"
                             "    - name: controls\n"
                             "      compliance_rate: 1.0\n"
                             "      transmission_effects_file: " +
                                 files.csv.string() + "\n");

  PolicyManager manager(f.world);
  PolicyLoader::loadPolicies(manager, files.yaml.string(), "2020-01-01");
  manager.precomputePolicyApplicability(f.world.people);
  manager.initializeTransmissionModifiers(f.disease, 0.0);

  CHECK(
      manager.personModifier(f.world.people[0], 0,
                             TransmissionEffectChannel::TargetSusceptibility) ==
      doctest::Approx(0.60));
  CHECK(
      manager.personModifier(f.world.people[1], 0,
                             TransmissionEffectChannel::TargetSusceptibility) ==
      doctest::Approx(1.0));
  CHECK(manager.venueModifier(&f.world.venues[0], 1,
                              TransmissionEffectChannel::EnvironmentalRisk) ==
        doctest::Approx(0.50));
  CHECK(manager.venueModifier(&f.world.venues[0], 0,
                              TransmissionEffectChannel::EnvironmentalRisk) ==
        doctest::Approx(1.0));
}

TEST_CASE("transmission modifier CSV rejects duplicate matching definitions") {
  Fixture f;
  TempFiles files;
  files.csv = writeTempFile(
      ".csv",
      "filter.properties.role,scope,transmission_mode,effect_channel,"
      "multiplier\n"
      "worker,person,direct,target_susceptibility,0.60\n"
      "worker,person,direct,target_susceptibility,0.70\n");
  files.yaml = writeTempFile(".yaml",
                             "policies:\n"
                             "  temporal_policies:\n"
                             "    - name: duplicate\n"
                             "      transmission_effects_file: " +
                                 files.csv.string() + "\n");

  PolicyManager manager(f.world);
  CHECK_THROWS_WITH(
      PolicyLoader::loadPolicies(manager, files.yaml.string(), "2020-01-01"),
      doctest::Contains("duplicate matching transmission effect"));
}

TEST_CASE("venue effects require fully compliant temporal policies") {
  Fixture f;
  TempFiles files;
  files.csv =
      writeTempFile(".csv",
                    "scope,transmission_mode,effect_channel,multiplier\n"
                    "venue,fomite,environmental_risk,0.50\n");
  files.yaml = writeTempFile(".yaml",
                             "policies:\n"
                             "  temporal_policies:\n"
                             "    - name: partial-cleaning\n"
                             "      compliance_rate: 0.5\n"
                             "      transmission_effects_file: " +
                                 files.csv.string() + "\n");
  PolicyManager manager(f.world);
  CHECK_THROWS_WITH(
      PolicyLoader::loadPolicies(manager, files.yaml.string(), "2020-01-01"),
      doctest::Contains("venue effects require a temporal policy"));

  TempFiles symptom_files;
  symptom_files.yaml = writeTempFile(".yaml",
                                     "policies:\n"
                                     "  symptom_policies:\n"
                                     "    - name: symptomatic-cleaning\n"
                                     "      symptoms: [cough]\n"
                                     "      transmission_effects_file: " +
                                         files.csv.string() + "\n");
  PolicyManager symptom_manager(f.world);
  CHECK_THROWS_WITH(
      PolicyLoader::loadPolicies(symptom_manager, symptom_files.yaml.string(),
                                 "2020-01-01"),
      doctest::Contains("venue effects require a temporal policy"));
}

TEST_CASE("transmission modifier effects stack by mode and channel") {
  Fixture f;
  PolicyManager manager(f.world);

  TemporalPolicy mask;
  mask.name = "mask";
  mask.action.compliance_rate = 1.0;
  manager.addTemporalPolicy(mask);
  manager.addTransmissionEffect(
      personEffect(0, "properties.role", "worker", 0.60));

  TemporalPolicy shield;
  shield.name = "shield";
  shield.action.compliance_rate = 1.0;
  manager.addTemporalPolicy(shield);
  manager.addTransmissionEffect(
      personEffect(1, "properties.role", "worker", 0.80));

  manager.precomputePolicyApplicability(f.world.people);
  manager.initializeTransmissionModifiers(f.disease, 0.0);
  CHECK(
      manager.personModifier(f.world.people[0], 0,
                             TransmissionEffectChannel::TargetSusceptibility) ==
      doctest::Approx(0.48));
  CHECK(
      manager.personModifier(f.world.people[0], 1,
                             TransmissionEffectChannel::TargetSusceptibility) ==
      doctest::Approx(1.0));
}

TEST_CASE("distinct matching rows within one policy multiply") {
  Fixture f;
  PolicyManager manager(f.world);
  TemporalPolicy policy;
  policy.name = "two-filters";
  manager.addTemporalPolicy(policy);
  manager.addTransmissionEffect(
      personEffect(0, "properties.role", "worker", 0.60));
  SelectionCriterion age;
  age.property_path = "age";
  age.operator_type = ">=";
  age.value = 18.0;
  PolicyTransmissionEffect second;
  second.policy_index = 0;
  second.scope = TransmissionEffectScope::Person;
  second.mode_name = "direct";
  second.channel = TransmissionEffectChannel::TargetSusceptibility;
  second.multiplier = 0.80;
  second.criteria.push_back(age);
  manager.addTransmissionEffect(second);

  manager.precomputePolicyApplicability(f.world.people);
  manager.initializeTransmissionModifiers(f.disease, 0.0);
  CHECK(
      manager.personModifier(f.world.people[0], 0,
                             TransmissionEffectChannel::TargetSusceptibility) ==
      doctest::Approx(0.48));
}

TEST_CASE("transmission modifier windows and compliance rebuild identity") {
  Fixture f;
  PolicyManager manager(f.world);
  manager.setBaseSeed(19);

  TemporalPolicy timed;
  timed.name = "timed";
  timed.window = ActiveWindow{2.0, 5.0};
  timed.action.compliance_rate = 1.0;
  manager.addTemporalPolicy(timed);
  manager.addTransmissionEffect(
      personEffect(0, "properties.role", "worker", 0.50));
  TemporalPolicy refuses;
  refuses.name = "refuses";
  refuses.action.compliance_rate = 0.0;
  manager.addTemporalPolicy(refuses);
  manager.addTransmissionEffect(
      personEffect(1, "properties.role", "worker", 0.10));
  manager.precomputePolicyApplicability(f.world.people);
  manager.initializeTransmissionModifiers(f.disease, 1.0);
  CHECK(
      manager.personModifier(f.world.people[0], 0,
                             TransmissionEffectChannel::TargetSusceptibility) ==
      doctest::Approx(1.0));

  std::unordered_set<PersonId> no_active_infections;
  manager.refreshTransmissionModifiers(2.0, no_active_infections);
  const auto active_id = f.world.people[0].transmission_modifier_set_id;
  const auto active_table_size = manager.transmissionModifierTable().size();
  CHECK(
      manager.personModifier(f.world.people[0], 0,
                             TransmissionEffectChannel::TargetSusceptibility) ==
      doctest::Approx(0.50));
  // Repeated slots inside one policy window do not reevaluate selectors or
  // rebuild the shared table.
  manager.refreshTransmissionModifiers(2.0, no_active_infections);
  CHECK(f.world.people[0].transmission_modifier_set_id == active_id);
  CHECK(manager.transmissionModifierTable().size() == active_table_size);
  manager.refreshTransmissionModifiers(5.0, no_active_infections);
  CHECK(
      manager.personModifier(f.world.people[0], 0,
                             TransmissionEffectChannel::TargetSusceptibility) ==
      doctest::Approx(1.0));

  // The second policy is active but refused, so its effect is absent from the
  // compiled set.
  manager.refreshTransmissionModifiers(2.0, no_active_infections);
  CHECK(
      manager.personModifier(f.world.people[0], 0,
                             TransmissionEffectChannel::TargetSusceptibility) ==
      doctest::Approx(0.50));
}
