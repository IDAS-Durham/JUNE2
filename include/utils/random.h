#pragma once

#include <algorithm>
#include <random>
#include <vector>

namespace june {

// Build cumulative weights into `out` (cleared first). Returns the total
// weight (= out.back() if any weights were positive).
inline double buildCumulative(const std::vector<double>& weights,
                              std::vector<double>& out) {
  out.clear();
  out.reserve(weights.size());
  double total = 0.0;
  for (double w : weights) {
    total += w;
    out.push_back(total);
  }
  return total;
}

// Sample an index in [0, cumulative.size()) using the given RNG.
// Returns -1 if cumulative is empty or has zero total weight.
// This is a drop-in replacement for std::discrete_distribution's
// operator() that avoids reconstructing the distribution per call:
// cumulative is built once via buildCumulative() and sampled many times.
template <typename RNG>
inline int sampleFromCumulative(const std::vector<double>& cumulative,
                                RNG& rng) {
  if (cumulative.empty()) return -1;
  double total = cumulative.back();
  if (!(total > 0.0)) return -1;
  std::uniform_real_distribution<double> u(0.0, total);
  double r = u(rng);
  auto it = std::lower_bound(cumulative.begin(), cumulative.end(), r);
  int idx = static_cast<int>(it - cumulative.begin());
  if (idx >= static_cast<int>(cumulative.size()))
    idx = static_cast<int>(cumulative.size()) - 1;
  return idx;
}

// Global random number generator. DEPRECATED for simulation hot paths.
// Use SplitMix64 from deterministic_rng.h with mix_seed() for MPI-reproducible
// per-entity random draws. GlobalRNG remains only for backward-compatible
// initialization and components not yet migrated.
class GlobalRNG {
 public:
  // Get the global RNG instance
  // DEPRECATED: Use SplitMix64(mix_seed(base_seed, entity_id, ...)) instead
  // for MPI-reproducible simulation code.
  [[deprecated(
      "Use SplitMix64 from deterministic_rng.h with mix_seed() for "
      "MPI-reproducible RNG")]]
  static std::mt19937& get() {
    return instance();
  }

  // Seed the global RNG (call once at simulation start)
  // This remains non-deprecated as it's used for initialization.
  static void seed(unsigned int seed) { instance().seed(seed); }

  // Prevent copying
  GlobalRNG(const GlobalRNG&) = delete;
  GlobalRNG& operator=(const GlobalRNG&) = delete;

 private:
  GlobalRNG() = default;

  static std::mt19937& instance() {
    static std::mt19937 rng;
    return rng;
  }
};

}  // namespace june
