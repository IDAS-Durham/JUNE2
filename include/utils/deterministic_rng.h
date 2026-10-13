#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace june {

// SplitMix64 finalizer: high-quality seed mixing function.
// Combines a base seed with up to 3 entity/context keys to produce
// a deterministic, well-distributed 64-bit seed.
// This ensures that (seed, person_id, timestep) always maps to the same
// output regardless of which MPI rank processes the entity.
inline constexpr uint64_t mix_seed(uint64_t base, uint64_t key1,
                                   uint64_t key2 = 0, uint64_t key3 = 0) {
  uint64_t z = base ^ (key1 * 0x9E3779B97F4A7C15ULL) ^
               (key2 * 0x6C62272E07BB0142ULL) ^ (key3 * 0x94D049BB133111EBULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  z = z ^ (z >> 31);
  return z;
}

// Portable string hash, for cases where a seed has to be derived from a name
// rather than an id. `std::hash<std::string>` cannot be used here: it is
// implementation-defined, so libc++ and libstdc++ return different values for
// the same string, and a run seeded through it is not reproducible across
// builds. This has happened: the exact-seed path hashed a geographic unit id
// this way, and the same config, seed and world picked nine entirely
// different index cases under libc++ and under libstdc++, so calibration runs
// made on the two builds were no longer comparable.
//
// FNV-1a, 64-bit, with the standard offset basis and prime. Fixed by the
// specification rather than by the standard library, so every build agrees.
inline constexpr uint64_t hash_name(const char* s, size_t n) {
  uint64_t h = 0xCBF29CE484222325ULL;
  for (size_t i = 0; i < n; ++i) {
    h ^= static_cast<uint64_t>(static_cast<unsigned char>(s[i]));
    h *= 0x100000001B3ULL;
  }
  return h;
}

inline uint64_t hash_name(const std::string& s) {
  return hash_name(s.data(), s.size());
}

// Portable draws, for the same reason as hash_name above.
//
// Measured across the two builds this project runs on, with an identically
// seeded SplitMix64: raw draws agree, `std::uniform_real_distribution` agrees,
// and `std::uniform_int_distribution` and `std::shuffle` do not. Both are
// implementation-defined and both sit in paths that decide which person is
// picked, so a run seeded through them is not reproducible across builds.
//
// bounded() is plain rejection sampling: unbiased, and specified here rather
// than by the standard library. shuffle_det() is Fisher-Yates, walking from
// the back and drawing each swap index through bounded(), so the permutation
// for a given generator is fixed by this code on every build.
//
// A shuffle only helps if what follows keeps its order. Ranking a shuffled
// range by a key that can tie needs std::stable_sort: std::sort leaves equal
// elements in an implementation-defined order, which undoes the shuffle
// differently on each standard library.
template <typename Rng>
inline uint64_t bounded(Rng& g, uint64_t n) {
  if (n <= 1) return 0;
  const uint64_t limit = std::numeric_limits<uint64_t>::max() -
                         (std::numeric_limits<uint64_t>::max() % n) - 1;
  uint64_t x;
  do {
    x = g();
  } while (x > limit);
  return x % n;
}

// Uniform double in [0, 1), from exactly one 64-bit draw. The draw is scaled by
// 2^-64, which is what the standard's generate_canonical computes for a 64-bit
// engine, so every call site that used uniform_real_distribution(0, 1) over
// SplitMix64 keeps its value. It is written out here so that the value is
// fixed by this code and not by whichever standard library the build links.
// A draw within 2^-54 of the top rounds up to 1.0 and is pulled back to the
// largest double below 1. Only engines that span the full 64 bits qualify.
template <typename Rng>
inline double uniform01(Rng& g) {
  static_assert(
      Rng::min() == 0 && Rng::max() == std::numeric_limits<uint64_t>::max(),
      "uniform01 needs a generator spanning all 64 bits");
  const double u = static_cast<double>(g()) * 0x1.0p-64;
  return u < 1.0 ? u : std::nextafter(1.0, 0.0);
}

template <typename It, typename Rng>
inline void shuffle_det(It first, It last, Rng& g) {
  const auto n = last - first;
  for (auto i = n - 1; i > 0; --i) {
    const auto j =
        static_cast<decltype(i)>(bounded(g, static_cast<uint64_t>(i) + 1));
    if (i != j) std::iter_swap(first + i, first + j);
  }
}

// Poisson count by inversion of the cumulative distribution, from exactly one
// uniform01 draw. The stream position afterwards therefore does not depend on
// the value drawn, which std::poisson_distribution does not give.
//
// The terms are built from p(0) = exp(-mean) by p(k) = p(k-1) * mean / k, and
// the result is the first k whose running sum exceeds the draw. In double
// precision the running sum stops growing a little below 1, and the largest
// uniform01 value can sit above that plateau. When the next term is too small
// to change the sum, the result is the k reached: the draw is then within about
// 1e-16 of 1, where the exact answer is in the far tail anyway.
//
// A mean above kPoissonMaxMean throws: exp(-mean) underflows near 745, and the
// terms lose accuracy well before that. The loop is also capped at
// kPoissonMaxTerms. The plateau stops it first for every allowed mean (for the
// largest draw it stops at k = 20 for mean 1.5, 46 for mean 10 and 693 for
// mean 500, against a cap of 4096), so the cap is a guard that cannot be
// reached, and it throws if it ever is.
inline constexpr double kPoissonMaxMean = 500.0;
inline constexpr int kPoissonMaxTerms = 4096;

template <typename Rng>
inline int poisson(Rng& g, double mean) {
  if (!(mean >= 0.0) || !std::isfinite(mean) || mean > kPoissonMaxMean)
    throw std::runtime_error("poisson: mean " + std::to_string(mean) +
                             " is outside [0, " +
                             std::to_string(kPoissonMaxMean) + "]");
  const double u = uniform01(g);
  double term = std::exp(-mean);
  double sum = term;
  for (int k = 0; k < kPoissonMaxTerms; ++k) {
    if (u < sum) return k;
    term *= mean / (k + 1);
    if (sum + term == sum) return k;  // plateau: the sum cannot grow further
    sum += term;
  }
  throw std::runtime_error("poisson: no result within " +
                           std::to_string(kPoissonMaxTerms) +
                           " terms for mean " + std::to_string(mean));
}

// Binomial count as n independent uniform01 draws below p, so it consumes
// exactly n draws and is exact. It is O(n); n here is a person's friend count,
// at most a few tens.
template <typename Rng>
inline int binomial(Rng& g, int n, double p) {
  if (n < 0)
    throw std::runtime_error("binomial: n " + std::to_string(n) +
                             " is negative");
  if (!(p >= 0.0 && p <= 1.0))
    throw std::runtime_error("binomial: p " + std::to_string(p) +
                             " is outside [0, 1]");
  int count = 0;
  for (int i = 0; i < n; ++i)
    if (uniform01(g) < p) ++count;
  return count;
}

// Standard normal by the Box-Muller transform, from exactly two uniform01
// draws. The transform also yields a second, independent variate (the sine
// term); it is thrown away. Keeping it for the next call would halve the
// draws, but it would tie a call to whatever the previous call on the same
// generator did, so a result would depend on call order and on hidden state.
// Throwing it away keeps every call a pure function of its two draws. The log
// argument is 1 - u1, which lies in (0, 1], so a draw of exactly 0 is safe.
template <typename Rng>
inline double normal01(Rng& g) {
  constexpr double kTwoPi = 6.283185307179586476925286766559;
  const double u1 = uniform01(g);
  const double u2 = uniform01(g);
  return std::sqrt(-2.0 * std::log(1.0 - u1)) * std::cos(kTwoPi * u2);
}

// Lognormal in the scipy convention the disease configs use: the value is
// loc + scale * exp(s * z) with z standard normal, so `scale` is the median of
// the part above loc and `s` is the standard deviation of the log. Exactly two
// draws.
template <typename Rng>
inline double lognormal(Rng& g, double loc, double scale, double s) {
  if (!(scale > 0.0) || !std::isfinite(scale) || !(s >= 0.0) ||
      !std::isfinite(s) || !std::isfinite(loc))
    throw std::runtime_error("lognormal: loc " + std::to_string(loc) +
                             ", scale " + std::to_string(scale) + ", s " +
                             std::to_string(s) +
                             " must be finite with scale > 0 and s >= 0");
  return loc + scale * std::exp(s * normal01(g));
}

// Gamma by Marsaglia and Tsang (2000), shape > 0, with the given scale.
//
// The number of draws is variable. Each try draws a normal (two uniforms) and
// one uniform to accept or reject, about 3 draws, and the chance a try is
// rejected is under 5%. That is acceptable here because every infection owns
// its stream and uses it in a fixed order: a variable count only moves that
// infection's later draws, in a way fixed by its seed and parameters, and
// never depends on other people, on rank count or on container order.
//
// For a shape below 1 the method is applied at shape + 1 and the result is
// multiplied by U^(1/shape), where U is one extra uniform drawn first.
//
// The loop is capped at kGammaMaxTries. The chance of one try being rejected is
// below 0.05, so the chance of reaching the cap is below 1e-1300 and it cannot
// be reached; it throws if it ever is.
inline constexpr int kGammaMaxTries = 1000;

template <typename Rng>
inline double gamma(Rng& g, double shape, double scale) {
  if (!(shape > 0.0) || !std::isfinite(shape) || !(scale > 0.0) ||
      !std::isfinite(scale))
    throw std::runtime_error("gamma: shape " + std::to_string(shape) +
                             " and scale " + std::to_string(scale) +
                             " must be finite and positive");
  if (shape < 1.0) {
    const double boost = uniform01(g);
    return gamma(g, shape + 1.0, scale) * std::pow(boost, 1.0 / shape);
  }
  const double d = shape - 1.0 / 3.0;
  const double c = 1.0 / std::sqrt(9.0 * d);
  for (int attempt = 0; attempt < kGammaMaxTries; ++attempt) {
    const double x = normal01(g);
    double v = 1.0 + c * x;
    if (v <= 0.0) continue;
    v = v * v * v;
    const double u = uniform01(g);
    // Quick acceptance, then the exact test.
    if (u < 1.0 - 0.0331 * x * x * x * x) return d * v * scale;
    if (std::log(u) < 0.5 * x * x + d * (1.0 - v + std::log(v)))
      return d * v * scale;
  }
  throw std::runtime_error("gamma: no draw accepted in " +
                           std::to_string(kGammaMaxTries) +
                           " tries for shape " + std::to_string(shape));
}

// Beta from two gammas, x / (x + y). Variable draws, as for gamma. If both
// gammas come out as zero the ratio is undefined, and that is an error, not a
// number.
template <typename Rng>
inline double beta(Rng& g, double a, double b) {
  const double x = gamma(g, a, 1.0);
  const double y = gamma(g, b, 1.0);
  if (x + y == 0.0)
    throw std::runtime_error("beta: both gamma draws are zero for shapes a " +
                             std::to_string(a) + " and b " + std::to_string(b));
  return x / (x + y);
}

// SplitMix64: lightweight RNG satisfying C++ UniformRandomBitGenerator.
// 8 bytes state, ~1 ns/draw, excellent statistical quality.
// Designed for per-entity ephemeral use; construct from mix_seed() output.
class SplitMix64 {
 public:
  using result_type = uint64_t;

  explicit SplitMix64(uint64_t seed = 0) : state_(seed) {}

  static constexpr result_type min() { return 0; }
  static constexpr result_type max() {
    return std::numeric_limits<uint64_t>::max();
  }

  result_type operator()() {
    state_ += 0x9E3779B97F4A7C15ULL;
    uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }

  // Allow seeding after construction
  void seed(uint64_t s) { state_ = s; }

 private:
  uint64_t state_;
};

}  // namespace june
