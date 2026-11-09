
#include <filesystem>
#include <fstream>
#include <sstream>
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
  ScopedTestFiles files{"june_disease_loader_wide_test"};
  fs::path yaml;

  explicit WideDiseaseFixture(const std::string& rates) {
    files.write(
        "rates.csv",
        "gp_mild_male,gp_severe_male,gp_mild_female,gp_severe_female,"
        "ch_mild_male,ch_severe_male,ch_mild_female,ch_severe_female\n" +
            rates + "\n");

    yaml = files.write("disease.yaml", R"(disease:
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
)");
  }
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

namespace {

// The fomite test disease with its max_age line replaced (empty = removed).
std::string loadFomiteDiseaseWithMaxAge(const ScopedTestFiles& files,
                                        const std::string& max_age_line) {
  std::ifstream in("tests/configs/fomite_disease.yaml");
  std::stringstream buffer;
  buffer << in.rdbuf();
  std::string text = buffer.str();
  const std::string rates = "../../data/infection_outcome_rates_covid19.csv";
  text.replace(text.find(rates), rates.size(), "rates.csv");
  std::ifstream csv_in("data/infection_outcome_rates_covid19.csv");
  std::stringstream csv;
  csv << csv_in.rdbuf();
  files.write("rates.csv", csv.str());
  const std::string old_line = "      max_age: 7.0\n";
  text.replace(text.find(old_line), old_line.size(),
               max_age_line.empty() ? "" : "      " + max_age_line + "\n");
  return files.write("disease.yaml", text).string();
}

}  // namespace

TEST_CASE("A fomite mode without max_age is rejected and named") {
  ScopedTestFiles files{"june_disease_loader_fomite_max_age_missing"};
  const std::string yaml = loadFomiteDiseaseWithMaxAge(files, "");
  CHECK_THROWS_WITH_AS(DiseaseLoader::loadFromYAML(yaml),
                       doctest::Contains("fomite mode 'fomite' needs max_age"),
                       std::runtime_error);
}

TEST_CASE("A fomite mode with a non-positive max_age is rejected") {
  for (const char* value : {"max_age: 0.0", "max_age: -3.0"}) {
    ScopedTestFiles files{"june_disease_loader_fomite_max_age_bad"};
    const std::string yaml = loadFomiteDiseaseWithMaxAge(files, value);
    CHECK_THROWS_WITH_AS(DiseaseLoader::loadFromYAML(yaml),
                         doctest::Contains("max_age must be greater than 0"),
                         std::runtime_error);
  }
}

TEST_CASE("A fomite mode with a positive max_age loads") {
  ScopedTestFiles files{"june_disease_loader_fomite_max_age_ok"};
  const std::string yaml = loadFomiteDiseaseWithMaxAge(files, "max_age: 7.0");
  CHECK_NOTHROW(DiseaseLoader::loadFromYAML(yaml));
}

TEST_CASE("A disease with no fomite mode needs no max_age") {
  WideDiseaseFixture fixture("0.7,0.3,0.7,0.3,0.7,0.3,0.7,0.3");
  CHECK_NOTHROW(DiseaseLoader::loadFromYAML(fixture.yaml.string()));
}
