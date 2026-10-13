#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>
#include <vector>

#include "../include/utils/deterministic_rng.h"
#include "doctest.h"

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

TEST_CASE("mix_seed initializes SplitMix64 deterministically") {
  SplitMix64 rng1(mix_seed(42, 100, 200));
  SplitMix64 rng2(mix_seed(42, 100, 200));
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
  SplitMix64 rng_r0_p10(mix_seed(base, 10, 0));
  SplitMix64 rng_r0_p20(mix_seed(base, 20, 0));
  double val_r0_p10 =
      std::uniform_real_distribution<double>(0.0, 1.0)(rng_r0_p10);
  double val_r0_p20 =
      std::uniform_real_distribution<double>(0.0, 1.0)(rng_r0_p20);

  // Rank 1 processes person 20 first, then person 10
  SplitMix64 rng_r1_p20(mix_seed(base, 20, 0));
  SplitMix64 rng_r1_p10(mix_seed(base, 10, 0));
  double val_r1_p20 =
      std::uniform_real_distribution<double>(0.0, 1.0)(rng_r1_p20);
  double val_r1_p10 =
      std::uniform_real_distribution<double>(0.0, 1.0)(rng_r1_p10);

  // Same person gets same value regardless of processing order
  CHECK(val_r0_p10 == val_r1_p10);
  CHECK(val_r0_p20 == val_r1_p20);
}

namespace {
// Returns a fixed raw value forever, to probe the ends of the 64-bit range.
struct ConstantBits {
  using result_type = uint64_t;
  uint64_t value;
  static constexpr uint64_t min() { return 0; }
  static constexpr uint64_t max() { return UINT64_MAX; }
  uint64_t operator()() { return value; }
};
}  // namespace

TEST_CASE("uniform01 equals uniform_real_distribution over SplitMix64") {
  SplitMix64 a(mix_seed(42, 700));
  SplitMix64 b(mix_seed(42, 700));
  std::uniform_real_distribution<double> dist(0.0, 1.0);
  for (int i = 0; i < 1000000; ++i) {
    const double got = uniform01(a);
    REQUIRE(got == dist(b));
    REQUIRE(got >= 0.0);
    REQUIRE(got < 1.0);
  }
}

TEST_CASE("uniform01 stays below one at the top of the range") {
  ConstantBits top{UINT64_MAX};
  CHECK(uniform01(top) == std::nextafter(1.0, 0.0));
  ConstantBits zero{0};
  CHECK(uniform01(zero) == 0.0);
  ConstantBits half{1ULL << 63};
  CHECK(uniform01(half) == 0.5);
}

TEST_CASE("uniform01 golden values for a fixed seed") {
  // Raw SplitMix64 output is fixed by the algorithm, so these are exact on
  // every build. A different value here means the mapping to double changed.
  SplitMix64 g(0);
  CHECK(uniform01(g) == 0x1.c4415072f63bap-1);
  CHECK(uniform01(g) == 0x1.b9e279aa86e59p-2);
  CHECK(uniform01(g) == 0x1.b117462002515p-6);
}

// ---------------------------------------------------------------------------
// poisson and binomial
// ---------------------------------------------------------------------------

namespace {

// Raw bits that uniform01 maps back to exactly u (u is a double below 1).
uint64_t bitsFor(double u) { return static_cast<uint64_t>(u * 0x1.0p64); }

// Plays back a list of uniform01 values and counts how many were consumed.
struct ReplayBits {
  using result_type = uint64_t;
  std::vector<uint64_t> values;
  size_t used = 0;
  static constexpr uint64_t min() { return 0; }
  static constexpr uint64_t max() { return UINT64_MAX; }
  uint64_t operator()() { return values.at(used++); }
};

int poissonAt(double mean, double u) {
  ReplayBits g{{bitsFor(u)}};
  return poisson(g, mean);
}

constexpr double kLargestUniform01 = 1.0 - 0x1.0p-53;

double poissonCdf(double mean, int k) {
  double term = std::exp(-mean), sum = term;
  for (int i = 1; i <= k; ++i) {
    term *= mean / i;
    sum += term;
  }
  return sum;
}

}  // namespace

TEST_CASE("poisson golden values, away from any boundary") {
  // The cumulative sums for mean 1.5 are 0.2231, 0.5578, 0.8088, 0.9344,
  // 0.9814. Each draw below sits at least 0.015 from a boundary, so a one ulp
  // difference in exp cannot change the answer on any build. Expected values
  // come from a 60 digit evaluation of the cumulative sums.
  CHECK(poissonAt(1.5, 0.0) == 0);
  CHECK(poissonAt(1.5, 0.40) == 1);
  CHECK(poissonAt(1.5, 0.70) == 2);
  CHECK(poissonAt(1.5, 0.88) == 3);
  CHECK(poissonAt(1.5, 0.96) == 4);
  CHECK(poissonAt(10.0, 0.5) == 10);
  CHECK(poissonAt(10.0, 0.9) == 14);  // cumulative 0.8645 at 13, 0.9165 at 14
  CHECK(poissonAt(0.0, 0.0) == 0);
  CHECK(poissonAt(0.0, kLargestUniform01) == 0);
}

TEST_CASE("poisson at the largest uniform01 stops on the plateau") {
  // The running sum stalls just below 1, and the largest draw can sit above
  // it. The result is then the k reached, not an error and not a hang.
  for (double mean : {0.5, 1.5, 10.0, 100.0, kPoissonMaxMean}) {
    const int k = poissonAt(mean, kLargestUniform01);
    CHECK(k >= mean);
    CHECK(k < kPoissonMaxTerms);
    CHECK(poissonCdf(mean, k) > 1.0 - 1e-12);
  }
  CHECK(poissonAt(1.5, kLargestUniform01) >= 18);
  CHECK(poissonAt(1.5, kLargestUniform01) <= 24);
}

TEST_CASE("poisson consumes exactly one draw, whatever the value") {
  for (double mean : {0.0, 0.1, 1.5, 50.0}) {
    SplitMix64 a(mix_seed(5, 1)), b(mix_seed(5, 1));
    (void)poisson(a, mean);
    (void)b();
    CHECK(a() == b());  // the next draw is the same for every mean
  }
}

TEST_CASE("poisson rejects a mean it cannot handle") {
  SplitMix64 g(1);
  CHECK_THROWS_WITH_AS(poisson(g, -0.5), doctest::Contains("-0.5"),
                       std::runtime_error);
  CHECK_THROWS_AS(poisson(g, std::numeric_limits<double>::quiet_NaN()),
                  std::runtime_error);
  CHECK_THROWS_AS(poisson(g, std::numeric_limits<double>::infinity()),
                  std::runtime_error);
  CHECK_THROWS_WITH_AS(poisson(g, 500.5), doctest::Contains("500.5"),
                       std::runtime_error);
  CHECK_NOTHROW(poisson(g, kPoissonMaxMean));
}

TEST_CASE("poisson mean and variance match the parameter") {
  const int n = 200000;
  for (double mean : {0.3, 1.5, 10.0, 80.0}) {
    SplitMix64 g(mix_seed(77, static_cast<uint64_t>(mean * 10)));
    double sum = 0, sum_sq = 0;
    for (int i = 0; i < n; ++i) {
      const double v = poisson(g, mean);
      sum += v;
      sum_sq += v * v;
    }
    const double m = sum / n, var = sum_sq / n - m * m;
    // Four standard errors; the variance of the sample variance of a
    // Poisson is (mean + 2 mean^2) / n.
    CHECK(std::abs(m - mean) < 4.0 * std::sqrt(mean / n));
    CHECK(std::abs(var - mean) < 4.0 * std::sqrt((mean + 2 * mean * mean) / n));
  }
}

TEST_CASE("poisson histogram matches the exact distribution") {
  const int n = 400000;
  const double mean = 1.5;
  std::vector<double> count(10, 0.0);
  SplitMix64 g(mix_seed(78, 1));
  for (int i = 0; i < n; ++i) count[std::min(poisson(g, mean), 9)] += 1;
  for (int k = 0; k < 10; ++k) {
    const double p =
        k < 9 ? poissonCdf(mean, k) - (k ? poissonCdf(mean, k - 1) : 0)
              : 1.0 - poissonCdf(mean, 8);
    const double expected = n * p, sd = std::sqrt(n * p * (1 - p));
    CHECK(std::abs(count[k] - expected) < 4.5 * sd);
  }
}

TEST_CASE("binomial golden values and draw count") {
  // u: 0.1 0.5 0.29 0.9 against p = 0.3 gives two successes in four draws.
  ReplayBits g{{bitsFor(0.1), bitsFor(0.5), bitsFor(0.29), bitsFor(0.9)}};
  CHECK(binomial(g, 4, 0.3) == 2);
  CHECK(g.used == 4);

  ReplayBits zeros{std::vector<uint64_t>(5, 0)};
  CHECK(binomial(zeros, 5, 0.3) == 5);  // u = 0 is below any p > 0
  CHECK(zeros.used == 5);
  ReplayBits zeros_p0{std::vector<uint64_t>(5, 0)};
  CHECK(binomial(zeros_p0, 5, 0.0) == 0);  // but never below p = 0

  ReplayBits tops{std::vector<uint64_t>(5, UINT64_MAX)};
  CHECK(binomial(tops, 5, 0.3) == 0);
  ReplayBits tops_p1{std::vector<uint64_t>(5, UINT64_MAX)};
  CHECK(binomial(tops_p1, 5, 1.0) == 5);  // the largest draw is still below 1

  ReplayBits none{{}};
  CHECK(binomial(none, 0, 0.5) == 0);
  CHECK(none.used == 0);
}

TEST_CASE("binomial rejects n or p it cannot handle") {
  SplitMix64 g(1);
  CHECK_THROWS_WITH_AS(binomial(g, -1, 0.5), doctest::Contains("-1"),
                       std::runtime_error);
  CHECK_THROWS_AS(binomial(g, 3, -0.1), std::runtime_error);
  CHECK_THROWS_AS(binomial(g, 3, 1.1), std::runtime_error);
  CHECK_THROWS_AS(binomial(g, 3, std::numeric_limits<double>::quiet_NaN()),
                  std::runtime_error);
}

TEST_CASE("binomial mean and histogram match the exact distribution") {
  const int trials = 200000, n = 9;
  const double p = 0.3;
  std::vector<double> count(n + 1, 0.0);
  SplitMix64 g(mix_seed(79, 1));
  double sum = 0;
  for (int i = 0; i < trials; ++i) {
    const int v = binomial(g, n, p);
    count[v] += 1;
    sum += v;
  }
  CHECK(std::abs(sum / trials - n * p) <
        4.0 * std::sqrt(n * p * (1 - p) / trials));
  double choose = 1;
  for (int k = 0; k <= n; ++k) {
    if (k > 0) choose = choose * (n - k + 1) / k;
    const double pk = choose * std::pow(p, k) * std::pow(1 - p, n - k);
    const double expected = trials * pk, sd = std::sqrt(trials * pk * (1 - pk));
    CHECK(std::abs(count[k] - expected) < 4.5 * sd);
  }
}
