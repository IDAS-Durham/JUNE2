#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include "doctest.h"
#include "epidemiology/infection_seed.h"
#include "test_utils.h"
#include "utils/run_dir.h"

using namespace june;

namespace {
bool contains(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

}  // namespace

TEST_CASE("collectConfigPaths - uses loader-recorded nested data CSVs") {
  Config config;
  config.simulation.referenced_paths = {"configs/config_2021/rates.csv",
                                        "data/bulk_seeds.csv"};

  auto paths = run_dir::collectConfigPaths(config, "sim.yaml");

  CHECK(contains(paths, "configs/config_2021/rates.csv"));
  CHECK(contains(paths, "data/bulk_seeds.csv"));

  // No duplicates: the de-dup set in collectConfigPaths must hold.
  std::vector<std::string> sorted = paths;
  std::sort(sorted.begin(), sorted.end());
  CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
}

TEST_CASE("bulk seed loader records CSV for the run snapshot") {
  ScopedTestFiles files{"june_bulk_seed_snapshot"};
  const auto csv_path = files.write("bulk.csv",
                                    "name,date,type,geo_level,geo_unit,cases\n"
                                    "test,2020-02-01 08:00,exact,MGU,U1,2\n");
  const auto seed_path =
      files.write("seeds.yaml", "bulk_csv: \"" + csv_path.string() + "\"\n");
  const auto sim_path = files.write("simulation.yaml", "config_paths: {}\n");

  Config config;
  config.simulation.infection_seeds_file = seed_path.string();
  const InfectionSeedConfig loaded = InfectionSeedConfigLoader::loadFromFile(
      seed_path.string(), &config.simulation.referenced_paths);
  REQUIRE(loaded.seeds.size() == 1);
  REQUIRE(config.simulation.referenced_paths.size() == 1);
  CHECK(config.simulation.referenced_paths.front() == csv_path.string());

  const std::filesystem::path run_path = seed_path.parent_path() / "run";
  std::filesystem::create_directories(run_path);
  run_dir::snapshotRun(run_path, sim_path.string(), config, 1,
                       {"--config", sim_path.string()}, 123, "world.h5");

  const auto copied_csv = run_path / "configs" / "bulk.csv";
  REQUIRE(std::filesystem::exists(copied_csv));
  CHECK(std::filesystem::file_size(copied_csv) ==
        std::filesystem::file_size(csv_path));

  const YAML::Node manifest =
      YAML::LoadFile((run_path / "manifest.yaml").string());
  bool csv_in_manifest = false;
  for (const auto& file : manifest["files"]) {
    if (file["original"].as<std::string>() == csv_path.string()) {
      csv_in_manifest = true;
      CHECK(file["snapshot"].as<std::string>() == "configs/bulk.csv");
    }
  }
  CHECK(csv_in_manifest);
}
