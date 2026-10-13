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

// ---------------------------------------------------------------------------
// normal01, lognormal, gamma, beta
// ---------------------------------------------------------------------------

namespace {

// Draws from the given uniform values, in order.
ReplayBits replay(std::initializer_list<double> us) {
  ReplayBits g;
  for (double u : us) g.values.push_back(bitsFor(u));
  return g;
}

// A generator that counts the draws taken from it.
struct CountingRng {
  using result_type = uint64_t;
  SplitMix64 inner;
  uint64_t draws = 0;
  explicit CountingRng(uint64_t seed) : inner(seed) {}
  static constexpr uint64_t min() { return 0; }
  static constexpr uint64_t max() { return UINT64_MAX; }
  uint64_t operator()() {
    ++draws;
    return inner();
  }
};

constexpr double kRel = 1e-12;
#define CHECK_REL(value, expected) \
  CHECK((value) == doctest::Approx(expected).epsilon(kRel).scale(0.0))

double normalCdf(double q) {
  return 0.5 * (1.0 + std::erf(q / std::sqrt(2.0)));
}

// Regularised lower incomplete gamma P(a, x) by its series.
double gammaCdf(double a, double x) {
  if (x <= 0) return 0.0;
  double term = 1.0 / a, sum = term;
  for (int n = 1; n < 2000; ++n) {
    term *= x / (a + n);
    sum += term;
    if (term < 1e-18 * sum) break;
  }
  return sum * std::exp(-x + a * std::log(x) - std::lgamma(a));
}

// Regularised incomplete beta I_x(a, b) by Simpson's rule on the density.
double betaCdf(double a, double b, double x) {
  const int n = 40000;  // even
  const double h = x / n;
  auto pdf = [&](double t) {
    return t <= 0 ? 0.0
                  : std::exp((a - 1) * std::log(t) + (b - 1) * std::log1p(-t));
  };
  double sum = pdf(0) + pdf(x);
  for (int i = 1; i < n; ++i) sum += pdf(i * h) * (i % 2 ? 4 : 2);
  const double norm =
      std::exp(std::lgamma(a + b) - std::lgamma(a) - std::lgamma(b));
  return sum * h / 3.0 * norm;
}

struct Moments {
  double mean, var, m4;
  size_t n;
};

template <typename Draw>
Moments sample(uint64_t seed, size_t n, Draw draw,
               std::vector<double>* keep = nullptr) {
  SplitMix64 g(seed);
  std::vector<double> v(n);
  for (auto& x : v) x = draw(g);
  double mean = 0;
  for (double x : v) mean += x;
  mean /= n;
  double var = 0, m4 = 0;
  for (double x : v) {
    const double d = x - mean;
    var += d * d;
    m4 += d * d * d * d;
  }
  var /= n;
  m4 /= n;
  if (keep) *keep = std::move(v);
  return {mean, var, m4, n};
}

// Sample mean and variance within four standard errors of the theory. The
// standard error of the variance uses the sample's own fourth moment.
void checkMoments(const Moments& m, double mean, double var) {
  const double se_mean = std::sqrt(var / m.n);
  const double se_var = std::sqrt((m.m4 - m.var * m.var) / m.n);
  CHECK(std::abs(m.mean - mean) < 4.0 * se_mean);
  CHECK(std::abs(m.var - var) < 4.0 * se_var);
}

// Fraction of the sample at or below q against the exact CDF at q.
void checkCdf(const std::vector<double>& v, double q, double exact) {
  size_t below = 0;
  for (double x : v) below += (x <= q);
  // A quantile in a saturated tail carries no information; fail rather than
  // pass vacuously.
  REQUIRE(exact > 1e-6);
  REQUIRE(exact < 1.0 - 1e-6);
  const double n = static_cast<double>(v.size());
  const double sd = std::sqrt(exact * (1 - exact) / n);
  CHECK(std::abs(below / n - exact) < 4.5 * sd);
}

}  // namespace

TEST_CASE("normal01 golden values from an independent implementation") {
  // Reference values from a separate implementation of the transform.
  auto z = [](std::initializer_list<double> us) {
    ReplayBits g = replay(us);
    const double v = normal01(g);
    CHECK(g.used == 2);
    return v;
  };
  CHECK_REL(z({0.5, 0.0}), 1.1774100225154747);
  CHECK(std::abs(z({0.0, 0.25})) < 1e-300);  // u1 = 0 gives a zero radius
  CHECK_REL(z({0.3, 0.7}), -0.26099588660468637);
  CHECK_REL(z({0.9, 0.1}), 1.736122984619357);
  // The largest uniform01 gives the largest radius, about 8.57.
  CHECK_REL(z({kLargestUniform01, 0.5}), -8.571674348652905);
}

TEST_CASE("lognormal golden values and draw count") {
  ReplayBits a = replay({0.5, 0.0});
  CHECK_REL(lognormal(a, 1.0, 2.0, 0.5), 4.603307565393083);
  CHECK(a.used == 2);
  ReplayBits b = replay({0.3, 0.7});
  CHECK_REL(lognormal(b, 1.0, 2.0, 0.5), 2.755316596049295);
  ReplayBits c = replay({kLargestUniform01, 0.5});
  CHECK_REL(lognormal(c, 1.0, 2.0, 0.5), 1.027524190848648);
}

TEST_CASE("gamma golden values on each acceptance path") {
  // Shape 2.5, scale 1.5. The first sequence is taken by the quick squeeze,
  // the second by the exact log test, the third rejects once and then accepts.
  ReplayBits sq = replay({0.323833, 0.150849, 0.650934});
  CHECK_REL(gamma(sq, 2.5, 1.5), 4.528070969454632);
  CHECK(sq.used == 3);
  ReplayBits lg = replay({0.984729, 0.149463, 0.724156});
  CHECK_REL(gamma(lg, 2.5, 1.5), 8.666804054658025);
  CHECK(lg.used == 3);
  ReplayBits rj =
      replay({0.963999, 0.503737, 0.851377, 0.618276, 0.030981, 0.412921});
  CHECK_REL(gamma(rj, 2.5, 1.5), 7.278712546222226);
  CHECK(rj.used == 6);  // a rejected try costs three draws
  // x = 0 gives v = 1 and the result d * scale exactly.
  ReplayBits mid = replay({0.0, 0.25, 0.3});
  CHECK_REL(gamma(mid, 2.5, 1.5), 3.25);
  // Shape 1.
  ReplayBits one = replay({0.41, 0.62, 0.55});
  CHECK_REL(gamma(one, 1.0, 2.0), 0.4462267578096749);
}

TEST_CASE("gamma below shape 1 takes one extra draw first") {
  ReplayBits g = replay({0.37, 0.41, 0.62, 0.55});
  CHECK_REL(gamma(g, 0.5, 1.0), 0.07260461551697564);
  CHECK(g.used == 4);  // one boost uniform, then three for the shape 1.5 try
  ReplayBits zero = replay({0.0, 0.5, 0.5, 0.5});
  CHECK(gamma(zero, 0.5, 1.0) == 0.0);  // a boost draw of 0 gives 0
}

TEST_CASE("beta golden value and the all zero case") {
  ReplayBits g = replay({0.4, 0.6, 0.5, 0.35, 0.8, 0.3, 0.2, 0.9});
  CHECK_REL(beta(g, 1.21, 1.97), 0.13322781265826503);
  CHECK(g.used == 8 - 2);  // 3 draws for each gamma
  // Two boosted gammas that both come out as 0 have no defined ratio.
  ReplayBits z = replay({0.0, 0.5, 0.5, 0.5, 0.0, 0.5, 0.5, 0.5});
  CHECK_THROWS_WITH_AS(beta(z, 0.5, 0.25), doctest::Contains("a 0.5"),
                       std::runtime_error);
}

TEST_CASE(
    "normal01 and lognormal take a fixed two draws, gamma a variable number") {
  CountingRng n(11), l(12);
  for (int i = 0; i < 1000; ++i) (void)normal01(n);
  CHECK(n.draws == 2000);
  for (int i = 0; i < 1000; ++i) (void)lognormal(l, 0.0, 1.0, 0.5);
  CHECK(l.draws == 2000);

  // Gamma at shape 2.5: three draws a try, a few percent of tries rejected.
  CountingRng g(13);
  const int calls = 20000;
  for (int i = 0; i < calls; ++i) (void)gamma(g, 2.5, 1.0);
  const double per_call = static_cast<double>(g.draws) / calls;
  CHECK(per_call >= 3.0);
  CHECK(per_call < 3.3);
  // Below shape 1 the boost adds exactly one draw to a call at shape + 1.
  CountingRng a(14), b(14);
  for (int i = 0; i < calls; ++i) {
    (void)gamma(a, 0.5, 1.0);
  }
  for (int i = 0; i < calls; ++i) (void)gamma(b, 1.5, 1.0);
  CHECK(a.draws > b.draws);  // the extra uniform per call
}

TEST_CASE("normal01 matches the standard normal distribution") {
  std::vector<double> v;
  const auto m =
      sample(101, 400000, [](SplitMix64& g) { return normal01(g); }, &v);
  checkMoments(m, 0.0, 1.0);
  for (double q : {-2.0, -1.0, -0.3, 0.0, 0.7, 1.64, 2.5})
    checkCdf(v, q, normalCdf(q));
}

TEST_CASE("lognormal matches its mean, variance and distribution function") {
  const double loc = 0.5, scale = 2.0, s = 0.4;
  const double m1 = scale * std::exp(s * s / 2);
  const double var = scale * scale * (std::exp(s * s) - 1) * std::exp(s * s);
  std::vector<double> v;
  const auto m = sample(
      102, 400000, [&](SplitMix64& g) { return lognormal(g, loc, scale, s); },
      &v);
  checkMoments(m, loc + m1, var);
  for (double q : {1.0, 1.7, 2.5, 3.2, 5.0})
    checkCdf(v, loc + q, normalCdf(std::log(q / scale) / s));
}

TEST_CASE("gamma matches its mean, variance and distribution function") {
  for (double shape : {0.5, 1.0, 2.5, 8.0}) {
    const double scale = 1.5;
    std::vector<double> v;
    const auto m = sample(
        103 + static_cast<uint64_t>(shape * 10), 400000,
        [&](SplitMix64& g) { return gamma(g, shape, scale); }, &v);
    checkMoments(m, shape * scale, shape * scale * scale);
    for (double frac : {0.25, 0.75, 1.0, 1.6})
      checkCdf(v, frac * shape * scale, gammaCdf(shape, frac * shape));
  }
}

TEST_CASE("beta matches its mean, variance and distribution function") {
  for (auto [a, b] : std::vector<std::pair<double, double>>{
           {1.21, 1.97}, {2.29, 19.05}, {0.6, 0.9}}) {
    std::vector<double> v;
    const auto m = sample(
        104 + static_cast<uint64_t>(a * 100), 400000,
        [&](SplitMix64& g) { return beta(g, a, b); }, &v);
    const double mean = a / (a + b);
    checkMoments(m, mean, a * b / ((a + b) * (a + b) * (a + b + 1)));
    // Quantiles placed relative to the mean, so none is in a saturated tail.
    for (double k : {0.3, 0.7, 1.0, 1.5, 2.2})
      checkCdf(v, std::min(0.99, k * mean),
               betaCdf(a, b, std::min(0.99, k * mean)));
  }
}

TEST_CASE("gamma and lognormal reject parameters they cannot handle") {
  SplitMix64 g(1);
  CHECK_THROWS_WITH_AS(gamma(g, 0.0, 1.0), doctest::Contains("shape 0.0"),
                       std::runtime_error);
  CHECK_THROWS_AS(gamma(g, -1.0, 1.0), std::runtime_error);
  CHECK_THROWS_AS(gamma(g, 2.0, 0.0), std::runtime_error);
  CHECK_THROWS_AS(gamma(g, std::numeric_limits<double>::quiet_NaN(), 1.0),
                  std::runtime_error);
  CHECK_THROWS_AS(lognormal(g, 0.0, 0.0, 0.5), std::runtime_error);
  CHECK_THROWS_AS(lognormal(g, 0.0, 1.0, -0.1), std::runtime_error);
}

TEST_CASE("normal01 depends on nothing but its generator") {
  // Two calls from one generator equal the same two calls from a fresh copy:
  // no variate is carried between calls.
  SplitMix64 a(mix_seed(9, 9)), b(mix_seed(9, 9));
  const double a1 = normal01(a), a2 = normal01(a);
  const double b1 = normal01(b);
  SplitMix64 c(mix_seed(9, 9));
  (void)c();
  (void)c();  // skip the first call's two draws
  CHECK(a1 == b1);
  CHECK(a2 == normal01(c));
}
