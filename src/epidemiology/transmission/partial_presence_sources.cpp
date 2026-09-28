#include "epidemiology/transmission/partial_presence_sources.h"

#include <algorithm>

#include "utils/random.h"

namespace june {

std::pair<uint8_t, PersonId> sampleInfectorFromAccumSources(
    std::vector<PartialPresenceAccumSource>& sources, SplitMix64& rng) {
  if (sources.empty()) return {kNoModeIndex, -1};

  std::sort(sources.begin(), sources.end(),
            [](const PartialPresenceAccumSource& a,
               const PartialPresenceAccumSource& b) {
              if (a.mode != b.mode) return a.mode < b.mode;
              return a.infector < b.infector;
            });
  std::vector<double> cumulative;
  cumulative.reserve(sources.size());
  double total = 0.0;
  for (const auto& source : sources) {
    total += source.weighted;
    cumulative.push_back(total);
  }
  int sampled = (total > 0.0) ? sampleFromCumulative(cumulative, rng) : 0;
  if (sampled < 0) sampled = 0;
  if (sampled >= static_cast<int>(sources.size())) return {kNoModeIndex, -1};
  return {static_cast<uint8_t>(sources[sampled].mode),
          sources[sampled].infector};
}

}  // namespace june
