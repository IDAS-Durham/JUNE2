#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <filesystem>

#include "core/world_state.h"
#include "doctest.h"
#include "epidemiology/vaccine.h"
#include "loaders/config_loader.h"

using namespace june;

TEST_CASE("ConfigLoader - Vaccination Config from YAML") {
  VaccinationConfig config =
      ConfigLoader::loadVaccination("tests/configs/vaccines.yaml");

  SUBCASE("Pfizer vaccine is loaded correctly") {
    REQUIRE(config.vaccines.count("Pfizer") > 0);
    const auto& pfizer = config.vaccines.at("Pfizer");

    REQUIRE(pfizer.doses.size() == 3);

    // First dose: 52% infection efficacy for covid19
    REQUIRE(pfizer.doses[0].infection_efficacy.count("covid19") > 0);
    REQUIRE(!pfizer.doses[0].infection_efficacy.at("covid19").empty());
    CHECK(pfizer.doses[0].infection_efficacy.at("covid19")[0].efficacy ==
          doctest::Approx(0.52));
    CHECK(pfizer.doses[0].infection_efficacy.at("covid19")[0].min_age == 0);
    CHECK(pfizer.doses[0].infection_efficacy.at("covid19")[0].max_age == 100);

    // Second dose: 95% infection efficacy
    CHECK(pfizer.doses[1].infection_efficacy.at("covid19")[0].efficacy ==
          doctest::Approx(0.95));

    // Third dose (booster): 98% infection efficacy
    CHECK(pfizer.doses[2].infection_efficacy.at("covid19")[0].efficacy ==
          doctest::Approx(0.98));
  }

  SUBCASE("AstraZeneca vaccine is loaded correctly") {
    REQUIRE(config.vaccines.count("AstraZeneca") > 0);
    const auto& az = config.vaccines.at("AstraZeneca");

    REQUIRE(az.doses.size() == 3);
    CHECK(az.doses[0].infection_efficacy.at("covid19")[0].efficacy ==
          doctest::Approx(0.32));
    CHECK(az.doses[1].infection_efficacy.at("covid19")[0].efficacy ==
          doctest::Approx(0.75));
  }

  SUBCASE("Campaigns are loaded correctly") {
    REQUIRE(config.campaigns.size() == 3);

    CHECK(config.campaigns[0].vaccine_type == "Pfizer");
    CHECK(config.campaigns[0].daily_coverage == doctest::Approx(0.3));
    REQUIRE(config.campaigns[0].selection_criteria.size() == 1);
    CHECK(config.campaigns[0].selection_criteria[0].property_path ==
          "activities.residence.venue_type");

    CHECK(config.campaigns[1].vaccine_type == "AstraZeneca");

    // Booster campaign
    CHECK(config.campaigns[2].dose_sequence.size() == 1);
    CHECK(config.campaigns[2].dose_sequence[0] == 2);
  }
}

TEST_CASE("ConfigLoader - Contact Matrix Config from YAML") {
  ContactMatrixConfig cm =
      ConfigLoader::loadContactMatrices("tests/configs/contact_matrices.yaml");

  SUBCASE("default_contacts is loaded") {
    CHECK(cm.default_contacts == doctest::Approx(2.0));
  }

  SUBCASE("Venue matrices are loaded") {
    REQUIRE(cm.matrices.count("household") > 0);
    const auto& hh = cm.matrices.at("household");
    CHECK(hh.bins.size() == 1);
    CHECK(hh.bins[0] == "residents");
  }

  SUBCASE("Resolve builds fast lookup by venue ID") {
    WorldState world;
    world.venue_type_names = {"office", "household", "pub"};
    world.buildIndices();
    cm.resolve(world);

    uint8_t hh_id = world.getVenueTypeIndex("household");
    const ContactMatrix* mat = cm.getMatrix(hh_id);
    REQUIRE(mat != nullptr);
    CHECK(mat->bins[0] == "residents");

    uint8_t office_id = world.getVenueTypeIndex("office");
    REQUIRE(cm.getMatrix(office_id) != nullptr);

    uint8_t pub_id = world.getVenueTypeIndex("pub");
    REQUIRE(cm.getMatrix(pub_id) == nullptr);
  }
}

TEST_CASE("ContactMatrix beta scales contacts at load time") {
  ContactMatrixConfig cm =
      ConfigLoader::loadContactMatrices("tests/configs/contact_matrices.yaml");

  SUBCASE("single-mode venue with beta=3.0 triples contacts") {
    REQUIRE(cm.matrices.count("household") > 0);
    const auto& household_matrix = cm.matrices.at("household");
    // Raw YAML value is 2.5; beta=3.0 → effective = 7.5
    CHECK(household_matrix.contacts[0][0] == doctest::Approx(7.5));
  }

  SUBCASE("single-mode venue with no beta leaves contacts unchanged") {
    REQUIRE(cm.matrices.count("office") > 0);
    const auto& office_matrix = cm.matrices.at("office");
    CHECK(office_matrix.contacts[0][0] == doctest::Approx(5.0));
  }

  SUBCASE("multi-mode venue: respiratory beta=2.0 doubles contacts") {
    REQUIRE(cm.mode_matrices.count("hospital") > 0);
    REQUIRE(cm.mode_matrices.at("hospital").count("respiratory") > 0);
    const auto& respiratory_matrix =
        cm.mode_matrices.at("hospital").at("respiratory");
    // Raw YAML value is 1.5; beta=2.0 → effective = 3.0
    CHECK(respiratory_matrix.contacts[0][0] == doctest::Approx(3.0));
  }

  SUBCASE("multi-mode venue: mode with no beta leaves contacts unchanged") {
    REQUIRE(cm.mode_matrices.count("hospital") > 0);
    REQUIRE(cm.mode_matrices.at("hospital").count("physical_contact") > 0);
    const auto& physical_contact_matrix =
        cm.mode_matrices.at("hospital").at("physical_contact");
    CHECK(physical_contact_matrix.contacts[0][0] == doctest::Approx(0.5));
  }

  SUBCASE("default_contacts_matrix beta scales contacts") {
    REQUIRE(cm.default_matrix.has_value());
    // respiratory mode: raw=0.004, beta=1.5 → effective=0.006
    CHECK(cm.default_matrix->contacts[0][0] == doctest::Approx(0.006));
  }
}

TEST_CASE("ConfigLoader - Optional Files Handling") {
  SUBCASE("Missing vaccines file is optional and disables the module") {
    // Vaccines is the only truly-optional sub-config: a missing file means
    // "don't run this module", which is fine — the rest of the simulator
    // can proceed.
    VaccinationConfig vc =
        ConfigLoader::loadVaccination("non_existent_vaccines.yaml");
    CHECK(vc.enabled == false);
  }

  SUBCASE("Missing coordinated_encounters file is a misconfiguration") {
    // simulation.yaml's coordinated_encounters_file points at this YAML;
    // absence is a bug, not a silent opt-out.
    CHECK_THROWS_AS(
        ConfigLoader::loadCoordinatedEncounters("non_existent_ce.yaml"),
        std::runtime_error);
  }

  SUBCASE("Corrupted coordinated_encounters file must throw") {
    // Required field missing should fail loudly; previously the loader
    // wrapped this in a try/catch and disabled the feature with a cerr
    // warning, which masked real bugs in production runs.
    std::string bad_file = "tmp_bad_ce.yaml";
    {
      std::ofstream f(bad_file);
      f << "coordinated_encounters:\n  encounters:\n    - name: bad\n      "
           "network: missing_fields\n";
    }

    CHECK_THROWS_AS(ConfigLoader::loadCoordinatedEncounters(bad_file),
                    std::runtime_error);

    std::filesystem::remove(bad_file);
  }
}
