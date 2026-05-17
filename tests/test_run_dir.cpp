#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

#include "doctest.h"
#include "utils/run_dir.h"

using namespace june;
namespace fs = std::filesystem;

namespace {

// Unique scratch dir per fixture so resolution-relative-to-YAML can be
// asserted without touching tracked configs/.
fs::path makeTempDir() {
  static std::atomic<unsigned> counter{0};
  fs::path d = fs::temp_directory_path() /
               ("june_run_dir_test_" + std::to_string(::getpid()) + "_" +
                std::to_string(counter.fetch_add(1)));
  fs::remove_all(d);
  fs::create_directories(d);
  return d;
}

void writeFile(const fs::path& p, const std::string& content) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p);
  f << content;
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

}  // namespace

TEST_CASE("nestedDiseaseOutcomeCsv - resolves relative to the disease YAML") {
  fs::path tmp = makeTempDir();

  SUBCASE("scalar form, path relative to the YAML directory") {
    // disease_loader.cpp resolves outcome_rates_csv against the YAML's own
    // directory (yaml_dir + rel), NOT the process CWD.
    fs::path yaml = tmp / "cfg" / "sub" / "disease.yaml";
    writeFile(yaml,
              "disease:\n"
              "  name: plague\n"
              "  outcome_rates_csv: ../../data/rates.csv\n");

    std::string got = run_dir::nestedDiseaseOutcomeCsv(yaml.string());
    REQUIRE(!got.empty());
    CHECK(fs::path(got).lexically_normal() ==
          (tmp / "data" / "rates.csv").lexically_normal());
  }

  SUBCASE("mapping form with a 'file:' key") {
    fs::path yaml = tmp / "cfg" / "disease.yaml";
    writeFile(yaml,
              "disease:\n"
              "  outcome_rates_csv:\n"
              "    file: rates2.csv\n");

    std::string got = run_dir::nestedDiseaseOutcomeCsv(yaml.string());
    CHECK(fs::path(got).lexically_normal() ==
          (tmp / "cfg" / "rates2.csv").lexically_normal());
  }

  SUBCASE("absent key yields empty (nothing to snapshot)") {
    fs::path yaml = tmp / "disease.yaml";
    writeFile(yaml, "disease:\n  name: plague\n");
    CHECK(run_dir::nestedDiseaseOutcomeCsv(yaml.string()).empty());
  }

  SUBCASE("missing disease section yields empty") {
    fs::path yaml = tmp / "disease.yaml";
    writeFile(yaml, "not_disease:\n  foo: bar\n");
    CHECK(run_dir::nestedDiseaseOutcomeCsv(yaml.string()).empty());
  }

  SUBCASE("empty path argument yields empty") {
    CHECK(run_dir::nestedDiseaseOutcomeCsv("").empty());
  }

  SUBCASE("unparseable YAML is left for the authoritative loader") {
    fs::path yaml = tmp / "broken.yaml";
    writeFile(yaml, "disease: [unterminated\n");
    CHECK(run_dir::nestedDiseaseOutcomeCsv(yaml.string()).empty());
  }
}

TEST_CASE("nestedBulkSeedCsv - verbatim, NOT prefixed with the YAML dir") {
  fs::path tmp = makeTempDir();

  SUBCASE("path is returned exactly as written (CWD-relative)") {
    // infection_seed.cpp passes bulk_csv straight to the CSV reader, so it is
    // CWD-relative. Prefixing it with the YAML directory would be a bug — this
    // is the regression guard for the two differing conventions.
    fs::path yaml = tmp / "deep" / "infection_seeds.yaml";
    writeFile(yaml, "bulk_csv: data/seeds.csv\n");

    CHECK(run_dir::nestedBulkSeedCsv(yaml.string()) == "data/seeds.csv");
  }

  SUBCASE("absent key yields empty") {
    fs::path yaml = tmp / "infection_seeds.yaml";
    writeFile(yaml, "global_parameters:\n  base_cases_per_capita: 1.0\n");
    CHECK(run_dir::nestedBulkSeedCsv(yaml.string()).empty());
  }

  SUBCASE("empty path argument yields empty") {
    CHECK(run_dir::nestedBulkSeedCsv("").empty());
  }
}

TEST_CASE("collectConfigPaths - regional risk is gated on enabled") {
  Config config;
  config.simulation.regional_risk.regional_risk_file = "data/rr.csv";
  const std::string sim_yaml = "configs/x/simulation.yaml";

  SUBCASE("disabled: the CSV is NOT snapshotted") {
    config.simulation.regional_risk.enabled = false;
    auto paths = run_dir::collectConfigPaths(config, sim_yaml);
    CHECK_FALSE(contains(paths, "data/rr.csv"));
  }

  SUBCASE("enabled: the CSV IS snapshotted") {
    config.simulation.regional_risk.enabled = true;
    auto paths = run_dir::collectConfigPaths(config, sim_yaml);
    CHECK(contains(paths, "data/rr.csv"));
  }

  SUBCASE("the anchoring simulation.yaml is always first") {
    auto paths = run_dir::collectConfigPaths(config, sim_yaml);
    REQUIRE(!paths.empty());
    CHECK(paths.front() == sim_yaml);
  }
}

TEST_CASE("collectConfigPaths - nested data CSVs are collected") {
  fs::path tmp = makeTempDir();

  fs::path disease_yaml = tmp / "cfg" / "disease.yaml";
  writeFile(disease_yaml,
            "disease:\n"
            "  outcome_rates_csv: rates.csv\n");

  fs::path seeds_yaml = tmp / "cfg" / "infection_seeds.yaml";
  writeFile(seeds_yaml, "bulk_csv: data/bulk_seeds.csv\n");

  Config config;
  config.simulation.disease_file = disease_yaml.string();
  config.simulation.infection_seeds_file = seeds_yaml.string();

  auto paths = run_dir::collectConfigPaths(config, "sim.yaml");

  // Outcome CSV resolved relative to the disease YAML's directory.
  bool has_outcome =
      std::any_of(paths.begin(), paths.end(), [&](const auto& p) {
        return fs::path(p).lexically_normal() ==
               (tmp / "cfg" / "rates.csv").lexically_normal();
      });
  CHECK(has_outcome);

  // Bulk-seed CSV kept verbatim (CWD-relative).
  CHECK(contains(paths, "data/bulk_seeds.csv"));

  // No duplicates: the de-dup set in collectConfigPaths must hold.
  std::vector<std::string> sorted = paths;
  std::sort(sorted.begin(), sorted.end());
  CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
}
