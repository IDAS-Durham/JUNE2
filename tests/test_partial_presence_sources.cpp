#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <vector>

#include "core/types.h"
#include "doctest.h"
#include "epidemiology/transmission/partial_presence_sources.h"
#include "utils/deterministic_rng.h"

using namespace june;

TEST_CASE("no accumulated sources samples an absent mode and no infector") {
  std::vector<PartialPresenceAccumSource> sources;
  SplitMix64 rng(1);

  const auto [transmission_mode_index, infector_id] =
      sampleInfectorFromAccumSources(sources, {}, rng);

  CHECK(transmission_mode_index == kNoModeIndex);
  CHECK(infector_id == -1);
}

TEST_CASE("a lone source is sampled with its own mode and infector") {
  std::vector<PartialPresenceAccumSource> sources{
      {/*mode=*/1, /*infector=*/42, /*weighted=*/0.5}};
  SplitMix64 rng(1);

  const auto [transmission_mode_index, infector_id] =
      sampleInfectorFromAccumSources(sources, {}, rng);

  CHECK(transmission_mode_index == 1);
  CHECK(infector_id == 42);
}

TEST_CASE("a mode with zero target modifier is never sampled") {
  std::vector<PartialPresenceAccumSource> sources{
      {/*mode=*/0, /*infector=*/7, /*weighted=*/10.0},
      {/*mode=*/1, /*infector=*/42, /*weighted=*/0.5}};
  SplitMix64 rng(1);

  const auto [transmission_mode_index, infector_id] =
      sampleInfectorFromAccumSources(sources, {0.0, 1.0}, rng);

  CHECK(transmission_mode_index == 1);
  CHECK(infector_id == 42);
}
