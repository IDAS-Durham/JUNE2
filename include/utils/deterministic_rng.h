#pragma once

#include <cstdint>
#include <limits>

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
