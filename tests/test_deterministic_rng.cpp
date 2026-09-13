#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <algorithm>
#include <random>
#include <set>
#include <vector>

#include "../include/utils/deterministic_rng.h"

using namespace june;

TEST_CASE("mix_seed: determinism") {
  CHECK(mix_seed(42, 1, 2, 3) == mix_seed(42, 1, 2, 3));
  CHECK(mix_seed(0, 0, 0, 0) == mix_seed(0, 0, 0, 0));
  CHECK(mix_seed(UINT64_MAX, UINT64_MAX) == mix_seed(UINT64_MAX, UINT64_MAX));
}

TEST_CASE("mix_seed: avalanche — different inputs produce different outputs") {
  // Changing any single key should change the output
  uint64_t base = mix_seed(42, 100, 200, 300);
  CHECK(base != mix_seed(43, 100, 200, 300));  // different base
  CHECK(base != mix_seed(42, 101, 200, 300));  // different key1
  CHECK(base != mix_seed(42, 100, 201, 300));  // different key2
  CHECK(base != mix_seed(42, 100, 200, 301));  // different key3

  // Sequential person IDs should produce distinct seeds
  std::set<uint64_t> seeds;
  for (uint64_t pid = 0; pid < 1000; ++pid) {
    seeds.insert(mix_seed(42, pid, 0));
  }
  CHECK(seeds.size() == 1000);
}

TEST_CASE("SplitMix64: same seed produces identical sequence") {
  SplitMix64 a(12345);
  SplitMix64 b(12345);
  for (int i = 0; i < 1000; ++i) {
    CHECK(a() == b());
  }
}

TEST_CASE("SplitMix64: different seeds produce different sequences") {
  SplitMix64 a(12345);
  SplitMix64 b(12346);
  int matches = 0;
  for (int i = 0; i < 100; ++i) {
    if (a() == b()) matches++;
  }
  CHECK(matches < 5);  // statistically ~0 matches expected
}

TEST_CASE("SplitMix64: works with std::uniform_real_distribution") {
  SplitMix64 rng(mix_seed(42, 100, 0));
  std::uniform_real_distribution<double> dist(0.0, 1.0);

  for (int i = 0; i < 1000; ++i) {
    double val = dist(rng);
    CHECK(val >= 0.0);
    CHECK(val < 1.0);
  }

  // Verify determinism: same seed produces same sequence of doubles
  SplitMix64 rng2(mix_seed(42, 100, 0));
  SplitMix64 rng3(mix_seed(42, 100, 0));
  std::uniform_real_distribution<double> dist2(0.0, 1.0);
  std::uniform_real_distribution<double> dist3(0.0, 1.0);
  for (int i = 0; i < 100; ++i) {
    CHECK(dist2(rng2) == dist3(rng3));
  }
}

TEST_CASE("SplitMix64: works with std::uniform_int_distribution") {
  SplitMix64 rng(mix_seed(42, 200));
  std::uniform_int_distribution<int> dist(0, 99);

  for (int i = 0; i < 1000; ++i) {
    int val = dist(rng);
    CHECK(val >= 0);
    CHECK(val <= 99);
  }
}

TEST_CASE("SplitMix64: works with std::discrete_distribution") {
  SplitMix64 rng(mix_seed(42, 300));
  std::vector<double> weights = {1.0, 2.0, 3.0, 4.0};
  std::discrete_distribution<int> dist(weights.begin(), weights.end());

  std::vector<int> counts(4, 0);
  for (int i = 0; i < 10000; ++i) {
    counts[dist(rng)]++;
  }

  // Weight 3 (index 3, weight 4.0) should be most common
  CHECK(counts[3] > counts[0]);
  CHECK(counts[3] > counts[1]);

  // Verify determinism
  SplitMix64 rng2(mix_seed(42, 300));
  std::discrete_distribution<int> dist2(weights.begin(), weights.end());
  SplitMix64 rng3(mix_seed(42, 300));
  std::discrete_distribution<int> dist3(weights.begin(), weights.end());
  for (int i = 0; i < 100; ++i) {
    CHECK(dist2(rng2) == dist3(rng3));
  }
}

TEST_CASE("SplitMix64: works with std::normal_distribution") {
  SplitMix64 rng(mix_seed(42, 400));
  std::normal_distribution<double> dist(0.0, 1.0);

  double sum = 0;
  int n = 10000;
  for (int i = 0; i < n; ++i) {
    sum += dist(rng);
  }
  double mean = sum / n;
  CHECK(mean > -0.1);
  CHECK(mean < 0.1);
}

TEST_CASE("SplitMix64: works with std::shuffle") {
  std::vector<int> a = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  std::vector<int> b = a;

  SplitMix64 rng1(mix_seed(42, 500));
  SplitMix64 rng2(mix_seed(42, 500));

  std::shuffle(a.begin(), a.end(), rng1);
  std::shuffle(b.begin(), b.end(), rng2);

  CHECK(a == b);  // Same seed → same shuffle
}

TEST_CASE("Portable bounded draws stay within bounds") {
  SplitMix64 rng(mix_seed(42, 600));
  for (uint64_t n : {0ULL, 1ULL, 2ULL, 3ULL, 17ULL, 1000ULL}) {
    for (int i = 0; i < 1000; ++i) {
      CHECK(bounded(rng, n) < (n == 0 ? 1 : n));
    }
  }
}

TEST_CASE("Portable shuffle is repeatable") {
  std::vector<int> a = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  std::vector<int> b = a;
  SplitMix64 rng1(mix_seed(42, 601));
  SplitMix64 rng2(mix_seed(42, 601));

  shuffle_det(a.begin(), a.end(), rng1);
  shuffle_det(b.begin(), b.end(), rng2);

  CHECK(a == b);
  std::sort(a.begin(), a.end());
  CHECK(a == std::vector<int>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9}));
}

// The values below are fixed by the algorithms, not by the standard library,
// so every build must produce exactly these. They were worked out
// independently of this code. Linux (libstdc++) and macOS (libc++) CI both
// running these is the check that the two builds agree.

TEST_CASE("hash_name matches the published FNV-1a 64-bit values") {
  CHECK(hash_name(std::string("")) == 0xCBF29CE484222325ULL);
  CHECK(hash_name(std::string("a")) == 0xAF63DC4C8601EC8CULL);
  CHECK(hash_name(std::string("foobar")) == 0x85944171F73967E8ULL);
  CHECK(hash_name(std::string("E02001234")) == 0x36D2A217FB8894B2ULL);
}

TEST_CASE("bounded draws a fixed sequence, rejections included") {
  // n = 2^63 + 1 rejects almost half of all raw draws, so this sequence also
  // pins down how many raw draws each rejection consumes.
  const uint64_t half = (1ULL << 63) + 1;
  const std::vector<uint64_t> ns = {1,    2,    3,    7,    10,   1000,
                                    half, half, half, half, half, half,
                                    half, half, 6,    6};
  const std::vector<uint64_t> expected = {0,
                                          0,
                                          0,
                                          5,
                                          0,
                                          363,
                                          6217189988962137646ULL,
                                          2262534019502804546ULL,
                                          7959005890829367068ULL,
                                          8850488307750713623ULL,
                                          3405751836678233477ULL,
                                          7014104804809742358ULL,
                                          7224149396417083062ULL,
                                          6938261716188683755ULL,
                                          3,
                                          2};
  SplitMix64 rng(12345);
  std::vector<uint64_t> drawn;
  for (uint64_t n : ns) drawn.push_back(bounded(rng, n));
  CHECK(drawn == expected);
}

TEST_CASE("shuffle_det produces a fixed permutation") {
  std::vector<int> v(40);
  for (int i = 0; i < 40; ++i) v[i] = i;
  SplitMix64 rng(mix_seed(42, 601));
  shuffle_det(v.begin(), v.end(), rng);
  CHECK(v == std::vector<int>({24, 14, 19, 30, 27, 10, 39, 9,  5,  35,
                               12, 37, 34, 17, 7,  29, 3,  21, 36, 25,
                               2,  15, 28, 18, 31, 22, 0,  11, 23, 8,
                               20, 33, 1,  4,  38, 32, 13, 26, 6,  16}));
}

TEST_CASE("make_rng convenience function") {
  auto rng1 = make_rng(42, 100, 200);
  auto rng2 = make_rng(42, 100, 200);
  for (int i = 0; i < 100; ++i) {
    CHECK(rng1() == rng2());
  }
}

TEST_CASE("SplitMix64: min and max satisfy UniformRandomBitGenerator") {
  CHECK(SplitMix64::min() == 0);
  CHECK(SplitMix64::max() == std::numeric_limits<uint64_t>::max());
  CHECK(SplitMix64::min() < SplitMix64::max());
}

TEST_CASE("SplitMix64: seed() resets state") {
  SplitMix64 rng(42);
  auto v1 = rng();
  auto v2 = rng();

  rng.seed(42);
  CHECK(rng() == v1);
  CHECK(rng() == v2);
}

TEST_CASE("Per-entity RNG independence: order of creation does not matter") {
  // Simulate two ranks processing people in different order
  uint64_t base = 42;

  // Rank 0 processes person 10, then person 20
  auto rng_r0_p10 = make_rng(base, 10, 0);
  auto rng_r0_p20 = make_rng(base, 20, 0);
  double val_r0_p10 =
      std::uniform_real_distribution<double>(0.0, 1.0)(rng_r0_p10);
  double val_r0_p20 =
      std::uniform_real_distribution<double>(0.0, 1.0)(rng_r0_p20);

  // Rank 1 processes person 20 first, then person 10
  auto rng_r1_p20 = make_rng(base, 20, 0);
  auto rng_r1_p10 = make_rng(base, 10, 0);
  double val_r1_p20 =
      std::uniform_real_distribution<double>(0.0, 1.0)(rng_r1_p20);
  double val_r1_p10 =
      std::uniform_real_distribution<double>(0.0, 1.0)(rng_r1_p10);

  // Same person gets same value regardless of processing order
  CHECK(val_r0_p10 == val_r1_p10);
  CHECK(val_r0_p20 == val_r1_p20);
}
