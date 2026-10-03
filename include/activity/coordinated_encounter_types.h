#pragma once

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

}  // namespace june
