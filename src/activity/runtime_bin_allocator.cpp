#include "../../include/activity/runtime_bin_allocator.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <unordered_map>

#include "../../include/core/config.h"
#include "../../include/core/world_state.h"
#include "../../include/utils/deterministic_rng.h"

#ifdef USE_MPI
#include <mpi.h>

#include "../../include/parallel/mpi_utils.h"
#endif

namespace june {

RuntimeBinAllocator::RuntimeBinAllocator(const WorldState& world,
                                         const Config& config)
    : world_(world), config_(config) {
  venue_type_mask_ =
      config_.simulation.partial_presence.enabled_venue_type_mask;
}

void RuntimeBinAllocator::allocateForSlot(
    int time_slot_index, int day_type_idx, const TimeSlot& /*slot*/,
    double current_simulation_time, double delta_hours,
    const std::vector<PersonLocation>& locations) {
  // Clear bin assignments from the previous slot.
  bin_by_av_idx_.clear();

  if (venue_type_mask_ == 0) return;  // feature off — zero overhead

  // Slot key for canonical RNG seeding across ranks. Sim time is in days;
  // round to integer minutes to avoid FP-summation drift between ranks.
  const uint64_t slot_minutes =
      static_cast<uint64_t>(current_simulation_time * 1440.0 + 0.5);
  (void)delta_hours;  // FOI clamp is downstream of the allocator.
  const uint64_t base_seed = config_.simulation.random_seed;

  // -------------------------------------------------------------------------
  // Phase 1 — collect LOCAL (rider, leg) entries.
  //
  // For each person assigned a non-trivial activity this slot, walk every
  // activity_meta with that activity_index and add EACH leg whose venue is
  // partial-presence. A multi-leg journey (e.g. a 3-leg train commute)
  // produces 3 entries — one per leg — keyed independently by their flat
  // activity_venue index.
  //
  // We deliberately include legs whose raw (t_board_min, t_alight_min)
  // sit outside the slot window: a long-distance commuter (e.g. Durham →
  // London) must still expose riders on their destination-region legs to
  // model inter-regional disease spread. The FOI loop downstream
  // distributes each rider's effective presence across their legs
  // proportionally so the total transit FOI per rider is bounded by the
  // slot duration.
  // -------------------------------------------------------------------------
  struct LocalLeg {
    uint32_t av_idx;
    VenueId venue_id;
    PersonId person_id;
  };
  std::vector<LocalLeg> local_legs;
  local_legs.reserve(locations.size() / 64);

  for (size_t i = 0; i < locations.size(); ++i) {
    const int16_t act = locations[i].activity_index;
    if (act < 0) continue;  // person inactive this slot

    const Person& person = world_.people[i];
    for (int m = 0; m < person.activity_meta_count; ++m) {
      const auto& meta = world_.activity_meta[person.activity_meta_start + m];
      if (meta.activity_index != act) continue;

      auto venues = world_.getActivityVenues(meta);
      for (size_t k = 0; k < venues.size(); ++k) {
        const VenueId vid = venues[k].first;
        const uint8_t vt = world_.getVenueTypeId(vid);
        if (vt >= 64) continue;
        if (((venue_type_mask_ >> vt) & 1ULL) == 0) continue;

        const uint32_t av_idx = static_cast<uint32_t>(meta.venue_start + k);
        local_legs.push_back({av_idx, vid, person.id});
      }
    }
  }

  // -------------------------------------------------------------------------
  // Phase 2 — exchange (venue_id, person_id) pairs across ranks so every
  // rank has the same global rider list per partial-presence venue.
  // Multi-leg riders contribute one pair per leg (same person_id may
  // appear in multiple venues' lists — that's the point).
  // -------------------------------------------------------------------------
  std::vector<int32_t> local_packed;
  local_packed.reserve(local_legs.size() * 2);
  for (const auto& lg : local_legs) {
    local_packed.push_back(static_cast<int32_t>(lg.venue_id));
    local_packed.push_back(static_cast<int32_t>(lg.person_id));
  }

  std::vector<int32_t> global_packed = local_packed;

#ifdef USE_MPI
  int world_size = 1;
  MPI_Comm_size(MPI_COMM_WORLD, &world_size);
  if (world_size > 1) {
    int local_count = static_cast<int>(local_packed.size());
    std::vector<int> counts(world_size);
    MPI_Allgather(&local_count, 1, MPI_INT, counts.data(), 1, MPI_INT,
                  MPI_COMM_WORLD);
    std::vector<int> displs;
    int total = 0;
    mpi_utils::computeDisplacements(counts, displs, total);
    global_packed.assign(total, 0);
    MPI_Allgatherv(local_packed.data(), local_count, MPI_INT,
                   global_packed.data(), counts.data(), displs.data(), MPI_INT,
                   MPI_COMM_WORLD);
  }
#endif

  // -------------------------------------------------------------------------
  // Phase 3 — group global pairs by venue, canonical-sort, deal into bins.
  // -------------------------------------------------------------------------
  std::unordered_map<VenueId, std::vector<PersonId>> riders_by_venue;
  for (size_t i = 0; i + 1 < global_packed.size(); i += 2) {
    VenueId v = static_cast<VenueId>(global_packed[i]);
    PersonId p = static_cast<PersonId>(global_packed[i + 1]);
    riders_by_venue[v].push_back(p);
  }

  // (venue_id, person_id) → bin. The same person on multiple venues gets a
  // separate bin per venue (multi-leg). For local riders we then look this
  // up by their leg's (vid, pid) to populate bin_by_av_idx_.
  std::unordered_map<uint64_t, uint16_t> bin_by_vid_pid;
  bin_by_vid_pid.reserve(global_packed.size() / 2);

  for (auto& [vid, riders] : riders_by_venue) {
    const uint8_t vt = world_.getVenueTypeId(vid);
    const int tgs = config_.simulation.partial_presence.getTargetGroupSize(vt);
    if (tgs <= 0) continue;  // defensive: type wasn't actually enabled

    // Canonical shuffle: sort by hash(seed, slot_minutes, vid, pid). Same
    // hash on every rank → same order → same bin assignments. PersonId as
    // a deterministic tiebreaker on the (vanishingly rare) hash collision.
    auto hash_key = [&](PersonId pid) {
      return mix_seed(
          mix_seed(base_seed, slot_minutes, static_cast<uint64_t>(vid)),
          static_cast<uint64_t>(pid));
    };
    std::sort(riders.begin(), riders.end(), [&](PersonId a, PersonId b) {
      uint64_t ha = hash_key(a), hb = hash_key(b);
      if (ha != hb) return ha < hb;
      return a < b;
    });

    const int num_bins =
        std::max(1, static_cast<int>((riders.size() + tgs - 1) / tgs));

    for (size_t pos = 0; pos < riders.size(); ++pos) {
      const PersonId pid = riders[pos];
      const uint16_t bin = static_cast<uint16_t>(pos % num_bins);
      const uint64_t key =
          (static_cast<uint64_t>(vid) << 32) | static_cast<uint64_t>(pid);
      bin_by_vid_pid[key] = bin;
    }
  }

  // -------------------------------------------------------------------------
  // Phase 4 — write per-leg bins into the sparse av_idx map.
  // -------------------------------------------------------------------------
  bin_by_av_idx_.reserve(local_legs.size());
  for (const auto& lg : local_legs) {
    const uint64_t key = (static_cast<uint64_t>(lg.venue_id) << 32) |
                         static_cast<uint64_t>(lg.person_id);
    auto it = bin_by_vid_pid.find(key);
    bin_by_av_idx_[lg.av_idx] =
        (it == bin_by_vid_pid.end()) ? kNoBin : it->second;
  }

  // One-time visibility on first non-empty slot.
  static bool printed_first = false;
  if (!printed_first && !riders_by_venue.empty()) {
    size_t total_global_pairs = 0;
    size_t max_bins = 0;
    size_t multi_leg_local = 0;
    {
      std::unordered_map<PersonId, int> per_pid;
      for (const auto& lg : local_legs) per_pid[lg.person_id]++;
      for (auto& kv : per_pid)
        if (kv.second > 1) multi_leg_local++;
    }
    for (const auto& kv : riders_by_venue) {
      total_global_pairs += kv.second.size();
      const int tgs = config_.simulation.partial_presence.getTargetGroupSize(
          world_.getVenueTypeId(kv.first));
      if (tgs > 0) {
        size_t nb = std::max<size_t>(1, (kv.second.size() + tgs - 1) / tgs);
        max_bins = std::max(max_bins, nb);
      }
    }
    std::cout << "[RuntimeBinAllocator] first active slot: time_slot_index="
              << time_slot_index << " day_type=" << day_type_idx
              << " partial-presence venues=" << riders_by_venue.size()
              << " global_legs=" << total_global_pairs
              << " local_legs=" << local_legs.size()
              << " local_multi_leg_riders=" << multi_leg_local
              << " max_bins_in_a_venue=" << max_bins << std::endl;

    // Verification dump (env-var-gated). One line per (local rider, leg).
    if (const char* dump_dir = std::getenv("JUNE_BIN_DUMP")) {
      int rank = 0;
#ifdef USE_MPI
      MPI_Comm_rank(MPI_COMM_WORLD, &rank);
#endif
      std::string path =
          std::string(dump_dir) + "/bins_rank" + std::to_string(rank) + ".txt";
      FILE* f = std::fopen(path.c_str(), "w");
      if (f) {
        for (const auto& lg : local_legs) {
          std::fprintf(f, "pid=%d vid=%d bin=%u\n",
                       static_cast<int>(lg.person_id),
                       static_cast<int>(lg.venue_id),
                       static_cast<unsigned>(bin_by_av_idx_[lg.av_idx]));
        }
        std::fclose(f);
      }
    }
    printed_first = true;
  }
}

}  // namespace june
