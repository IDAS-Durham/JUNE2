#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/config.h"
#include "core/types.h"
#include "core/world_state.h"
#include "epidemiology/policy.h"

// Internal helpers of the follow subsystem, exposed here only so the follow
// unit tests can drive the binding logic directly on a synthetic world. The
// definitions live in simulator_encounters.cpp; nothing outside the simulator
// and its tests should depend on them.
namespace june {
namespace follow_detail {

// The venues that feed a person's pool, after venue_choice. Sorted by id.
// key_day holds a random choice: 0 for a standing rule, the trip's start day
// for a hop rule.
std::vector<VenueId> poolVenues(const WorldState& world, const FollowConfig& fc,
                                uint64_t seed, const Person& person,
                                int key_day);

// Where a host is this slot: its resolved venue and subset, and the activity
// that put it there. The venue type travels with the venue because a follower's
// rank may not hold the host's venue in its own halo and so cannot type it.
struct HostSlot {
  VenueId venue = -1;
  SubsetIndex subset = -1;
  int16_t activity = -1;
  uint8_t venue_type = kUnknownVenueTypeId;
};

// One rule's view of this slot: which hosts are placed somewhere, and where,
// read before any follower moves.
struct RuleSlot {
  std::unordered_map<PersonId, HostSlot> host_loc;
  std::unordered_set<PersonId> active_now;
};

// One live bond this slot: follower goes where host goes, under rule `rule`.
struct FollowEdge {
  int rule;
  PersonId follower, host;
};

// Settle chains. Edges are taken in a fixed order (rule, then host id, then
// follower id); one that would make a person their own root is refused and
// appended to `refused`. Returns follower -> host for the accepted edges, which
// form no loop, so followRoot always ends.
std::unordered_map<PersonId, PersonId> flattenFollowEdges(
    std::vector<FollowEdge> edges, std::vector<FollowEdge>& refused);

// The person at the end of a chain of follows: p itself when p follows nobody.
inline PersonId followRoot(const std::unordered_map<PersonId, PersonId>& parent,
                           PersonId p) {
  for (auto it = parent.find(p); it != parent.end(); it = parent.find(p))
    p = it->second;
  return p;
}

// The highest person id in the world, and a bitwise-or of a per-person byte
// table across ranks (both are no-ops when exchange is false).
PersonId maxPersonIdAcrossRanks(const WorldState& world, bool exchange);
void orAcrossRanks(std::vector<uint8_t>& bits, bool exchange);

// Who may follow and who may host under one rule, for every person in the
// world, identical on every rank. See buildFollowPeople.
struct FollowPeople {
  static constexpr uint8_t kCanFollow = 1;
  static constexpr uint8_t kCanHost = 2;
  std::vector<uint8_t> bits;  // indexed by PersonId
  bool canFollow(PersonId p) const { return has(p, kCanFollow); }
  bool canHost(PersonId p) const { return has(p, kCanHost); }

 private:
  bool has(PersonId p, uint8_t bit) const {
    return p >= 0 && static_cast<size_t>(p) < bits.size() &&
           (bits[static_cast<size_t>(p)] & bit);
  }
};

// Fill the table from the people this rank owns, then merge it across ranks
// when exchange is set. A person may follow if alive, eligible, barred by no
// earlier rule (follower_excl) and, when free_followers_only, not away on a
// trip of their own. A person may host if alive, host-eligible; a host may
// itself follow.
FollowPeople buildFollowPeople(
    WorldState& world, const FollowConfig& fc, bool free_followers_only,
    const std::unordered_set<PersonId>& follower_excl, bool exchange);

// Every rank's packed int records, or the local ones when exchange is false.
std::vector<int32_t> gatherAllRanks(const std::vector<int32_t>& local,
                                    bool exchange);

// A host's deterministic per-(seed, host, day) probability roll.
bool hostRollsFollow(const FollowConfig& fc, uint64_t seed, PersonId host,
                     int hop_start_day);

// Rebuild the criteria bindings from scratch: every eligible person follows the
// lowest-id eligible host in its pool, as the table says.
void rebuildCriteriaBindings(
    WorldState& world, const FollowConfig& fc, uint64_t seed,
    const FollowPeople& people,
    std::unordered_map<PersonId, PersonId>& follower_host,
    std::unordered_set<PersonId>& active_hosts,
    std::vector<std::pair<PersonId, PersonId>>* remote_hosts,
    std::unordered_map<PersonId, PersonId>* new_follows, bool exchange);

// Does an exception stop this follower from being mirrored onto its host for
// the slot? Any of the three firing means the follower keeps its own schedule:
// the host is at an excepted activity, the host is at an excepted venue type,
// or the follower has an activity of its own that outranks following. Pass -1
// for an activity the person does not have this slot.
bool mirrorSuppressed(const FollowConfig& fc, int16_t host_activity,
                      uint8_t host_venue_type, int16_t follower_activity);

// Does the follower's own policy outrank the mirror? A Policy Suppression
// query and nothing more: true means a policy would remove the follower from
// their own activity, so the mirror is declined and they keep their schedule.
// Deciding not to mirror commits nothing, so no venue is taken — nobody is
// pinned or moved here.
//
// The gate keys on host_venue_type, the type the mirror is about to move the
// follower to, not the type of their own scheduled venue; a gated policy asked
// about the latter would miss. The activity stays the follower's, since the
// mirror leaves it alone. The type comes off the broadcastHostLocations wire,
// since a cross-rank host's venue is outside this rank's halo and cannot be
// typed locally. An unresolvable type throws. The host's venue goes along with
// its type, so a venue a trigger has closed is closed to a mirroring follower.
bool policySuppressesMirror(PolicyManager* policy_manager,
                            const Person& follower,
                            int16_t follower_activity_index,
                            uint8_t host_venue_type, VenueId host_venue,
                            double current_time);

// Stochastic enrolment: each host rolls once and gathers followers from its
// pool and binds the followers this rank owns.
void enrolFollowHosts(
    WorldState& world, const FollowConfig& fc, const ScheduleConfig& sched,
    uint64_t seed, bool standing, const FollowPeople& people,
    std::unordered_set<PersonId>& active_hosts,
    std::unordered_map<PersonId, PersonId>& follower_host, int current_day,
    std::vector<std::pair<PersonId, PersonId>>* remote_invites,
    std::unordered_map<PersonId, PersonId>* new_follows, bool exchange);

}  // namespace follow_detail
}  // namespace june
