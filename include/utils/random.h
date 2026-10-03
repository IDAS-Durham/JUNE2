#pragma once

#include <algorithm>
#include <numeric>
#include <random>
#include <vector>

namespace june {

// Build cumulative weights into `out` (cleared first). Returns the total
// weight (= out.back() if any weights were positive).
inline double buildCumulative(const std::vector<double>& weights,
                              std::vector<double>& out) {
  out.clear();
  out.resize(weights.size());
  std::partial_sum(weights.begin(), weights.end(), out.begin());
  return out.empty() ? 0.0 : out.back();
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

}  // namespace june
