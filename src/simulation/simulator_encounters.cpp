// Simulator coordinated-encounter helpers: daily negotiation + slot injection
// + local-rank logging. Split from simulator.cpp (declared in
// simulation/simulator.h).
#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include "simulation/simulator.h"
#include "utils/deterministic_rng.h"
#ifdef USE_MPI
#include "parallel/mpi_utils.h"
#endif

namespace june {

namespace {

// Per-slot lookup tables derived from the coordinated-encounters config:
// encounter_type_id -> trigger activity indices, and
// encounter_type_id -> min_attendees threshold.
struct EncounterLookups {
  std::unordered_map<uint8_t, std::vector<int16_t>> trigger_activities;
  std::unordered_map<uint8_t, int> min_attendees;
};

// Pass-1 result for one daily_encounter: which local participants pass the
// eligibility checks (alive + not policy-blocked at this slot), how many of
// them, and the threshold needed before this encounter gets injected.
struct EncounterEligibility {
  int encounter_idx;                     // index into daily_encounters
  std::vector<size_t> eligible_indices;  // local people passing policy
  int local_eligible;
  int min_required;
};

EncounterLookups buildEncounterLookups(
    const WorldState& world,
    const std::vector<CoordinatedEncounterDef>& encounters) {
  EncounterLookups out;
  for (const auto& def : encounters) {
    int type_id = world.getEncounterTypeIndex(def.name);
    if (type_id < 0) continue;
    std::vector<int16_t> indices;
    for (const auto& slot_name : def.trigger_slots) {
      int idx = world.getActivityIndex(slot_name);
      if (idx >= 0) indices.push_back(static_cast<int16_t>(idx));
    }
    out.trigger_activities[static_cast<uint8_t>(type_id)] = std::move(indices);
    out.min_attendees[static_cast<uint8_t>(type_id)] = def.min_attendees;
  }
  return out;
}

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

// Pass 1: per-encounter, compute the local participants who survive the
// alive + policy-block checks. Encounters not scheduled for this slot are
// skipped. Returned vector is index-aligned with the surviving subset of
// daily_encounters (each entry stores back into the source via
// .encounter_idx).
std::vector<EncounterEligibility> computeLocalEligibility(
    const std::vector<CoordinatedEncounter>& daily_encounters,
    int time_slot_index, double current_simulation_time,
    const std::unordered_map<uint8_t, std::vector<int16_t>>&
        encounter_trigger_activities,
    const std::unordered_map<uint8_t, int>& encounter_min_attendees,
    const WorldState& world, const std::vector<PersonLocation>& locations,
    PolicyManager* policy_manager) {
  std::vector<EncounterEligibility> slot_encounters;
  for (int ei = 0; ei < static_cast<int>(daily_encounters.size()); ++ei) {
    const auto& enc = daily_encounters[ei];
    if (enc.slot != time_slot_index) continue;

    auto trig_it = encounter_trigger_activities.find(enc.encounter_type_id);

    std::vector<size_t> eligible_indices;
    for (PersonId pid : enc.participants) {
      auto it = world.person_index.find(pid);
      if (it == world.person_index.end()) continue;

      size_t array_idx = it->second;
      if (array_idx >= locations.size()) continue;

      const Person& person = world.people[array_idx];
      if (person.is_dead) continue;

      bool policy_blocked = false;
      if (policy_manager && trig_it != encounter_trigger_activities.end()) {
        for (int16_t trigger_act_idx : trig_it->second) {
          auto override = policy_manager->getOverride(
              const_cast<Person&>(world.people[array_idx]), trigger_act_idx,
              locations[array_idx].venue_id, locations[array_idx].subset_index,
              current_simulation_time, time_slot_index);
          if (override.has_value()) {
            policy_blocked = true;
            break;
          }
        }
      }
      if (!policy_blocked) eligible_indices.push_back(array_idx);
    }

    int min_required = 2;
    auto min_it = encounter_min_attendees.find(enc.encounter_type_id);
    if (min_it != encounter_min_attendees.end()) {
      min_required = min_it->second;
    }

    EncounterEligibility ee;
    ee.encounter_idx = ei;
    ee.eligible_indices = std::move(eligible_indices);
    ee.local_eligible = static_cast<int>(ee.eligible_indices.size());
    ee.min_required = min_required;
    slot_encounters.push_back(std::move(ee));
  }
  return slot_encounters;
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
  std::vector<int> local_pairs;  // flat array: [eid, count, eid, count, ...]
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
      local_pairs.push_back(enc.encounter_id);
      local_pairs.push_back(ee.local_eligible);
    }
  }

  int local_count = static_cast<int>(local_pairs.size());
  int num_ranks = domain_mgr->getNumRanks();
  std::vector<int> all_counts(num_ranks);
  MPI_Allgather(&local_count, 1, MPI_INT, all_counts.data(), 1, MPI_INT,
                MPI_COMM_WORLD);

  std::vector<int> displs(num_ranks, 0);
  int total = 0;
  for (int r = 0; r < num_ranks; ++r) {
    displs[r] = total;
    total += all_counts[r];
  }

  std::vector<int> all_pairs(total);
  MPI_Allgatherv(local_pairs.data(), local_count, MPI_INT, all_pairs.data(),
                 all_counts.data(), displs.data(), MPI_INT, MPI_COMM_WORLD);

  for (int i = 0; i < total; i += 2) {
    global_eligible_map[all_pairs[i]] += all_pairs[i + 1];
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
      // Bug #13: adopt the host's subset so every participant bins as the
      // host's subgroup, not by the stale subset_index of the venue they were
      // scheduled to. -1 on virtual venues (no subsets), so left untouched.
      if (enc.host_subset_index >= 0)
        locations[array_idx].subset_index = enc.host_subset_index;
    }

#ifdef USE_MPI
    // Register virtual venue ownership so the visitor exchange can route
    // cross-rank encounter participants to the host's rank. Physical
    // venues already have ownership via the venue ownership map; virtual
    // venues (negative IDs) need explicit registration.
    if (domain_mgr && enc.venue_id < 0) {
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

void Simulator::negotiateAndLogDailyEncounters(int day, int rank) {
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
  logFinalizedEncountersLocally(finalized, rank);

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
    const std::vector<CoordinatedEncounter>& finalized, int rank) {
  // group_id fans one unique uint64 per real encounter across every pair-row
  // belonging to it. Rank is packed into the high 16 bits so counters stay
  // unique across MPI ranks without coordination; the low 48 bits are a
  // per-rank monotonic counter (2^48 events / rank is effectively unbounded
  // for realistic simulations).
  const uint64_t rank_prefix = static_cast<uint64_t>(rank) << 48;
  for (const auto& enc : finalized) {
    const uint64_t group_id =
        rank_prefix | (next_encounter_group_id_++ & 0x0000FFFFFFFFFFFFULL);
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
      daily_encounters, time_slot_index, current_simulation_time_,
      lookups.trigger_activities, lookups.min_attendees, world_, locations_,
      policy_manager_.get());

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

// Find the host's venue of the configured pool type, or -1 if it has none.
VenueId findPoolVenue(const WorldState& world, const Person& host,
                      int pool_venue_type_id) {
  for (const auto& meta : world.getActivityMetas(host)) {
    for (const auto& vs : world.getActivityVenues(meta)) {
      const Venue* v = world.getVenue(vs.first);
      if (v && v->type_id == pool_venue_type_id) return vs.first;
    }
  }
  return -1;
}

// The host's candidate pool: co-members of its venue of the configured type, or
// its partners in the configured network. Both return global PersonIds; callers
// enrol only those local to this rank (remote network partners are routed
// separately).
std::vector<PersonId> gatherPool(const WorldState& world,
                                 const FollowConfig& fc, const Person& host) {
  std::vector<PersonId> pool;
  if (fc.usesNetwork()) {
    for (PersonId m : world.getNetworkPartners(host, fc.network_idx))
      pool.push_back(m);
    return pool;
  }
  VenueId pool_venue = findPoolVenue(world, host, fc.pool_venue_type_id);
  if (pool_venue < 0) return pool;
  for (const auto& sub : world.getSubsets(*world.getVenue(pool_venue)))
    for (PersonId m : world.getSubsetMembers(sub)) pool.push_back(m);
  return pool;
}

// A host passes its once-per-trip probability roll. Deterministic per
// (seed, host, hop-start day) so the same host commits the same way at any rank
// count and stays committed for every day of a multi-day trip.
bool hostRollsFollow(const FollowConfig& fc, uint64_t seed, PersonId host,
                     int hop_start_day) {
  if (fc.probability >= 1.0) return true;
  SplitMix64 rng(mix_seed(seed, static_cast<uint64_t>(host),
                          static_cast<uint64_t>(hop_start_day)));
  double u =
      static_cast<double>(rng()) / static_cast<double>(SplitMix64::max());
  return u < fc.probability;
}

// Any local person newly on a hop rolls once to gather followers from its pool.
// A host is only tried once per trip, and someone already following (or away on
// their own trip, or dead) is never enrolled. Only pool members local to this
// rank are enrolled here; remote network partners are collected into
// remote_invites for cross-rank routing by the caller.
// Returns {hosts that gained followers, total local followers enrolled}.
std::pair<int, int> enrolFollowHosts(
    WorldState& world, const FollowConfig& fc, const ScheduleConfig& sched,
    uint64_t seed, std::unordered_set<PersonId>& active_hosts,
    std::unordered_map<PersonId, PersonId>& follower_host, int current_day,
    std::vector<std::pair<PersonId, PersonId>>* remote_invites,
    std::unordered_map<PersonId, PersonId>* new_follows) {
  int hosts = 0, followers = 0;
  for (Person& host : world.people) {
    if (host.is_dead || !host.schedule_hop.isActive()) continue;
    if (active_hosts.count(host.id) || follower_host.count(host.id)) continue;
    active_hosts.insert(host.id);

    // Key the roll on the hop's start day, not today, so a trip resumed from a
    // checkpoint mid-way re-rolls identically to the uninterrupted run. Both
    // are derivable from the checkpointed hop state, so follows need no
    // checkpoint serialization of their own.
    int hopped = host.schedule_hop.hopped_schedule_id;
    int n =
        (hopped >= 0 && hopped < static_cast<int>(sched.schedule_types.size()))
            ? static_cast<int>(sched.schedule_types[hopped].flat_slots.size())
            : 1;
    if (n < 1) n = 1;
    int hop_start_day =
        ScheduleHop::hopStartDay(current_day, static_cast<int16_t>(n),
                                 host.schedule_hop.temp_slot_progress);
    if (!hostRollsFollow(fc, seed, host.id, hop_start_day)) continue;

    int enrolled = 0;
    for (PersonId m : gatherPool(world, fc, host)) {
      if (m == host.id) continue;
      // A follower eligible for several on-hop hosts follows the smallest
      // host_id. This tiebreak is order-independent, so the choice is the same
      // however the world is partitioned across ranks.
      auto existing = follower_host.find(m);
      if (existing != follower_host.end() && existing->second <= host.id)
        continue;
      auto mi = world.person_index.find(m);
      if (mi == world.person_index.end()) {
        // Not local. For a network pool this partner lives in another domain;
        // route an invite to their rank. Venue pools are co-resident, so a
        // non-local member cannot occur there.
        if (remote_invites) remote_invites->push_back({m, host.id});
        continue;
      }
      const Person& mp = world.people[mi->second];
      if (mp.is_dead || mp.schedule_hop.isActive()) continue;
      follower_host[m] = host.id;
      if (new_follows) (*new_follows)[m] = host.id;
      ++enrolled;
    }
    if (enrolled > 0) {
      ++hosts;
      followers += enrolled;
    }
  }
  return {hosts, followers};
}

#ifdef USE_MPI
// Allgatherv a flat int array; every rank receives all ranks' contributions
// concatenated in rank order.
std::vector<int> allgathervInts(const std::vector<int>& local) {
  int nr;
  MPI_Comm_size(MPI_COMM_WORLD, &nr);
  int local_n = static_cast<int>(local.size());
  std::vector<int> sizes(nr);
  MPI_Allgather(&local_n, 1, MPI_INT, sizes.data(), 1, MPI_INT, MPI_COMM_WORLD);
  std::vector<int> displs;
  int total = 0;
  mpi_utils::computeDisplacements(sizes, displs, total);
  std::vector<int> all(total);
  MPI_Allgatherv(const_cast<int*>(local.data()), local_n, MPI_INT, all.data(),
                 sizes.data(), displs.data(), MPI_INT, MPI_COMM_WORLD);
  return all;
}

// Route network follow invites across ranks: apply each (follower, host) pair
// whose follower is local, keeping the smallest host_id (order-independent, so
// identical at any rank count).
void applyFollowInvites(
    const std::vector<std::pair<PersonId, PersonId>>& invites,
    WorldState& world, std::unordered_map<PersonId, PersonId>& follower_host,
    std::unordered_map<PersonId, PersonId>* new_follows) {
  std::vector<int> local;
  local.reserve(invites.size() * 2);
  for (const auto& [f, h] : invites) {
    local.push_back(static_cast<int>(f));
    local.push_back(static_cast<int>(h));
  }
  std::vector<int> all = allgathervInts(local);
  for (size_t i = 0; i + 1 < all.size(); i += 2) {
    PersonId f = all[i], h = all[i + 1];
    auto existing = follower_host.find(f);
    if (existing != follower_host.end() && existing->second <= h) continue;
    auto mi = world.person_index.find(f);
    if (mi == world.person_index.end()) continue;  // not local to this rank
    const Person& fp = world.people[mi->second];
    if (fp.is_dead || fp.schedule_hop.isActive()) continue;
    follower_host[f] = h;
    if (new_follows) (*new_follows)[f] = h;
  }
}

// Broadcast every rank's active-follow-host locations so remote followers can
// mirror. Adds each broadcast host to active_now, and (venue >= 0) to host_loc.
void broadcastHostLocations(
    const std::unordered_set<PersonId>& active_hosts, WorldState& world,
    const std::vector<PersonLocation>& locations,
    std::unordered_map<PersonId, std::pair<VenueId, SubsetIndex>>& host_loc,
    std::unordered_set<PersonId>& active_now) {
  std::vector<int> local;
  for (PersonId h : active_hosts) {
    auto hi = world.person_index.find(h);
    if (hi == world.person_index.end()) continue;
    const PersonLocation& hl = locations[hi->second];
    local.push_back(static_cast<int>(h));
    local.push_back(static_cast<int>(hl.venue_id));
    local.push_back(static_cast<int>(hl.subset_index));
  }
  std::vector<int> all = allgathervInts(local);
  for (size_t i = 0; i + 3 <= all.size(); i += 3) {
    PersonId h = all[i];
    VenueId v = all[i + 1];
    SubsetIndex s = all[i + 2];
    active_now.insert(h);
    if (v >= 0) host_loc[h] = {v, s};
  }
}
#endif  // USE_MPI

}  // namespace

void Simulator::injectFollowsIntoSlot(int time_slot_index) {
  const auto& fc = config_.coordinated_encounters.follow;
  if (!fc.enabled) return;

  static const bool dbg = std::getenv("JUNE_DEBUG_FOLLOW") != nullptr;
  // JUNE_DEBUG_FOLLOW_SAMPLE=N traces the first N followers through every slot
  // of their trip, so one travel party can be read end to end without drowning
  // in a line per follower per slot.
  static const int sample_n = [] {
    const char* e = std::getenv("JUNE_DEBUG_FOLLOW_SAMPLE");
    return e ? std::atoi(e) : 0;
  }();
  static std::set<PersonId> sampled;
  const int day = static_cast<int>(current_simulation_time_);

  // Network pools may enrol partners in other domains; venue pools are always
  // co-resident, so they need no cross-rank routing or per-slot broadcast.
  bool cross_rank = false;
#ifdef USE_MPI
  cross_rank = domain_mgr_ != nullptr && fc.usesNetwork();
#endif

  // 1. Enrol newly-active local hosts. Remote network partners are routed to
  //    their own ranks and applied there.
  std::vector<std::pair<PersonId, PersonId>> remote_invites;
  std::unordered_map<PersonId, PersonId> new_follows;
  auto [new_hosts, new_followers] = enrolFollowHosts(
      world_, fc, config_.schedule, config_.simulation.random_seed,
      active_follow_hosts_, follower_host_, day,
      cross_rank ? &remote_invites : nullptr, fc.log ? &new_follows : nullptr);
#ifdef USE_MPI
  if (cross_rank)
    applyFollowInvites(remote_invites, world_, follower_host_,
                       fc.log ? &new_follows : nullptr);
#endif

  // Log each newly-established follow on the follower's rank (single log; event
  // shards merge at run end). group_id = host_id groups a host's whole travel
  // party; the follow encounter_type distinguishes these rows from coordinated
  // encounters.
  if (fc.log) {
    for (const auto& [follower, host] : new_follows)
      event_logger_.logCoordinatedEncounter(
          host, follower, current_simulation_time_, fc.encounter_type_id,
          time_slot_index, static_cast<uint64_t>(host));
  }

  // 2. Which hosts are still on a hop this slot, and where? Local hosts come
  //    from locations_; remote (network) hosts from a broadcast. A local host
  //    whose hop ended is dropped from active_follow_hosts_ so it stops being
  //    broadcast and its followers get released below.
  std::unordered_map<PersonId, std::pair<VenueId, SubsetIndex>> host_loc;
  std::unordered_set<PersonId> active_now;
  for (auto it = active_follow_hosts_.begin();
       it != active_follow_hosts_.end();) {
    auto hi = world_.person_index.find(*it);
    bool live = hi != world_.person_index.end() &&
                world_.people[hi->second].schedule_hop.isActive();
    if (!live) {
      it = active_follow_hosts_.erase(it);
      continue;
    }
    active_now.insert(*it);
    const PersonLocation& hl = locations_[hi->second];
    if (hl.venue_id >= 0) host_loc[*it] = {hl.venue_id, hl.subset_index};
    ++it;
  }
#ifdef USE_MPI
  if (cross_rank)
    broadcastHostLocations(active_follow_hosts_, world_, locations_, host_loc,
                           active_now);
#endif

  // 3. Release followers whose host is no longer on a hop anywhere.
  int released = 0;
  for (auto f = follower_host_.begin(); f != follower_host_.end();) {
    if (!active_now.count(f->second)) {
      f = follower_host_.erase(f);
      ++released;
    } else {
      ++f;
    }
  }

  // 4. Mirror each follower onto its host's location for this slot.
  int mirrored = 0;
  for (const auto& [follower, host] : follower_host_) {
    auto fi = world_.person_index.find(follower);
    if (fi == world_.person_index.end() || world_.people[fi->second].is_dead)
      continue;
    auto hl = host_loc.find(host);
    if (hl == host_loc.end()) continue;  // host active but no venue this slot

    PersonLocation& floc = locations_[fi->second];
    // The follower's own policy wins. If a policy would move them (sick and
    // sent home, say), leave them where the policy put them.
    if (policy_manager_ &&
        policy_manager_
            ->getOverride(world_.people[fi->second], floc.activity_index,
                          floc.venue_id, floc.subset_index,
                          current_simulation_time_, time_slot_index)
            .has_value())
      continue;

    // Copy the host's (venue, subset). The host resolved the subset against
    // this venue, so it is valid there and identical at any rank count.
    floc.venue_id = hl->second.first;
    floc.encounter_type_id = fc.encounter_type_id;
    floc.subset_index = hl->second.second;
    ++mirrored;

    if (dbg && sample_n) {
      if (static_cast<int>(sampled.size()) < sample_n) sampled.insert(follower);
      if (sampled.count(follower))
        std::cerr << "[FOLLOW/trace] day " << day << " slot " << time_slot_index
                  << ": follower " << follower << " at host " << host
                  << " venue " << floc.venue_id << " subset "
                  << floc.subset_index << "\n";
    }
  }

  if (dbg)
    std::cerr << "[FOLLOW] day " << day << " slot " << time_slot_index << ": +"
              << new_hosts << " host(s)/" << new_followers << " follower(s), -"
              << released << " released, " << follower_host_.size()
              << " active, mirrored " << mirrored << "\n";
}

}  // namespace june
