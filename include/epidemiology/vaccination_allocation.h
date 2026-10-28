#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "utils/deterministic_rng.h"

namespace june {

// The pieces a vaccination campaign needs to spend a shared dose budget. They
// are pure functions of integers, so every rank that feeds them the same
// global counts gets the same answer, whatever the process count.

// Doses a campaign may give on its day `day` (0 is the first day), when it has
// `doses` to give every `per_days` days. The days add up exactly: over any
// whole number of periods the total is a multiple of `doses`, with no drift,
// and no two days differ by more than one dose. Doses not given on their day
// are lost, so this is a rate and not a stockpile.
inline int64_t doseAllotment(int64_t doses, int64_t per_days, int64_t day) {
  if (doses <= 0)
    throw std::runtime_error("doseAllotment: doses " + std::to_string(doses) +
                             " must be positive");
  if (per_days <= 0)
    throw std::runtime_error("doseAllotment: per_days " +
                             std::to_string(per_days) + " must be positive");
  if (day < 0)
    throw std::runtime_error("doseAllotment: day " + std::to_string(day) +
                             " must not be negative");
  // (day + 1) * doses has to fit in 64 bits for the floor below to be exact.
  if (day >= std::numeric_limits<int64_t>::max() / doses)
    throw std::runtime_error("doseAllotment: day " + std::to_string(day) +
                             " times doses " + std::to_string(doses) +
                             " overflows a 64 bit integer");
  return (day + 1) * doses / per_days - day * doses / per_days;
}

// How many people each tier takes today. `counts` is the number of candidates
// in each tier over the whole world, in priority order. `allotment` is the
// day's dose budget (none when the campaign has no limit). `starts_course`
// marks the tiers that give a first dose, and `cap_remaining` is how many more
// courses may be started (none when there is no coverage target).
//
// Tiers are served strictly in order. A tier takes as many of its candidates
// as the budget left allows and, if it starts courses, no more than the cap
// left allows. When the cap runs out only the tiers that start courses are
// cut: a later dose of a course already begun still takes budget.
inline std::vector<int64_t> fillTiers(std::optional<int64_t> allotment,
                                      const std::vector<int64_t>& counts,
                                      const std::vector<bool>& starts_course,
                                      std::optional<int64_t> cap_remaining) {
  if (starts_course.size() != counts.size())
    throw std::runtime_error(
        "fillTiers: " + std::to_string(counts.size()) + " tier counts but " +
        std::to_string(starts_course.size()) + " starts_course flags");
  if (allotment && *allotment < 0)
    throw std::runtime_error("fillTiers: allotment " +
                             std::to_string(*allotment) + " is negative");
  if (cap_remaining && *cap_remaining < 0)
    throw std::runtime_error("fillTiers: cap_remaining " +
                             std::to_string(*cap_remaining) + " is negative");
  // What is left to spend; none stays none, so "no limit" is never a number
  // that a very large count could use up.
  std::optional<int64_t> budget = allotment;
  std::optional<int64_t> cap = cap_remaining;
  std::vector<int64_t> taken(counts.size(), 0);
  for (size_t t = 0; t < counts.size(); ++t) {
    if (counts[t] < 0)
      throw std::runtime_error("fillTiers: tier " + std::to_string(t) +
                               " has a negative count " +
                               std::to_string(counts[t]));
    int64_t take = counts[t];
    if (budget) take = std::min(take, *budget);
    if (starts_course[t] && cap) take = std::min(take, *cap);
    taken[t] = take;
    if (budget) *budget -= take;
    if (starts_course[t] && cap) *cap -= take;
  }
  return taken;
}

// A person's fixed place in the lottery of one tier of one campaign. The
// lowest keys are served first.
//
// The keys of one tier are unique. With the seed, campaign and tier fixed,
// mix_seed is a bijection of the person id: the id is multiplied by an odd
// constant (invertible modulo 2^64), XORed with a constant (invertible), and
// then passed through a finalizer made of xor-shifts and odd multiplications
// (each invertible). Two people therefore never share a key, and that is what
// lets lotteryCutoff pick exactly the number of people asked for.
inline uint64_t lotteryKey(uint64_t seed, uint64_t person_id,
                           uint64_t campaign_index, uint64_t tier_index) {
  return mix_seed(seed, person_id, campaign_index, tier_index);
}

// The cutoff for serving exactly `take` candidates, over all ranks together:
// the candidates with a key at or below it are the ones served. Empty when
// `take` is 0 (nobody is served). `sorted_local_keys` are the keys of this
// rank's candidates in ascending order. `take` must be the same on every
// rank, and so must the number of calls, since `global_sum` is collective: it
// adds one integer over all ranks (the value itself when running serially).
//
// The cutoff is found by bisection over the 64 bit key space. Each round asks
// how many candidates in the whole world have a key at or below the middle of
// the range, which is one global_sum of a local upper_bound. That is one
// reduce for the total and at most 64 for the search, with no gather of keys.
// Only integers are exchanged, so every rank ends on the same cutoff.
//
// The result is exact only if keys are unique. When several keys share the
// value the cutoff lands on, serving "at or below" would take more than `take`
// people, so that is an error and not a rounding.
inline std::optional<uint64_t> lotteryCutoff(
    const std::vector<uint64_t>& sorted_local_keys, int64_t take,
    const std::function<int64_t(int64_t)>& global_sum) {
  if (!std::is_sorted(sorted_local_keys.begin(), sorted_local_keys.end()))
    throw std::runtime_error("lotteryCutoff: local keys are not sorted");
  const int64_t total =
      global_sum(static_cast<int64_t>(sorted_local_keys.size()));
  if (take < 0 || take > total)
    throw std::runtime_error("lotteryCutoff: cannot take " +
                             std::to_string(take) + " of " +
                             std::to_string(total) + " candidates");
  if (take == 0) return std::nullopt;

  // Smallest key value with at least `take` candidates at or below it.
  uint64_t lo = 0;
  uint64_t hi = std::numeric_limits<uint64_t>::max();
  int64_t count_at_hi = total;  // every key is at or below the largest value
  while (lo < hi) {
    const uint64_t mid = lo + (hi - lo) / 2;
    const int64_t count = global_sum(
        static_cast<int64_t>(std::upper_bound(sorted_local_keys.begin(),
                                              sorted_local_keys.end(), mid) -
                             sorted_local_keys.begin()));
    if (count >= take) {
      hi = mid;
      count_at_hi = count;
    } else {
      lo = mid + 1;
    }
  }
  if (count_at_hi != take)
    throw std::runtime_error("lotteryCutoff: " + std::to_string(count_at_hi) +
                             " candidates share keys at or below the cutoff " +
                             std::to_string(hi) + " but exactly " +
                             std::to_string(take) +
                             " are wanted, so the keys are not unique");
  return hi;
}

}  // namespace june
