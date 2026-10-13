#pragma once

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/types.h"

namespace june {

// =============================================================================
// MPI Network Exchange Structures for Coordinated Encounters
// =============================================================================

// Status codes for encounter replies (uint8_t for minimal struct size & fast
// comparison)
enum class ReplyStatus : uint8_t {
  ACCEPTED = 0,
  REJECTED_NOT_FOUND,
  REJECTED_DEAD,
  REJECTED_ALREADY_COMMITTED,
  REJECTED_NO_MATCHING_DEF,
  REJECTED_SCHEDULE_CONFLICT,
  REJECTED_DECLINED
};

struct EncounterProposal {
  int encounter_id;
  PersonId host_id;
  int host_rank;
  PersonId invitee_id;

  // Geometry & Routing Data
  VenueId venue_id;
  int venue_owner_rank;  // Rank that owns venue_id and routes the proposal.
  int venue_type_id;     // Tells InteractionManager which matrix to use

  // Temporal Data
  int slot;
  uint8_t encounter_type_id;
};

struct EncounterReply {
  int encounter_id;
  PersonId host_id;
  PersonId invitee_id;
  VenueId venue_id;
  int venue_type_id;
  int slot;
  uint8_t encounter_type_id;

  ReplyStatus status;
};

struct CoordinatedEncounter {
  int encounter_id;
  PersonId host_id;
  VenueId venue_id;
  int venue_type_id;
  int slot;
  uint8_t encounter_type_id;
  // Host's subset at venue_id, resolved on the host's rank at finalize. The
  // host's subset is copied to every injected participant so all participants
  // use the host's subgroup during binning. Virtual venues have no subset, so
  // the sentinel remains -1.
  SubsetIndex host_subset_index = -1;

  std::set<PersonId> participants;
};

// The id stamped on every row of one finalised encounter in the
// coordinated_encounters dataset. It is built from what the encounter is, not
// from a counter, so the same encounter gets the same id at any rank count.
//
//   bits 62..51  day      (12 bits, absolute simulation day, up to 4095)
//   bits 50..36  ordinal  (15 bits, the host's records that day, numbered
//                          from 0 in encounter_id order)
//   bits 35..0   host_id  (36 bits)
//
// 63 bits in all, so the value is always positive when read as a signed
// 64 bit integer. A field that does not fit throws; nothing is clamped or
// wrapped, because a wrapped id would collide with another encounter's.
inline constexpr int kGroupIdHostBits = 36;
inline constexpr int kGroupIdOrdinalBits = 15;
inline constexpr int kGroupIdDayBits = 12;

struct EncounterGroupIdParts {
  int day;
  uint32_t ordinal;
  uint64_t host_id;
};

inline uint64_t packEncounterGroupId(int64_t day, int64_t ordinal,
                                     int64_t host_id) {
  auto check = [](const char* name, int64_t value, int bits) {
    if (value < 0 || value >= (int64_t{1} << bits))
      throw std::runtime_error(
          "coordinated encounter group_id: " + std::string(name) + " " +
          std::to_string(value) + " does not fit in " + std::to_string(bits) +
          " bits (limit " + std::to_string((int64_t{1} << bits) - 1) + ")");
  };
  check("day", day, kGroupIdDayBits);
  check("ordinal", ordinal, kGroupIdOrdinalBits);
  check("host_id", host_id, kGroupIdHostBits);
  return (static_cast<uint64_t>(day)
          << (kGroupIdOrdinalBits + kGroupIdHostBits)) |
         (static_cast<uint64_t>(ordinal) << kGroupIdHostBits) |
         static_cast<uint64_t>(host_id);
}

inline EncounterGroupIdParts unpackEncounterGroupId(uint64_t id) {
  const uint64_t host_mask = (uint64_t{1} << kGroupIdHostBits) - 1;
  const uint64_t ordinal_mask = (uint64_t{1} << kGroupIdOrdinalBits) - 1;
  return {static_cast<int>(id >> (kGroupIdOrdinalBits + kGroupIdHostBits)),
          static_cast<uint32_t>((id >> kGroupIdHostBits) & ordinal_mask),
          id & host_mask};
}

// One id per record, aligned with `finalized`. A host's ordinal is the rank of
// the record's encounter_id among that host's records, so it does not depend
// on the order the records arrive in or on which rank holds them: all of a
// host's replies are finalised on the host's own rank.
inline std::vector<uint64_t> assignEncounterGroupIds(
    const std::vector<CoordinatedEncounter>& finalized, int day) {
  std::vector<size_t> order(finalized.size());
  std::iota(order.begin(), order.end(), size_t{0});
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    if (finalized[a].host_id != finalized[b].host_id)
      return finalized[a].host_id < finalized[b].host_id;
    return finalized[a].encounter_id < finalized[b].encounter_id;
  });
  std::vector<uint64_t> ids(finalized.size());
  int64_t ordinal = 0;
  for (size_t k = 0; k < order.size(); ++k) {
    const auto& enc = finalized[order[k]];
    if (k > 0 && finalized[order[k - 1]].host_id != enc.host_id) ordinal = 0;
    ids[order[k]] = packEncounterGroupId(day, ordinal++, enc.host_id);
  }
  return ids;
}

}  // namespace june
