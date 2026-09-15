#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <atomic>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include "core/world_state.h"
#include "doctest.h"
#include "loaders/disease_loader.h"
#include "test_utils.h"
#include "utils/filtering.h"

using namespace june;
namespace fs = std::filesystem;

namespace {

struct WideDiseaseFixture {
  fs::path directory;
  fs::path yaml;

  explicit WideDiseaseFixture(const std::string& rates) {
    static std::atomic<unsigned> counter{0};
    directory =
        fs::temp_directory_path() / ("june_disease_loader_wide_test_" +
                                     std::to_string(counter.fetch_add(1)));
    fs::create_directories(directory);

    std::ofstream csv(directory / "rates.csv");
    csv << "gp_mild_male,gp_severe_male,gp_mild_female,gp_severe_female,"
           "ch_mild_male,ch_severe_male,ch_mild_female,ch_severe_female\n"
        << rates << "\n";

    yaml = directory / "disease.yaml";
    std::ofstream config(yaml);
    config << R"(disease:
  name: wide_test
  outcome_rates_csv:
    file: rates.csv
    populations:
      gp:
        name: general_population
      ch:
        name: care_home
    sexes:
      - male
      - female
    outcomes:
      mild:
        symptom_tag: mild
      severe:
        symptom_tag: severe
)";
  }

  ~WideDiseaseFixture() { fs::remove_all(directory); }
};

}  // namespace

TEST_CASE("DiseaseLoader loads a valid wide outcome table") {
  WideDiseaseFixture fixture("0.7,0.3,0.7,0.3,0.7,0.3,0.7,0.3");

  CHECK_NOTHROW(DiseaseLoader::loadFromYAML(fixture.yaml.string()));
}

TEST_CASE("DiseaseLoader names the malformed wide outcome group") {
  WideDiseaseFixture fixture("0.7,0.3,0.7,0.3,0.7,0.3,1.0,0.3");

  CHECK_THROWS_WITH_AS(DiseaseLoader::loadFromYAML(fixture.yaml.string()),
                       doctest::Contains("ch/female"), std::runtime_error);
}

TEST_CASE("DiseaseLoader keeps negative-rate validation for wide tables") {
  WideDiseaseFixture fixture("-0.1,1.1,0.7,0.3,0.7,0.3,0.7,0.3");

  CHECK_THROWS_WITH_AS(DiseaseLoader::loadFromYAML(fixture.yaml.string()),
                       doctest::Contains("gp_mild_male"), std::runtime_error);
}

TEST_CASE("1911 outcome table uses the current filter format") {
  Disease disease =
      DiseaseLoader::loadFromYAML("configs/config_1911/disease.yaml");

  CHECK(disease.getOutcomeRates().rows.size() == 40);
  for (const auto& row : disease.getOutcomeRates().rows) {
    double sum = 0.0;
    for (const auto& [name, probability] : row.probabilities) {
      sum += probability;
    }
    CHECK(sum == doctest::Approx(1.0));
  }
}

TEST_CASE("Open-ended age filters use a numeric comparison") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);
  auto criteria = filtering::parseCriterionFromKeyValue("age", ">=90");
  REQUIRE(criteria.size() == 1);
  criteria[0].resolve(world);

  world.people[0].age = 90.0f;
  CHECK(criteria[0].evaluate(world.people[0], &world));
  world.people[0].age = 89.9f;
  CHECK_FALSE(criteria[0].evaluate(world.people[0], &world));
}
