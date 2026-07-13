#pragma once

#include <iostream>
#include <set>

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

// For logging/debugging only. Not used in hot paths
inline const char* replyStatusToString(ReplyStatus s) {
  switch (s) {
    case ReplyStatus::ACCEPTED:
      return "ACCEPTED";
    case ReplyStatus::REJECTED_NOT_FOUND:
      return "REJECTED_NOT_FOUND";
    case ReplyStatus::REJECTED_DEAD:
      return "REJECTED_DEAD";
    case ReplyStatus::REJECTED_ALREADY_COMMITTED:
      return "REJECTED_ALREADY_COMMITTED";
    case ReplyStatus::REJECTED_NO_MATCHING_DEF:
      return "REJECTED_NO_MATCHING_DEF";
    case ReplyStatus::REJECTED_SCHEDULE_CONFLICT:
      return "REJECTED_SCHEDULE_CONFLICT";
    case ReplyStatus::REJECTED_DECLINED:
      return "REJECTED_DECLINED";
    default:
      return "UNKNOWN";
  }
}

inline std::ostream& operator<<(std::ostream& os, ReplyStatus s) {
  return os << replyStatusToString(s);
}

struct EncounterProposal {
  int encounter_id;
  PersonId host_id;
  int host_rank;
  PersonId invitee_id;

  // Geometry & Routing Data
  VenueId venue_id;
  int venue_owner_rank;  // Resolves the "Ghost Host" bug for MPI routing
  int venue_type_id;     // Tells InteractionManager which matrix to use

  // Temporal Data
  int slot;
  uint8_t encounter_type_id;
};

// The reply sent from the invitee back to the host rank
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

// The finalized event distributed to all participants' ranks
struct CoordinatedEncounter {
  int encounter_id;
  PersonId host_id;
  VenueId venue_id;
  int venue_type_id;
  int slot;
  uint8_t encounter_type_id;
  // Host's subset at venue_id, resolved on the host's rank at finalize. Every
  // injected participant adopts it so they bin as the host's subgroup rather
  // than by the subset_index of the venue they were scheduled to. -1 on
  // virtual venues (no subsets).
  SubsetIndex host_subset_index = -1;

  std::set<PersonId> participants;
};

}  // namespace june
