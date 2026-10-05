#include <algorithm>
#include <string>

#include "doctest.h"
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
