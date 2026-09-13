#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
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

template <typename It, typename Rng>
inline void shuffle_det(It first, It last, Rng& g) {
  const auto n = last - first;
  for (auto i = n - 1; i > 0; --i) {
    const auto j =
        static_cast<decltype(i)>(bounded(g, static_cast<uint64_t>(i) + 1));
    if (i != j) std::iter_swap(first + i, first + j);
  }
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

// Convenience: create a ready-to-use RNG from entity context.
// Usage: auto rng = make_rng(base_seed, person_id, timestep_bits);
inline SplitMix64 make_rng(uint64_t base, uint64_t key1, uint64_t key2 = 0,
                           uint64_t key3 = 0) {
  return SplitMix64(mix_seed(base, key1, key2, key3));
}

}  // namespace june
