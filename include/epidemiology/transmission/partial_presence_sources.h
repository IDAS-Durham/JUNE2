#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "core/types.h"
#include "utils/deterministic_rng.h"

namespace june {

// One infector's weighted share of a susceptible's partial-presence λ, for one
// Transmission Mode.
struct PartialPresenceAccumSource {
  int mode;
  PersonId infector;
  double weighted;
};

// Weight-samples one (mode, infector) from a susceptible's accumulated sources.
// Sorts in place by (mode, infector) for deterministic order, weights each
// source by its mode's target modifier (1.0 past the end), then draws once
// with the given RNG. An empty list has no source to sample, so the mode is
// absent (kNoModeIndex) and the infector -1, as on the venue path.
std::pair<uint8_t, PersonId> sampleInfectorFromAccumSources(
    std::vector<PartialPresenceAccumSource>& sources,
    const std::vector<double>& target_modifiers, SplitMix64& rng);

}  // namespace june
