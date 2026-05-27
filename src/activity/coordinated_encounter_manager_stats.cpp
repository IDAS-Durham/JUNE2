#include <algorithm>
#include <iomanip>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "activity/coordinated_encounter_manager.h"

#ifdef USE_MPI
#include <mpi.h>
#endif

namespace june {

namespace {

// Helper: resolve encounter_type_id to name
std::string resolveEncTypeName(uint8_t type_id, const WorldState& world) {
  if (type_id < world.encounter_type_names.size())
    return world.encounter_type_names[type_id];
  return "type_" + std::to_string(type_id);
}

// Sums the local vector into `global` across MPI_COMM_WORLD when running
// under MPI with more than one rank. Otherwise leaves `global` equal to its
// caller-supplied copy of the local vector. Two overloads: one for int and
// one for long long (the only types the encounter summary reduces).
void allReduceIfMulti(const std::vector<int>& local, std::vector<int>& global) {
#ifdef USE_MPI
  if (local.empty()) return;
  int world_size = 1;
  MPI_Comm_size(MPI_COMM_WORLD, &world_size);
  if (world_size > 1) {
    MPI_Allreduce(local.data(), global.data(), static_cast<int>(local.size()),
                  MPI_INT, MPI_SUM, MPI_COMM_WORLD);
  }
#else
  (void)local;
  (void)global;
#endif
}

void allReduceIfMulti(const std::vector<long long>& local,
                      std::vector<long long>& global) {
#ifdef USE_MPI
  if (local.empty()) return;
  int world_size = 1;
  MPI_Comm_size(MPI_COMM_WORLD, &world_size);
  if (world_size > 1) {
    MPI_Allreduce(local.data(), global.data(), static_cast<int>(local.size()),
                  MPI_LONG_LONG, MPI_SUM, MPI_COMM_WORLD);
  }
#else
  (void)local;
  (void)global;
#endif
}

// Layout of the per-type stats vector built by serializePerTypeStats:
//   [0] total_proposals
//   [1] total_finalized
//   [2 + i*kFieldsPerType + k] per-type field k for type i, where k indexes
//   into:
//     0 proposals_generated, 1 accepted, 2 rejected_not_found,
//     3 rejected_dead,       4 rejected_committed, 5 rejected_no_def,
//     6 rejected_schedule,   7 rejected_declined,
//     8 finalized_encounters, 9 total_participants
constexpr int kFieldsPerType = 10;

std::vector<int> serializePerTypeStats(
    const std::vector<CoordinatedEncounterDef>& enc_defs,
    const CoordinatedEncounterManager::DailyEncounterStats& daily_stats,
    const WorldState& world) {
  int num_types = static_cast<int>(enc_defs.size());
  int arr_size = num_types * kFieldsPerType + 2;

  std::vector<int> arr(arr_size, 0);
  arr[0] = daily_stats.total_proposals;
  arr[1] = daily_stats.total_finalized;

  for (int i = 0; i < num_types; ++i) {
    std::string name =
        resolveEncTypeName(enc_defs[i].cached_encounter_type_id, world);
    auto it = daily_stats.by_type.find(name);
    if (it == daily_stats.by_type.end()) continue;
    const auto& ts = it->second;
    int base = 2 + i * kFieldsPerType;
    arr[base + 0] = ts.proposals_generated;
    arr[base + 1] = ts.accepted;
    arr[base + 2] = ts.rejected_not_found;
    arr[base + 3] = ts.rejected_dead;
    arr[base + 4] = ts.rejected_committed;
    arr[base + 5] = ts.rejected_no_def;
    arr[base + 6] = ts.rejected_schedule;
    arr[base + 7] = ts.rejected_declined;
    arr[base + 8] = ts.finalized_encounters;
    arr[base + 9] = ts.total_participants;
  }
  return arr;
}

// Rank-0 dump of the global per-type encounter stats. `global_arr` must be
// laid out per the kFieldsPerType-aware contract from serializePerTypeStats;
// `enc_defs` supplies the human-readable type names.
void printPerTypeSummary(const std::vector<int>& global_arr,
                         const std::vector<CoordinatedEncounterDef>& enc_defs) {
  int num_types = static_cast<int>(enc_defs.size());
  for (int i = 0; i < num_types; ++i) {
    int base = 2 + i * kFieldsPerType;
    int proposals = global_arr[base + 0];
    int accepted = global_arr[base + 1];
    int rej_not_found = global_arr[base + 2];
    int rej_dead = global_arr[base + 3];
    int rej_committed = global_arr[base + 4];
    int rej_no_def = global_arr[base + 5];
    int rej_schedule = global_arr[base + 6];
    int rej_declined = global_arr[base + 7];
    int finalized = global_arr[base + 8];
    int participants = global_arr[base + 9];

    int total_replies = accepted + rej_not_found + rej_dead + rej_committed +
                        rej_no_def + rej_schedule + rej_declined;
    double accept_rate =
        total_replies > 0 ? 100.0 * accepted / total_replies : 0.0;

    std::cout << "      --- " << enc_defs[i].name << " ---" << std::endl;
    std::cout << "        Proposals:  " << proposals << std::endl;
    std::cout << "        Accepted:   " << accepted << " / " << total_replies
              << " (" << std::fixed << std::setprecision(1) << accept_rate
              << "%)" << std::endl;
    if (rej_committed > 0)
      std::cout << "        Rej(committed): " << rej_committed << std::endl;
    if (rej_schedule > 0)
      std::cout << "        Rej(schedule):  " << rej_schedule << std::endl;
    if (rej_declined > 0)
      std::cout << "        Rej(declined):  " << rej_declined << std::endl;
    if (rej_not_found > 0)
      std::cout << "        Rej(not_found): " << rej_not_found << std::endl;
    if (rej_dead > 0)
      std::cout << "        Rej(dead):      " << rej_dead << std::endl;
    if (rej_no_def > 0)
      std::cout << "        Rej(no_def):    " << rej_no_def << std::endl;
    std::cout << "        Finalized:  " << finalized << " encounters, "
              << participants << " participants" << std::endl;
  }
}

// Layout of the frequency-group stats vector (4 fields per group, in
// fg_names order): 0 persons_evaluated, 1 budget_hits, 2 encounters_emitted,
// 3 sum_daily_p * 1e6 (scaled to integer for MPI_SUM).
constexpr int kFgFields = 4;

// Rank-0 dump of the global per-frequency-group budget rolls.
void printFreqGroupSummary(const std::vector<std::string>& fg_names,
                           const std::vector<long long>& fg_global) {
  if (fg_names.empty()) return;
  std::cout << "      --- frequency_groups (budget rolls) ---" << std::endl;
  for (size_t gi = 0; gi < fg_names.size(); ++gi) {
    long long persons = fg_global[gi * kFgFields + 0];
    long long hits = fg_global[gi * kFgFields + 1];
    long long emitted = fg_global[gi * kFgFields + 2];
    double sum_p = static_cast<double>(fg_global[gi * kFgFields + 3]) / 1e6;
    double hit_rate = persons > 0 ? 100.0 * hits / persons : 0.0;
    double avg_p = persons > 0 ? sum_p / persons : 0.0;
    double emit_rate = hits > 0 ? 100.0 * emitted / hits : 0.0;
    std::cout << "        [" << fg_names[gi] << "] persons=" << persons
              << " avg_daily_p=" << std::fixed << std::setprecision(4) << avg_p
              << " hits=" << hits << " (" << std::fixed << std::setprecision(1)
              << hit_rate << "%) emitted=" << emitted << " (" << std::fixed
              << std::setprecision(1) << emit_rate << "% of hits)" << std::endl;
  }
}

std::vector<long long> serializeFreqGroupStats(
    const std::vector<std::string>& fg_names,
    const std::unordered_map<std::string,
                             CoordinatedEncounterManager::FreqGroupStats>&
        freq_group_stats) {
  std::vector<long long> arr(fg_names.size() * kFgFields, 0);
  for (size_t gi = 0; gi < fg_names.size(); ++gi) {
    auto it = freq_group_stats.find(fg_names[gi]);
    if (it == freq_group_stats.end()) continue;
    const auto& s = it->second;
    arr[gi * kFgFields + 0] = s.persons_evaluated;
    arr[gi * kFgFields + 1] = s.budget_hits;
    arr[gi * kFgFields + 2] = s.encounters_emitted;
    arr[gi * kFgFields + 3] = static_cast<long long>(s.sum_daily_p * 1e6);
  }
  return arr;
}

}  // namespace

void CoordinatedEncounterManager::accumulateProposalStats(
    const std::vector<EncounterProposal>& proposals) {
  for (const auto& p : proposals) {
    std::string name = resolveEncTypeName(p.encounter_type_id, world_);
    daily_stats_.by_type[name].proposals_generated++;
    daily_stats_.total_proposals++;
  }
}

void CoordinatedEncounterManager::accumulateReplyStats(
    const std::vector<EncounterReply>& replies) {
  for (const auto& r : replies) {
    std::string name = resolveEncTypeName(r.encounter_type_id, world_);
    auto& ts = daily_stats_.by_type[name];
    switch (r.status) {
      case ReplyStatus::ACCEPTED:
        ts.accepted++;
        break;
      case ReplyStatus::REJECTED_NOT_FOUND:
        ts.rejected_not_found++;
        break;
      case ReplyStatus::REJECTED_DEAD:
        ts.rejected_dead++;
        break;
      case ReplyStatus::REJECTED_ALREADY_COMMITTED:
        ts.rejected_committed++;
        break;
      case ReplyStatus::REJECTED_NO_MATCHING_DEF:
        ts.rejected_no_def++;
        break;
      case ReplyStatus::REJECTED_SCHEDULE_CONFLICT:
        ts.rejected_schedule++;
        break;
      case ReplyStatus::REJECTED_DECLINED:
        ts.rejected_declined++;
        break;
    }
  }
}

void CoordinatedEncounterManager::accumulateFinalizeStats(
    const std::vector<CoordinatedEncounter>& finalized) {
  for (const auto& enc : finalized) {
    std::string name = resolveEncTypeName(enc.encounter_type_id, world_);
    auto& ts = daily_stats_.by_type[name];
    ts.finalized_encounters++;
    ts.total_participants += static_cast<int>(enc.participants.size());
    daily_stats_.total_finalized++;
  }
}

void CoordinatedEncounterManager::printDailyEncounterSummary(int day) const {
  if (!config_.coordinated_encounters.enabled) return;

  const auto& enc_defs = config_.coordinated_encounters.encounters;

  std::vector<int> local_arr =
      serializePerTypeStats(enc_defs, daily_stats_, world_);
  std::vector<int> global_arr(local_arr);
  allReduceIfMulti(local_arr, global_arr);

  // Per-frequency-group counters: serialize and MPI-reduce on all ranks
  // BEFORE the rank-0 early return (MPI_Allreduce is a collective op).
  const auto& fg_map = config_.coordinated_encounters.frequency_groups;
  std::vector<std::string> fg_names;
  fg_names.reserve(fg_map.size());
  for (const auto& kv : fg_map) fg_names.push_back(kv.first);
  std::sort(fg_names.begin(), fg_names.end());

  std::vector<long long> fg_local =
      serializeFreqGroupStats(fg_names, freq_group_stats_);
  std::vector<long long> fg_global(fg_local);
  allReduceIfMulti(fg_local, fg_global);

  // Only rank 0 prints
  if (mpi_rank_ != 0) return;

  std::cout << "\n      ========== [ENCOUNTER DAILY SUMMARY] Day " << day
            << " ==========" << std::endl;
  std::cout << "      Total proposals: " << global_arr[0]
            << "  Total finalized: " << global_arr[1] << std::endl;

  printPerTypeSummary(global_arr, enc_defs);
  printFreqGroupSummary(fg_names, fg_global);

  std::cout << "      =================================================="
            << std::endl;
}

}  // namespace june
