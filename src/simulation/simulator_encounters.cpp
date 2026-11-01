// Simulator coordinated-encounter helpers: daily negotiation + slot injection
// + local-rank logging. Split from simulator.cpp (declared in
// simulation/simulator.h).
#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "simulation/encounters/eligibility.h"
#include "simulation/follow_bindings.h"
#include "simulation/simulator.h"
#include "utils/deterministic_rng.h"
#ifdef USE_MPI
#include "parallel/mpi_utils.h"
#endif

namespace june {

using encounters::buildEncounterLookups;
using encounters::computeLocalEligibility;
using encounters::EncounterEligibility;
using encounters::EncounterLookups;

namespace {

// When both A→B and B→A proposals are accepted in the same slot, two
// encounters exist for the same pair. Collapse them by keeping the lowest
// encounter_id; then sort by encounter_id so injection order is
// deterministic across ranks (later encounters for the same person skip
// already-assigned slots).
std::vector<CoordinatedEncounter> dedupAndSortDailyEncounters(
    std::vector<CoordinatedEncounter> daily_encounters) {
  std::map<std::pair<std::set<PersonId>, int>, size_t> best;
  for (size_t i = 0; i < daily_encounters.size(); ++i) {
    auto key = std::make_pair(daily_encounters[i].participants,
                              daily_encounters[i].slot);
    auto it = best.find(key);
    if (it == best.end()) {
      best[key] = i;
    } else if (daily_encounters[i].encounter_id <
               daily_encounters[it->second].encounter_id) {
      it->second = i;
    }
  }
  std::vector<CoordinatedEncounter> deduped;
  deduped.reserve(best.size());
  for (auto& [key, idx] : best) {
    deduped.push_back(std::move(daily_encounters[idx]));
  }
  std::sort(deduped.begin(), deduped.end(),
            [](const CoordinatedEncounter& a, const CoordinatedEncounter& b) {
              return a.encounter_id < b.encounter_id;
            });
  return deduped;
}

// MPI eligibility exchange. Each rank only knows its local participants'
// eligibility; for encounters with any remote participants we Allgatherv
// (encounter_id, local_eligible) pairs and sum across ranks. The result
// maps encounter_id -> global eligible count, with entries only for
// encounters that had any remote participants. Serial or single-rank
// builds return an empty map.
std::unordered_map<int, int> exchangeGlobalEligibility(
    const std::vector<EncounterEligibility>& slot_encounters,
    const std::vector<CoordinatedEncounter>& daily_encounters,
    const WorldState& world, DomainManager* domain_mgr) {
  std::unordered_map<int, int> global_eligible_map;
#ifdef USE_MPI
  if (!domain_mgr) return global_eligible_map;
  // Collect (encounter_id, local_eligible) for encounters with any remote
  // participants; these are the only ones that need exchange.
  std::vector<int32_t>
      local_pairs;  // flat array: [eid, count, eid, count, ...]
  for (const auto& ee : slot_encounters) {
    const auto& enc = daily_encounters[ee.encounter_idx];
    bool has_remote = false;
    for (PersonId pid : enc.participants) {
      if (world.person_index.find(pid) == world.person_index.end()) {
        has_remote = true;
        break;
      }
    }
    if (has_remote) {
      local_pairs.push_back(static_cast<int32_t>(enc.encounter_id));
      local_pairs.push_back(static_cast<int32_t>(ee.local_eligible));
    }
  }

  std::vector<int32_t> all_pairs = mpi_utils::allgathervInt32(local_pairs);

  for (size_t i = 0; i < all_pairs.size(); i += 2) {
    global_eligible_map[static_cast<int>(all_pairs[i])] +=
        static_cast<int>(all_pairs[i + 1]);
  }
#else
  (void)slot_encounters;
  (void)daily_encounters;
  (void)world;
  (void)domain_mgr;
#endif
  return global_eligible_map;
}

// Pass 2: stamp the per-encounter venue_id / encounter_type_id onto every
// local eligible participant of any encounter that meets its min_required
// threshold. Then for virtual venues (negative IDs) register the host-rank
// ownership so the visitor exchange routes cross-rank participants
// correctly.
void applyEncounterInjection(
    const std::vector<EncounterEligibility>& slot_encounters,
    const std::vector<CoordinatedEncounter>& daily_encounters,
    const std::unordered_map<int, int>& global_eligible_map,
    std::vector<PersonLocation>& locations, DomainManager* domain_mgr) {
  for (size_t i = 0; i < slot_encounters.size(); ++i) {
    const auto& ee = slot_encounters[i];
    const auto& enc = daily_encounters[ee.encounter_idx];

    // Use global eligible count if available (MPI mode with remote
    // participants); otherwise use local count (serial or all-local).
    int total_eligible;
    auto ge_it = global_eligible_map.find(enc.encounter_id);
    if (ge_it != global_eligible_map.end()) {
      total_eligible = ge_it->second;
    } else {
      total_eligible = ee.local_eligible;
    }

    if (total_eligible < ee.min_required || ee.local_eligible <= 0) continue;

    for (size_t array_idx : ee.eligible_indices) {
      locations[array_idx].venue_id = enc.venue_id;
      locations[array_idx].encounter_type_id = enc.encounter_type_id;
      // Copy the host's subset so every participant is binned in the host's
      // subgroup rather than using the scheduled venue's subset. Virtual
      // venues have no subset and retain -1.
      if (enc.host_subset_index >= 0)
        locations[array_idx].subset_index = enc.host_subset_index;
    }

#ifdef USE_MPI
    // Register virtual venue ownership so the visitor exchange can route
    // cross-rank encounter participants to the host's rank. Physical
    // venues already have ownership via the venue ownership map; virtual
    // venues need explicit registration. isVirtualVenue, to match the test in
    // Domain::ownsVenue that reads this registry back.
    if (domain_mgr && isVirtualVenue(enc.venue_id)) {
      int host_rank = domain_mgr->getPersonRank(enc.host_id);
      domain_mgr->setVenueRank(enc.venue_id, host_rank);
      if (host_rank == domain_mgr->getRank()) {
        domain_mgr->getDomain().registerVirtualVenue(enc.venue_id);
      }
    }
#else
    (void)domain_mgr;
#endif
  }
}

}  // namespace

void Simulator::negotiateAndLogDailyEncounters(int day) {
  if (!coordinated_encounter_manager_) return;
  coordinated_encounter_manager_->resetDaily();

  int encounter_day_type_idx = config_.schedule.getDayTypeIndex(day);

  // Phase 1: Generate proposals locally
  std::vector<EncounterProposal> local_proposals;
  coordinated_encounter_manager_->generateProposals(day, local_proposals,
                                                    encounter_day_type_idx);

  // Accumulate proposal stats for debug summary
  coordinated_encounter_manager_->accumulateProposalStats(local_proposals);

  // Phase 2: Exchange proposals across ranks, then process.
  // Keep local_proposals alive for mutual proposal detection.
  std::vector<EncounterProposal> all_proposals;
#ifdef USE_MPI
  if (domain_mgr_) {
    domain_mgr_->exchangeEncounterProposals(local_proposals, all_proposals);
  } else {
    all_proposals = local_proposals;  // copy, not move
  }
#else
  all_proposals = local_proposals;  // copy, not move
#endif

  std::vector<EncounterReply> local_replies;
  coordinated_encounter_manager_->processProposals(
      all_proposals, local_proposals, local_replies, encounter_day_type_idx);

  // Phase 3: Exchange replies back to host ranks, then finalize
  std::vector<EncounterReply> all_replies;
#ifdef USE_MPI
  if (domain_mgr_) {
    domain_mgr_->exchangeEncounterReplies(local_replies, all_replies);
  } else {
    all_replies = std::move(local_replies);
  }
#else
  all_replies = std::move(local_replies);
#endif

  // Accumulate reply stats for debug summary
  coordinated_encounter_manager_->accumulateReplyStats(all_replies);

  std::vector<CoordinatedEncounter> finalized;
  coordinated_encounter_manager_->finalizeEncounters(all_replies, finalized);

  // Accumulate finalize stats for debug summary
  coordinated_encounter_manager_->accumulateFinalizeStats(finalized);

  // Log locally-finalized encounters to HDF5 BEFORE merging remote ones, so
  // cross-rank encounters never get double-logged.
  logFinalizedEncountersLocally(finalized, day);

  // Phase 4: Exchange finalized encounters so remote participants know
#ifdef USE_MPI
  if (domain_mgr_) {
    std::vector<CoordinatedEncounter> remote_finalized;
    domain_mgr_->exchangeFinalizedEncounters(finalized, remote_finalized);
    // Add remote encounters that involve our local people
    for (auto& enc : remote_finalized) {
      coordinated_encounter_manager_->addDailyEncounter(enc);
    }
  }
#endif
}

void Simulator::logFinalizedEncountersLocally(
    const std::vector<CoordinatedEncounter>& finalized, int day) {
  // Skip before anything enters the EventLogger buffer, so disabling this
  // avoids the RAM cost too, not just the HDF5 write. Encounters still
  // happen and still drive transmission (see
  // injectCoordinatedEncountersIntoSlot); this only gates whether they're
  // recorded to simulation_events.h5.
  if (!config_.simulation.save_coordinated_encounters) return;

  // One group_id per finalised encounter, shared by its pair-rows. It is
  // derived from (day, host, ordinal), so it is the same at any rank count.
  const std::vector<uint64_t> group_ids =
      assignEncounterGroupIds(finalized, day);
  for (size_t i = 0; i < finalized.size(); ++i) {
    const auto& enc = finalized[i];
    const uint64_t group_id = group_ids[i];
    for (PersonId pid : enc.participants) {
      if (pid != enc.host_id) {
        event_logger_.logCoordinatedEncounter(
            enc.host_id, pid, current_simulation_time_, enc.encounter_type_id,
            enc.slot, group_id);
      }
    }
  }
}

void Simulator::injectCoordinatedEncountersIntoSlot(int time_slot_index) {
  if (!coordinated_encounter_manager_) return;

  // Per-slot lookup tables (trigger activities + min attendees) keyed by
  // encounter_type_id. Cheap to rebuild per slot.
  EncounterLookups lookups =
      buildEncounterLookups(world_, config_.coordinated_encounters.encounters);

  auto daily_encounters = dedupAndSortDailyEncounters(
      coordinated_encounter_manager_->getDailyEncounters());

  // Pass 1: compute each encounter's local eligible-participant set under
  // the alive + policy-block checks.
  std::vector<EncounterEligibility> slot_encounters = computeLocalEligibility(
      daily_encounters, time_slot_index, current_simulation_time_, lookups,
      world_, locations_, policy_manager_.get());

  // Pass 1.5: in MPI mode, sum local_eligible across ranks for encounters
  // that span ranks so every rank sees the true global count.
  std::unordered_map<int, int> global_eligible_map = exchangeGlobalEligibility(
      slot_encounters, daily_encounters, world_, domain_mgr_);

  // Pass 2: stamp venue_id / encounter_type_id onto eligible participants
  // for encounters that meet their min_required threshold.
  applyEncounterInjection(slot_encounters, daily_encounters,
                          global_eligible_map, locations_, domain_mgr_);
}

namespace {

// Where a host is this slot, as seen by its followers. activity is the host's
// resolved activity, used to decide the follower-side activity exception.
// venue_type travels with the venue because a follower's rank may not hold the
// host's venue in its own halo and so cannot type it locally.
using follow_detail::HostSlot;

}  // namespace

// The binding helpers live in a named namespace (rather than the anonymous one)
// so the follow unit tests can drive them directly on a synthetic world. They
// are otherwise internal to this translation unit.
namespace follow_detail {

template <typename T>
void sortUnique(std::vector<T>& v) {
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
}

// The venues that feed this person's pool: the ones of the rule's types the
// person is assigned to, narrowed by venue_choice. Sorted by id with no
// duplicates, so the result does not depend on how the world file orders a
// person's activities. A random choice is one draw per (seed, rule, person,
// key_day): the caller passes day 0 for a standing rule so the venue is held
// for the run, and the trip's start day for a hop rule so it is held for the
// trip.
std::vector<VenueId> poolVenues(const WorldState& world, const FollowConfig& fc,
                                uint64_t seed, const Person& person,
                                int key_day) {
  std::vector<VenueId> venues;
  for (const auto& meta : world.getActivityMetas(person)) {
    for (const auto& vs : world.getActivityVenues(meta)) {
      const uint8_t type = world.getVenueTypeId(vs.first);
      if (std::find(fc.pool_venue_type_ids.begin(),
                    fc.pool_venue_type_ids.end(),
                    type) != fc.pool_venue_type_ids.end())
        venues.push_back(vs.first);
    }
  }
  sortUnique(venues);
  switch (fc.pool.venue_choice.value_or(FollowPool::VenueChoice::All)) {
    case FollowPool::VenueChoice::All:
      break;
    case FollowPool::VenueChoice::LowestId:
      if (venues.size() > 1) venues.resize(1);
      break;
    case FollowPool::VenueChoice::Random:
      if (venues.size() > 1) {
        SplitMix64 rng(mix_seed(seed, hash_name(fc.name),
                                static_cast<uint64_t>(person.id),
                                static_cast<uint64_t>(key_day)));
        venues = {venues[bounded(rng, venues.size())]};
      }
      break;
  }
  return venues;
}

// Lowest hash first, ties by id, so the people a cap keeps are fixed by the
// seed and the host alone, however the world is partitioned.
void keepLowestHashes(const FollowConfig& fc, uint64_t seed, PersonId host,
                      std::vector<PersonId>& followers) {
  if (!fc.max_followers ||
      followers.size() <= static_cast<size_t>(*fc.max_followers))
    return;
  const uint64_t tag = hash_name("follow_cap:" + fc.name);
  auto key = [&](PersonId m) {
    return std::make_pair(mix_seed(seed, tag, static_cast<uint64_t>(host),
                                   static_cast<uint64_t>(m)),
                          m);
  };
  std::sort(followers.begin(), followers.end(),
            [&](PersonId x, PersonId y) { return key(x) < key(y); });
  followers.resize(static_cast<size_t>(*fc.max_followers));
}

// Does an exception keep this follower on its own schedule for the slot?
//
// Two of the three ask where the host is going. An excepted host activity peels
// an infant off to its own nursery while the parent is at work, and drops a
// host who is a patient being treated (medical_facility) while still following
// one merely sent home sick (residence). An excepted host venue type makes the
// cut activity cannot: leisure reaches a cinema, a gym and a grocery alike.
//
// The third asks what the follower would otherwise be doing, and it is the only
// one that can outrank a host who is somewhere perfectly followable. A
// school-age child trails a parent all day, but when the child's own schedule
// says school, school wins, whether the parent is at work, at home, or out. The
// host-side exceptions cannot express that: a parent with no primary activity
// is never at an excepted activity, so without this the parent's venue would
// overwrite the child's school every time.
bool venueExcepted(const FollowConfig& fc, uint8_t venue_type) {
  return std::find(fc.venue_exception_type_ids.begin(),
                   fc.venue_exception_type_ids.end(),
                   venue_type) != fc.venue_exception_type_ids.end();
}

bool mirrorSuppressed(const FollowConfig& fc, int16_t host_activity,
                      uint8_t host_venue_type, int16_t follower_activity) {
  // Every rank types every Venue, and the host is known to have one, so an
  // unresolvable type here can only be an id naming no Venue at all — a defect
  // under every configuration, gated rule or not. Keep this check independent
  // of venue_exceptions. Otherwise an invalid venue type could reach the
  // mirroring code without a valid venue-based policy decision.
  if (host_venue_type == kUnknownVenueTypeId)
    throw std::runtime_error(
        "follow: host venue type is unresolvable at the mirror gate");
  auto listed = [](const auto& ids, auto value) {
    return std::find(ids.begin(), ids.end(), value) != ids.end();
  };
  if (listed(fc.activity_exception_ids, host_activity)) return true;
  if (venueExcepted(fc, host_venue_type)) return true;
  // -1 means the follower has nowhere of its own to be this slot, so there is
  // nothing for an exception to protect and it follows.
  if (follower_activity >= 0 &&
      listed(fc.follower_activity_exception_ids, follower_activity))
    return true;
  return false;
}

bool policySuppressesMirror(PolicyManager* policy_manager,
                            const Person& follower,
                            int16_t follower_activity_index,
                            uint8_t host_venue_type, VenueId host_venue,
                            double current_time) {
  if (!policy_manager) return false;
  // known(), not fromVenue(): the host's type travelled with its venue, so no
  // local lookup is needed and none would succeed for a cross-rank host. A host
  // only enters host_loc with a venue, so the type is never absent here. The
  // venue goes along too, so a venue a trigger has closed is closed to a
  // follower mirroring into it.
  return policy_manager->suppressesParticipation(
      follower, follower_activity_index,
      SlotVenueType::known(host_venue_type, host_venue), current_time);
}

// Gather a packed int record list from every rank, or return the local list
// when the world is not split across ranks. The order of the result depends on
// the rank count, so every caller groups and sorts what it gets back.
std::vector<int32_t> gatherAllRanks(const std::vector<int32_t>& local,
                                    bool exchange) {
#ifdef USE_MPI
  if (exchange) return mpi_utils::allgathervInt32(local);
#endif
  (void)exchange;
  return local;
}

// Gather (key, value) pairs from every rank and group them by key, each group
// sorted with no repeats, so the result does not depend on the rank count.
std::map<int32_t, std::vector<int32_t>> gatherGrouped(
    const std::vector<int32_t>& local_pairs, bool exchange) {
  std::map<int32_t, std::vector<int32_t>> grouped;
  const std::vector<int32_t> all = gatherAllRanks(local_pairs, exchange);
  for (size_t i = 0; i + 2 <= all.size(); i += 2)
    grouped[all[i]].push_back(all[i + 1]);
  for (auto& [key, values] : grouped) sortUnique(values);
  return grouped;
}

PersonId maxPersonIdAcrossRanks(const WorldState& world, bool exchange) {
  PersonId max_id = -1;
  for (const Person& p : world.people) max_id = std::max(max_id, p.id);
#ifdef USE_MPI
  if (exchange)
    MPI_Allreduce(MPI_IN_PLACE, &max_id, 1, MPI_INT32_T, MPI_MAX,
                  MPI_COMM_WORLD);
#endif
  (void)exchange;
  return max_id;
}

void orAcrossRanks(std::vector<uint8_t>& bits, bool exchange) {
#ifdef USE_MPI
  if (exchange)
    MPI_Allreduce(MPI_IN_PLACE, bits.data(), static_cast<int>(bits.size()),
                  MPI_UINT8_T, MPI_BOR, MPI_COMM_WORLD);
#endif
  (void)bits;
  (void)exchange;
}

std::unordered_map<PersonId, PersonId> flattenFollowEdges(
    std::vector<FollowEdge> edges, std::vector<FollowEdge>& refused) {
  std::sort(edges.begin(), edges.end(),
            [](const FollowEdge& a, const FollowEdge& b) {
              return std::tie(a.rule, a.host, a.follower) <
                     std::tie(b.rule, b.host, b.follower);
            });
  std::unordered_map<PersonId, PersonId> parent;
  for (const FollowEdge& e : edges) {
    if (followRoot(parent, e.host) == e.follower) {
      refused.push_back(e);
      continue;
    }
    parent[e.follower] = e.host;
  }
  return parent;
}

// Who may follow and who may host under one rule, for every person in the world
// on every rank. A rank sees the full record only of the people it owns, so
// each rank fills in its own people and one bitwise-or reduction gives everyone
// the whole table. Eligibility, being alive, being free to follow (not away on
// a trip of one's own, for a hop rule) and being barred by an earlier rule are
// all folded in, so what the table says is the same at any rank count.
FollowPeople buildFollowPeople(
    WorldState& world, const FollowConfig& fc, bool free_followers_only,
    const std::unordered_set<PersonId>& follower_excl, bool exchange) {
  const PersonId max_id = maxPersonIdAcrossRanks(world, exchange);
  FollowPeople out;
  out.bits.assign(static_cast<size_t>(max_id + 1), 0);
  for (const Person& p : world.people) {
    if (p.is_dead) continue;
    uint8_t b = 0;
    if (!follower_excl.count(p.id) &&
        (!free_followers_only || !p.schedule_hop.isActive()) &&
        filtering::matchesCriteria(p, &world, fc.follower))
      b |= FollowPeople::kCanFollow;
    if (filtering::matchesCriteria(p, &world, fc.host))
      b |= FollowPeople::kCanHost;
    out.bits[static_cast<size_t>(p.id)] = b;
  }
  orAcrossRanks(out.bits, exchange);
  return out;
}

// Rebuild the criteria bindings from scratch. Every eligible local person
// becomes a follower of the lowest-id eligible host in its pool. There is no
// randomness in the choice of host, so the result is the same at any rank count
// and can be thrown away and recomputed each day rather than saved.
//
// For a venue pool, the rank that owns a venue has its full member list, so it
// finds the venue's lowest two eligible hosts, once per venue, from the shared
// table. The second is there because a person who is both a follower and
// host-eligible must not pick themselves. Those answers are shared, and a
// follower in several pool venues takes the lowest across them. The cost grows
// with the number of people and not with the square of a venue's size. For a
// network pool the follower's rank lists its partners itself, and the table
// says which of them may host, wherever they live. A host on another rank is
// collected into remote_hosts for the caller to route to its owning rank.
// A host with more followers than max_followers keeps the lowest hashes; the
// rest stay unbound under this rule. The cap looks at every follower of a host,
// so the picks are shared before it is applied.
void rebuildCriteriaBindings(
    WorldState& world, const FollowConfig& fc, uint64_t seed,
    const FollowPeople& people,
    std::unordered_map<PersonId, PersonId>& follower_host,
    std::unordered_set<PersonId>& active_hosts,
    std::vector<std::pair<PersonId, PersonId>>* remote_hosts,
    std::unordered_map<PersonId, PersonId>* new_follows, bool exchange) {
  follower_host.clear();
  active_hosts.clear();
  const bool net = fc.usesNetwork();

  struct VenueHosts {
    PersonId lowest = -1, second = -1;
  };
  std::unordered_map<VenueId, VenueHosts> venue_hosts;
  if (!net) {
    std::vector<int32_t> owned;
    for (const Venue& v : world.venues) {
      if (std::find(fc.pool_venue_type_ids.begin(),
                    fc.pool_venue_type_ids.end(),
                    v.type_id) == fc.pool_venue_type_ids.end())
        continue;
      VenueHosts vh;
      for (const auto& sub : world.getSubsets(v)) {
        for (PersonId m : world.getSubsetMembers(sub)) {
          if (!people.canHost(m)) continue;
          if (vh.lowest < 0 || m < vh.lowest) {
            vh.second = vh.lowest;
            vh.lowest = m;
          } else if (vh.second < 0 || m < vh.second) {
            vh.second = m;
          }
        }
      }
      owned.insert(owned.end(), {v.id, vh.lowest, vh.second});
    }
    const std::vector<int32_t> all = gatherAllRanks(owned, exchange);
    for (size_t i = 0; i + 3 <= all.size(); i += 3)
      venue_hosts[all[i]] = {all[i + 1], all[i + 2]};
  }

  std::vector<std::pair<PersonId, PersonId>> picks;  // (follower, host)
  for (const Person& f : world.people) {
    if (!people.canFollow(f.id)) continue;
    PersonId host = -1;
    if (net) {
      for (PersonId m : world.getNetworkPartners(f, fc.network_idx)) {
        if (m == f.id || (host >= 0 && m >= host)) continue;
        if (people.canHost(m)) host = m;
      }
    } else {
      for (VenueId v : poolVenues(world, fc, seed, f, 0)) {
        auto it = venue_hosts.find(v);
        if (it == venue_hosts.end())
          throw std::runtime_error("follow rule '" + fc.name +
                                   "': pool venue " + std::to_string(v) +
                                   " has no owning rank to list its members");
        const VenueHosts& vh = it->second;
        PersonId cand = vh.lowest != f.id ? vh.lowest : vh.second;
        if (cand >= 0 && (host < 0 || cand < host)) host = cand;
      }
    }
    if (host >= 0) picks.push_back({f.id, host});
  }

  if (fc.max_followers) {
    std::vector<int32_t> flat;
    for (const auto& [f, h] : picks) flat.insert(flat.end(), {h, f});
    std::unordered_set<PersonId> kept;
    for (auto& [h, followers] : gatherGrouped(flat, exchange)) {
      keepLowestHashes(fc, seed, h, followers);
      kept.insert(followers.begin(), followers.end());
    }
    picks.erase(
        std::remove_if(picks.begin(), picks.end(),
                       [&](const auto& p) { return !kept.count(p.first); }),
        picks.end());
  }

  for (const auto& [f, host] : picks) {
    follower_host[f] = host;
    if (new_follows) (*new_follows)[f] = host;
    // Activate the host so its location is available for mirroring. A local
    // host is added here; a remote one is routed to its owning rank.
    if (world.person_index.count(host))
      active_hosts.insert(host);
    else if (remote_hosts)
      remote_hosts->push_back({f, host});
  }
}

// A host passes its once-per-trip probability roll. Deterministic per
// (seed, host, hop-start day) so the same host commits the same way at any rank
// count and stays committed for every day of a multi-day trip.
bool hostRollsFollow(const FollowConfig& fc, uint64_t seed, PersonId host,
                     int hop_start_day) {
  if (fc.probability >= 1.0) return true;
  SplitMix64 rng(mix_seed(seed, static_cast<uint64_t>(host),
                          static_cast<uint64_t>(hop_start_day)));
  return uniform01(rng) < fc.probability;
}

// A host rolls once to gather followers from its pool. For a hop span the host
// must be on a trip and the roll is keyed on the trip's start day, so a trip
// resumed from a checkpoint mid-way re-rolls the same way. For a standing span
// there is no trip: any alive host rolls once, keyed on (seed, host) with a
// fixed day of 0, so re-considering it each slot is idempotent and the bond
// simply persists once formed. A host already tried, someone already following,
// or a host who does not meet host_eligibility, never rolls. A candidate is
// enrolled only if the table says it may follow: eligible, alive, not away on a
// trip of its own, and not barred by an earlier rule.
//
// A network pool is listed by the host's rank, which also decides the capped
// set, since the table covers partners on any rank; each chosen partner on
// another rank is collected into remote_invites for the caller to route. A
// venue pool is listed by the rank that owns the venue: each host that passes
// its roll sends its pool venues out, the owners list the eligible members, and
// every rank then groups the candidates by host, applies the cap and binds the
// followers it owns. Nothing in that depends on the rank count.
// A person picked by several hosts follows the smallest host id, so the outcome
// does not depend on the order hosts are visited.
void enrolFollowHosts(
    WorldState& world, const FollowConfig& fc, const ScheduleConfig& sched,
    uint64_t seed, bool standing, const FollowPeople& people,
    std::unordered_set<PersonId>& active_hosts,
    std::unordered_map<PersonId, PersonId>& follower_host, int current_day,
    std::vector<std::pair<PersonId, PersonId>>* remote_invites,
    std::unordered_map<PersonId, PersonId>* new_follows, bool exchange) {
  const bool net = fc.usesNetwork();

  // Bind a local follower to a host, keeping the smaller host id when it is
  // already bound. Returns false when the follower is not on this rank.
  auto bindLocal = [&](PersonId host_id, PersonId m) {
    if (!world.person_index.count(m)) return false;
    auto existing = follower_host.find(m);
    if (existing != follower_host.end() && existing->second <= host_id)
      return true;
    follower_host[m] = host_id;
    if (new_follows) (*new_follows)[m] = host_id;
    return true;
  };
  std::vector<int32_t> requests;  // venue pools: host, venue count, venues
  for (Person& host : world.people) {
    if (host.is_dead) continue;
    if (!standing && !host.schedule_hop.isActive()) continue;
    if (active_hosts.count(host.id)) continue;
    if (!people.canHost(host.id)) continue;
    active_hosts.insert(host.id);

    int roll_day = 0;
    if (!standing) {
      int hopped = host.schedule_hop.hopped_schedule_id;
      int n =
          (hopped >= 0 &&
           hopped < static_cast<int>(sched.schedule_types.size()))
              ? static_cast<int>(sched.schedule_types[hopped].flat_slots.size())
              : 1;
      if (n < 1) n = 1;
      roll_day = ScheduleHop::hopStartDay(current_day, static_cast<int16_t>(n),
                                          host.schedule_hop.temp_slot_progress);
    }
    if (!hostRollsFollow(fc, seed, host.id, roll_day)) continue;

    if (!net) {
      std::vector<VenueId> venues = poolVenues(world, fc, seed, host, roll_day);
      if (venues.empty()) continue;
      requests.push_back(host.id);
      requests.push_back(static_cast<int32_t>(venues.size()));
      requests.insert(requests.end(), venues.begin(), venues.end());
      continue;
    }

    std::vector<PersonId> picked;
    for (PersonId m : world.getNetworkPartners(host, fc.network_idx)) {
      if (m == host.id || !people.canFollow(m)) continue;
      picked.push_back(m);
    }
    sortUnique(picked);
    keepLowestHashes(fc, seed, host.id, picked);
    for (PersonId m : picked) {
      if (bindLocal(host.id, m)) continue;
      if (!remote_invites)
        throw std::runtime_error("follow rule '" + fc.name + "': partner " +
                                 std::to_string(m) +
                                 " lives on another rank and no route to it "
                                 "was given");
      remote_invites->push_back({m, host.id});
    }
  }

  if (!net) {
    const std::vector<int32_t> all_requests =
        gatherAllRanks(requests, exchange);
    std::vector<int32_t> candidates;  // host, member
    for (size_t i = 0; i + 2 <= all_requests.size();) {
      const PersonId host_id = all_requests[i];
      const size_t n = static_cast<size_t>(all_requests[i + 1]);
      for (size_t k = 0; k < n; ++k) {
        const Venue* venue = world.getVenue(all_requests[i + 2 + k]);
        if (!venue) continue;  // another rank owns it and lists it
        for (const auto& sub : world.getSubsets(*venue)) {
          for (PersonId m : world.getSubsetMembers(sub)) {
            if (m == host_id || !people.canFollow(m)) continue;
            candidates.insert(candidates.end(), {host_id, m});
          }
        }
      }
      i += 2 + n;
    }
    for (auto& [host_id, pool] : gatherGrouped(candidates, exchange)) {
      keepLowestHashes(fc, seed, host_id, pool);
      for (PersonId m : pool) bindLocal(host_id, m);
    }
  }
}

}  // namespace follow_detail

namespace {

#ifdef USE_MPI
// Route network follow invites across ranks: apply each (follower, host) pair
// whose follower is local, keeping the smallest host_id (order-independent, so
// identical at any rank count). A follower already claimed by an earlier rule
// on its own rank is dropped here, so exclusivity holds at any rank count.
void applyFollowInvites(
    const std::vector<std::pair<PersonId, PersonId>>& invites,
    const FollowConfig& fc, WorldState& world,
    std::unordered_map<PersonId, PersonId>& follower_host,
    std::unordered_map<PersonId, PersonId>* new_follows,
    const std::unordered_set<PersonId>& follower_excl) {
  std::vector<int32_t> local;
  local.reserve(invites.size() * 2);
  for (const auto& [f, h] : invites) {
    local.push_back(f);
    local.push_back(h);
  }
  std::vector<int32_t> all = mpi_utils::allgathervInt32(local);
  for (size_t i = 0; i + 1 < all.size(); i += 2) {
    PersonId f = all[i], h = all[i + 1];
    if (follower_excl.count(f)) continue;  // claimed by an earlier rule
    auto existing = follower_host.find(f);
    if (existing != follower_host.end() && existing->second <= h) continue;
    auto mi = world.person_index.find(f);
    if (mi == world.person_index.end()) continue;  // not local to this rank
    const Person& fp = world.people[mi->second];
    if (fp.is_dead || fp.schedule_hop.isActive()) continue;
    if (!filtering::matchesCriteria(fp, &world, fc.follower)) continue;
    follower_host[f] = h;
    if (new_follows) (*new_follows)[f] = h;
  }
}

// A network criteria follower picks its host locally, but that host may live on
// another rank. This gathers every (follower, host) pick and each rank adds to
// its active set the hosts it owns, so their locations get broadcast for
// mirroring. The follower side of the pick was already recorded locally.
void activateRemoteCriteriaHosts(
    const std::vector<std::pair<PersonId, PersonId>>& picks, WorldState& world,
    std::unordered_set<PersonId>& active_hosts) {
  std::vector<int32_t> local;
  local.reserve(picks.size() * 2);
  for (const auto& [f, h] : picks) {
    local.push_back(f);
    local.push_back(h);
  }
  std::vector<int32_t> all = mpi_utils::allgathervInt32(local);
  for (size_t i = 0; i + 1 < all.size(); i += 2) {
    PersonId h = all[i + 1];
    if (world.person_index.count(h)) active_hosts.insert(h);
  }
}

// Broadcast the hosts that are active this slot so remote followers can mirror.
// The caller passes active_now already holding this rank's active hosts; each
// is sent with its location and the received remote hosts are added back into
// active_now (and, where they have a venue, host_loc). The host's activity
// travels too so the activity exception works cross-rank. Sending only the
// slot-active hosts (not every enrolled host) keeps an off-hop host from being
// mirrored on a hop span.
//
// Ints per host on the wire. Shared by the packer, the bounds guard and the
// unpack stride so the two halves cannot drift.
constexpr size_t kHostLocationInts = 5;

void broadcastHostLocations(WorldState& world,
                            const std::vector<PersonLocation>& locations,
                            std::unordered_map<PersonId, HostSlot>& host_loc,
                            std::unordered_set<PersonId>& active_now) {
  std::vector<int32_t> local;
  for (PersonId h : active_now) {
    auto hi = world.person_index.find(h);
    if (hi == world.person_index.end()) continue;
    const PersonLocation& hl = locations[hi->second];
    local.push_back(h);
    local.push_back(hl.venue_id);
    local.push_back(hl.subset_index);
    local.push_back(hl.activity_index);
    local.push_back(world.getVenueTypeId(hl.venue_id));
  }
  std::vector<int32_t> all = mpi_utils::allgathervInt32(local);
  for (size_t i = 0; i + kHostLocationInts <= all.size();
       i += kHostLocationInts) {
    PersonId h = all[i];
    VenueId v = all[i + 1];
    SubsetIndex s = all[i + 2];
    int16_t act = static_cast<int16_t>(all[i + 3]);
    uint8_t venue_type = static_cast<uint8_t>(all[i + 4]);
    active_now.insert(h);
    if (v < 0) continue;
    // The sender typed this venue against its own all-venue type map, and the
    // v < 0 check above has already excluded venue-less hosts, so an unknown
    // type arriving here means the wire and the sender's venue registry have
    // diverged.
    if (venue_type == kUnknownVenueTypeId)
      throw std::runtime_error(
          "follow: host " + std::to_string(h) +
          " was broadcast with an unresolvable venue type for venue " +
          std::to_string(v));
    host_loc[h] = {v, s, act, venue_type};
  }
}
#endif  // USE_MPI

}  // namespace

void Simulator::injectFollowsIntoSlot(int time_slot_index) {
  const auto& rules = config_.coordinated_encounters.follows;
  if (rules.empty()) return;
  if (follow_state_.size() != rules.size()) follow_state_.resize(rules.size());

  const int day = static_cast<int>(current_simulation_time_);
  bool exchange = false;
#ifdef USE_MPI
  exchange = domain_mgr_ != nullptr && domain_mgr_->getNumRanks() > 1;
#endif

  // The rules run in config order, carrying forward everyone an earlier rule
  // has bound as a follower. A later rule yields to them, so a follower belongs
  // to the first rule that binds it, and a host may recur across rules. Config
  // order is fixed on every rank, so the resolution is the same at any rank
  // count.
  std::unordered_set<PersonId> committed_followers;
  std::vector<follow_detail::RuleSlot> slots(rules.size());
  for (size_t ri = 0; ri < rules.size(); ++ri) {
    if (!rules[ri].enabled) continue;
    processFollowRule(time_slot_index, day, static_cast<uint8_t>(ri), rules[ri],
                      follow_state_[ri], slots[ri], committed_followers);
  }

  // A host may itself follow, so some followers' hosts are followers too. Find
  // the live bonds whose follower is also someone's host (everyone else cannot
  // be part of a loop), share them, and settle them: a loop is cut by refusing
  // its last bond in a fixed order, so the cut is the same at any rank count.
  std::vector<follow_detail::FollowEdge> edges;
  for (size_t ri = 0; ri < rules.size(); ++ri)
    for (const auto& [f, h] : follow_state_[ri].follower_host)
      if (slots[ri].active_now.count(h))
        edges.push_back({static_cast<int>(ri), f, h});
  std::vector<uint8_t> is_host(
      static_cast<size_t>(
          follow_detail::maxPersonIdAcrossRanks(world_, exchange) + 1),
      0);
  for (const auto& e : edges) is_host[static_cast<size_t>(e.host)] = 1;
  follow_detail::orAcrossRanks(is_host, exchange);
  std::vector<int32_t> flat;
  for (const auto& e : edges)
    if (is_host[static_cast<size_t>(e.follower)])
      flat.insert(flat.end(), {e.rule, e.follower, e.host});
  std::vector<follow_detail::FollowEdge> chain;
  const std::vector<int32_t> all =
      follow_detail::gatherAllRanks(flat, exchange);
  for (size_t i = 0; i + 3 <= all.size(); i += 3)
    chain.push_back({all[i], all[i + 1], all[i + 2]});

  std::unordered_map<PersonId, PersonId> parent;
  std::unordered_map<PersonId, follow_detail::HostSlot> all_loc;
  if (!chain.empty()) {
    std::vector<follow_detail::FollowEdge> refused;
    parent = follow_detail::flattenFollowEdges(chain, refused);
    for (const auto& r : refused)
      follow_state_[static_cast<size_t>(r.rule)].follower_host.erase(
          r.follower);
    if (!refused.empty() && getRank() == 0)
      std::cout << "[follow] day " << day << ": refused " << refused.size()
                << " bond(s) that would close a loop\n";
    // A chain's root can be on another rank and in no rule's broadcast, so
    // share every rule's hosts once, before anyone moves.
    std::unordered_set<PersonId> active;
    for (const auto& s : slots) {
      all_loc.insert(s.host_loc.begin(), s.host_loc.end());
      active.insert(s.active_now.begin(), s.active_now.end());
    }
#ifdef USE_MPI
    if (exchange) broadcastHostLocations(world_, locations_, all_loc, active);
#endif
  }

  // Followers bound for a line take over their host's rider entries, so they
  // board the same runtime groups at the same time. Done once for all rules,
  // since it costs one exchange between ranks.
  std::vector<std::pair<PersonId, PersonId>> cotravel;
  for (size_t ri = 0; ri < rules.size(); ++ri) {
    if (!rules[ri].enabled) continue;
    mirrorFollowRule(rules[ri], follow_state_[ri],
                     chain.empty() ? slots[ri].host_loc : all_loc, parent,
                     cotravel);
  }
  if (runtime_group_allocator_)
    runtime_group_allocator_->attachFollowers(cotravel);
}

void Simulator::processFollowRule(
    int time_slot_index, int day, uint8_t rule_id, const FollowConfig& fc,
    FollowRuntime& st, follow_detail::RuleSlot& slot,
    std::unordered_set<PersonId>& committed_followers) {
  // With the world split across ranks, who may follow, the hosts of a venue
  // pool and the capped sets all come from exchanges every rank joins, so the
  // outcome does not depend on the rank count.
  bool exchange = false;
#ifdef USE_MPI
  exchange = domain_mgr_ != nullptr && domain_mgr_->getNumRanks() > 1;
#endif

  // A person follows under at most one rule, the first that binds them; a
  // host may follow too, and a chain is settled once every rule has run. These
  // are this rank's people; the table merges them across ranks.
  const std::unordered_set<PersonId>& follower_excl = committed_followers;

  // A binding this rule already holds may have just been claimed by an earlier
  // rule; drop it so exclusivity holds. A criteria rebuild would redo this, but
  // it only runs on a new day, so this also covers a criteria rule between
  // rebuilds.
  for (auto it = st.follower_host.begin(); it != st.follower_host.end();) {
    if (follower_excl.count(it->first))
      it = st.follower_host.erase(it);
    else
      ++it;
  }

  // 1. Form the bindings. Criteria has no randomness, so its whole set of
  //    bindings is rebuilt once a day; being derived rather than rolled, a
  //    fresh run and one resumed from a checkpoint arrive at the same bindings
  //    with nothing saved. Stochastic instead enrols each host once (as a trip
  //    starts for a hop span, or once up front for a standing span) and keeps
  //    it, serialising the result. In both cases a network pool may reach
  //    across ranks: criteria routes each remote host to its owner so its
  //    location is broadcast, stochastic routes each remote follower to its
  //    owner.
  bool span_standing = fc.span == FollowConfig::Span::Standing;
  const uint64_t seed = config_.simulation.random_seed;
  std::vector<std::pair<PersonId, PersonId>> remote_pairs;
  std::unordered_map<PersonId, PersonId> new_follows;
  if (fc.usesCriteria()) {
    if (day != st.follow_day) {
      st.follow_day = day;
      const follow_detail::FollowPeople people =
          follow_detail::buildFollowPeople(world_, fc, false, follower_excl,
                                           exchange);
      follow_detail::rebuildCriteriaBindings(
          world_, fc, seed, people, st.follower_host, st.active_hosts,
          exchange ? &remote_pairs : nullptr, fc.log ? &new_follows : nullptr,
          exchange);
#ifdef USE_MPI
      if (exchange)
        activateRemoteCriteriaHosts(remote_pairs, world_, st.active_hosts);
#endif
    }
  } else {
    const follow_detail::FollowPeople people = follow_detail::buildFollowPeople(
        world_, fc, true, follower_excl, exchange);
    follow_detail::enrolFollowHosts(world_, fc, config_.schedule, seed,
                                    span_standing, people, st.active_hosts,
                                    st.follower_host, day,
                                    exchange ? &remote_pairs : nullptr,
                                    fc.log ? &new_follows : nullptr, exchange);
#ifdef USE_MPI
    if (exchange && fc.usesNetwork())
      applyFollowInvites(remote_pairs, fc, world_, st.follower_host,
                         fc.log ? &new_follows : nullptr, follower_excl);
#endif
  }

  // Does any follower on any rank have a host on another rank? Only then does
  // a per-slot broadcast of host locations have anything to carry.
  bool cross_rank = false;
#ifdef USE_MPI
  if (exchange) {
    int remote = 0;
    for (const auto& [follower, host] : st.follower_host)
      if (!world_.person_index.count(host)) {
        remote = 1;
        break;
      }
    MPI_Allreduce(MPI_IN_PLACE, &remote, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
    cross_rank = remote != 0;
  }
#endif

  // Log each newly-established follow on the follower's rank (single log; event
  // shards merge at run end). These go to /events/follows, not the encounter
  // dataset: a follow is a binding, not a negotiated encounter, and a host's
  // travel party is recovered by grouping on (rule_id, host).
  if (fc.log) {
    for (const auto& [follower, host] : new_follows)
      event_logger_.logFollow(host, follower, current_simulation_time_, rule_id,
                              time_slot_index);
  }

  // 2. Which hosts are placed somewhere this slot, and where? Local hosts come
  //    from locations_; remote (network) hosts from a broadcast. A hop-span
  //    host counts while its trip runs; a standing host counts while it is
  //    alive. For a stochastic binding a host that has dropped out is removed
  //    here so it stops being broadcast and its followers get released below. A
  //    criteria binding is owned by the daily rebuild, so its set is left
  //    untouched: an off-hop or just-dead host is simply skipped this slot (its
  //    followers keep their own schedule) and the next rebuild revises the set.
  bool criteria = fc.usesCriteria();
  std::unordered_map<PersonId, HostSlot>& host_loc = slot.host_loc;
  std::unordered_set<PersonId>& active_now = slot.active_now;
  host_loc.clear();
  active_now.clear();
  for (auto it = st.active_hosts.begin(); it != st.active_hosts.end();) {
    auto hi = world_.person_index.find(*it);
    bool live =
        hi != world_.person_index.end() && !world_.people[hi->second].is_dead &&
        (span_standing || world_.people[hi->second].schedule_hop.isActive());
    if (!live) {
      if (criteria) {
        ++it;
        continue;
      }
      it = st.active_hosts.erase(it);
      continue;
    }
    active_now.insert(*it);
    const PersonLocation& hl = locations_[hi->second];
    if (hl.venue_id >= 0)
      host_loc[*it] = {hl.venue_id, hl.subset_index, hl.activity_index,
                       world_.getVenueTypeId(hl.venue_id)};
    ++it;
  }
#ifdef USE_MPI
  if (cross_rank)
    broadcastHostLocations(world_, locations_, host_loc, active_now);
#endif

  // 3. Release followers whose host has dropped out. Only stochastic bindings
  //    are consumed this way; a criteria binding persists until the daily
  //    rebuild, so nothing is erased here for it.
  if (!criteria) {
    for (auto f = st.follower_host.begin(); f != st.follower_host.end();) {
      if (!active_now.count(f->second))
        f = st.follower_host.erase(f);
      else
        ++f;
    }
  }

  // This rule's followers are now committed, so later rules skip them.
  for (const auto& [follower, host] : st.follower_host)
    committed_followers.insert(follower);
}

// Mirror each of a rule's followers onto the location of the end of its chain
// (its root host) for this slot. Every location was read before any follower
// moved, so the result does not depend on the order rules run in. The rule's
// own exceptions still decide whether this follower moves.
void Simulator::mirrorFollowRule(
    const FollowConfig& fc, const FollowRuntime& st,
    const std::unordered_map<PersonId, follow_detail::HostSlot>& host_loc,
    const std::unordered_map<PersonId, PersonId>& parent,
    std::vector<std::pair<PersonId, PersonId>>& cotravel) {
  for (const auto& [follower, direct_host] : st.follower_host) {
    const PersonId host = follow_detail::followRoot(parent, direct_host);
    auto fi = world_.person_index.find(follower);
    if (fi == world_.person_index.end() || world_.people[fi->second].is_dead)
      continue;
    auto hl = host_loc.find(host);
    if (hl == host_loc.end()) continue;  // host active but no venue this slot

    PersonLocation& floc = locations_[fi->second];
    // Off the wire, not a local lookup: a cross-rank host's venue is outside
    // this rank's halo and would type as unresolvable here.
    const uint8_t host_venue_type = hl->second.venue_type;
    if (follow_detail::mirrorSuppressed(fc, hl->second.activity,
                                        host_venue_type, floc.activity_index))
      continue;
    // The follower's own policy wins. If a policy would move them (sick and
    // sent home, say), decline the mirror and leave them on their own
    // schedule — ActivityManager has already placed them, and declining to
    // mirror moves nobody anywhere. The question is asked about the host's
    // venue type, since that is where the mirror is about to put them; a
    // venue-gated policy asked about their own venue would miss. Only the
    // named leg is gated — a partial-presence host's other legs are handled by
    // the venue exceptions below.
    if (follow_detail::policySuppressesMirror(
            policy_manager_.get(), world_.people[fi->second],
            floc.activity_index, host_venue_type, hl->second.venue,
            current_simulation_time_))
      continue;

    // A rider cannot be dropped off mid-journey: someone on a line stays on it
    // unless the host is on a line too, when they take over the host's journey
    // below. Moving them anywhere else would leave them aboard a line they are
    // no longer on.
    if (runtime_group_allocator_ &&
        runtime_group_allocator_->isPartialPresenceVenue(floc.venue_id) &&
        !runtime_group_allocator_->isPartialPresenceVenue(hl->second.venue))
      continue;

    // Travelling with the host means riding the host's whole journey, not the
    // single leg their location happens to name. A journey is all or nothing:
    // if any leg of it is a venue type this rule excepts, the follower stays on
    // its own schedule, because a child cannot be dropped at a bus stop and
    // rejoin two legs later.
    if (runtime_group_allocator_ &&
        runtime_group_allocator_->isPartialPresenceVenue(hl->second.venue)) {
      const auto& legs = runtime_group_allocator_->legsOf(host);
      bool excepted = false;
      for (VenueId leg : legs) {
        // The host's line may belong to another rank, whose venue types this
        // rank does not hold, so take the type the rider broadcast carried.
        if (follow_detail::venueExcepted(
                fc, runtime_group_allocator_->venueTypeOf(leg))) {
          excepted = true;
          break;
        }
      }
      if (excepted) continue;
      cotravel.emplace_back(follower, host);
    }

    // Copy the host's (venue, subset). The host resolved the subset against
    // this venue, so it is valid there and identical at any rank count.
    floc.venue_id = hl->second.venue;
    floc.encounter_type_id = fc.encounter_type_id;
    floc.subset_index = hl->second.subset;
  }
}

}  // namespace june
