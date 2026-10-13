#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "activity/coordinated_encounter_types.h"
#include "doctest.h"
#include "utils/deterministic_rng.h"

using namespace june;

namespace {

// A synthetic day: `hosts` hosts, each with a few records whose encounter_ids
// are arbitrary distinct values, as the real ones are for one host.
std::vector<CoordinatedEncounter> syntheticDay(int hosts, uint64_t seed) {
  SplitMix64 rng(seed);
  std::vector<CoordinatedEncounter> out;
  for (int h = 0; h < hosts; ++h) {
    const PersonId host = static_cast<PersonId>(1000 + 7 * h);
    const int records = 1 + static_cast<int>(bounded(rng, 5));
    std::set<int> used;
    for (int r = 0; r < records; ++r) {
      int eid;
      do {
        eid = static_cast<int>(rng() & 0x7FFFFFFF);
      } while (!used.insert(eid).second);
      CoordinatedEncounter e;
      e.encounter_id = eid;
      e.host_id = host;
      out.push_back(e);
    }
  }
  return out;
}

}  // namespace

TEST_CASE("group_id round trips through day, ordinal and host_id") {
  for (const auto& t : std::vector<std::tuple<int, int, int64_t>>{
           {0, 0, 0},
           {1, 2, 3},
           {4095, 32767, (int64_t{1} << 36) - 1},
           {2047, 1, 123456789}}) {
    const auto [day, ordinal, host] = t;
    const uint64_t id = packEncounterGroupId(day, ordinal, host);
    CHECK(id < (uint64_t{1} << 63));
    const auto parts = unpackEncounterGroupId(id);
    CHECK(parts.day == day);
    CHECK(parts.ordinal == static_cast<uint32_t>(ordinal));
    CHECK(parts.host_id == static_cast<uint64_t>(host));
  }
}

TEST_CASE("group_id is unique across the records of a day") {
  const auto day = syntheticDay(500, 1);
  const auto ids = assignEncounterGroupIds(day, 12);
  REQUIRE(ids.size() == day.size());
  CHECK(std::set<uint64_t>(ids.begin(), ids.end()).size() == ids.size());
  for (size_t i = 0; i < ids.size(); ++i) {
    const auto parts = unpackEncounterGroupId(ids[i]);
    CHECK(parts.day == 12);
    CHECK(parts.host_id == static_cast<uint64_t>(day[i].host_id));
  }
}

TEST_CASE("group_id is the same at any rank count and arrival order") {
  const auto all = syntheticDay(400, 2);
  const auto reference = assignEncounterGroupIds(all, 7);
  std::map<std::pair<PersonId, int>, uint64_t> by_record;
  for (size_t i = 0; i < all.size(); ++i)
    by_record[{all[i].host_id, all[i].encounter_id}] = reference[i];

  for (int ranks : {1, 2, 3, 5}) {
    SplitMix64 rng(static_cast<uint64_t>(ranks));
    // A host's records all live on the host's rank, in any arrival order.
    std::vector<std::vector<CoordinatedEncounter>> per_rank(ranks);
    for (const auto& e : all)
      per_rank[static_cast<size_t>(e.host_id) % ranks].push_back(e);
    for (auto& records : per_rank) {
      shuffle_det(records.begin(), records.end(), rng);
      const auto ids = assignEncounterGroupIds(records, 7);
      for (size_t i = 0; i < records.size(); ++i)
        CHECK(ids[i] ==
              by_record.at({records[i].host_id, records[i].encounter_id}));
    }
  }
}

TEST_CASE("group_id rejects a day, ordinal or host_id that does not fit") {
  CHECK_NOTHROW(packEncounterGroupId(4095, 32767, (int64_t{1} << 36) - 1));
  CHECK_THROWS_WITH_AS(packEncounterGroupId(4096, 0, 0),
                       doctest::Contains("day 4096 does not fit in 12 bits"),
                       std::runtime_error);
  CHECK_THROWS_WITH_AS(packEncounterGroupId(0, 32768, 0),
                       doctest::Contains("ordinal 32768 does not fit in 15"),
                       std::runtime_error);
  CHECK_THROWS_WITH_AS(
      packEncounterGroupId(0, 0, int64_t{1} << 36),
      doctest::Contains("host_id 68719476736 does not fit in 36"),
      std::runtime_error);
  CHECK_THROWS_AS(packEncounterGroupId(-1, 0, 0), std::runtime_error);
  CHECK_THROWS_AS(packEncounterGroupId(0, 0, -5), std::runtime_error);

  // The ordinal limit is reached through the assignment too: one host with
  // more records in a day than the field can number.
  std::vector<CoordinatedEncounter> one_host(32769);
  for (size_t i = 0; i < one_host.size(); ++i) {
    one_host[i].host_id = 5;
    one_host[i].encounter_id = static_cast<int>(i);
  }
  CHECK_THROWS_AS(assignEncounterGroupIds(one_host, 0), std::runtime_error);
}
