#include "../../include/activity/runtime_bin_allocator.h"

#include <algorithm>
#include <cmath>
#include <cstring>
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
  bin_by_vid_pid_.clear();
  num_bins_by_venue_.clear();
  windows_by_vid_pid_.clear();

  if (venue_type_mask_ == 0) return;  // feature off — zero overhead

  // Slot key for canonical RNG seeding across ranks. Sim time is in days;
  // round to integer minutes to avoid FP-summation drift between ranks.
  const uint64_t slot_minutes =
      static_cast<uint64_t>(current_simulation_time * 1440.0 + 0.5);
  const uint64_t base_seed = config_.simulation.random_seed;

  // Slot duration in minutes — input to presence_window helper.
  const float slot_duration_min = static_cast<float>(delta_hours * 60.0);

  // Membership-metadata field indices for per-leg boarding/alighting.
  // These are deterministic per world; looked up once per call (cheap).
  const int tb_field = world_.getMembershipFieldIndex("t_board_min");
  const int ta_field = world_.getMembershipFieldIndex("t_alight_min");
  const bool have_timing = (tb_field >= 0 && ta_field >= 0);

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
    float eff_board;
    float eff_alight;
  };
  std::vector<LocalLeg> local_legs;
  local_legs.reserve(locations.size() / 64);

  // Per-rider scratch reused across iterations to avoid per-person
  // allocations. The full leg list for a single rider is small (commute
  // ≤ 4 legs), so these stay tiny.
  struct RawLeg {
    uint32_t av_idx;
    VenueId venue_id;
    float tb_min;
    float ta_min;
  };
  std::vector<RawLeg> raw_legs;
  std::vector<float> tb_buf, ta_buf;

  for (size_t i = 0; i < locations.size(); ++i) {
    const int16_t act = locations[i].activity_index;
    if (act < 0) continue;  // person inactive this slot

    const Person& person = world_.people[i];

    // Gather every partial-presence leg this rider has on this slot's
    // activity. We pass the full leg list (across all venues) to
    // computePresenceWindows so the proportional policy sees the rider's
    // total journey, not just one venue's piece. The
    // partial-overlap branch and the long-distance compressed branch are
    // both decided here, on the rider's home rank.
    raw_legs.clear();
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
        float tb = 0.0f, ta = slot_duration_min;
        if (have_timing) {
          tb = world_.getMembershipField(av_idx, tb_field);
          ta = world_.getMembershipField(av_idx, ta_field);
          if (tb == WorldState::kMembershipFieldAbsent ||
              ta == WorldState::kMembershipFieldAbsent) {
            // Missing per-leg metadata: degrade to full-slot presence so
            // the leg still participates in bucketing (D14 — never drop
            // a leg).
            tb = 0.0f;
            ta = slot_duration_min;
          }
        }
        raw_legs.push_back({av_idx, vid, tb, ta});
      }
    }
    if (raw_legs.empty()) continue;

    // Stable sort by t_board so sequential placement in the proportional
    // branch follows natural journey order. Venue id is a deterministic
    // tiebreaker on identical t_board (degenerate inputs).
    std::sort(raw_legs.begin(), raw_legs.end(),
              [](const RawLeg& a, const RawLeg& b) {
                if (a.tb_min != b.tb_min) return a.tb_min < b.tb_min;
                return a.venue_id < b.venue_id;
              });

    tb_buf.resize(raw_legs.size());
    ta_buf.resize(raw_legs.size());
    for (size_t j = 0; j < raw_legs.size(); ++j) {
      tb_buf[j] = raw_legs[j].tb_min;
      ta_buf[j] = raw_legs[j].ta_min;
    }
    auto windows = computePresenceWindows(tb_buf.data(), ta_buf.data(),
                                          raw_legs.size(), slot_duration_min);

    for (size_t j = 0; j < raw_legs.size(); ++j) {
      local_legs.push_back(LocalLeg{raw_legs[j].av_idx, raw_legs[j].venue_id,
                                    person.id, windows[j].eff_board,
                                    windows[j].eff_alight});
    }
  }

  // -------------------------------------------------------------------------
  // Phase 2 — exchange (venue_id, person_id) pairs across ranks so every
  // rank has the same global rider list per partial-presence venue.
  // Multi-leg riders contribute one pair per leg (same person_id may
  // appear in multiple venues' lists — that's the point).
  // -------------------------------------------------------------------------
  // 4 int32s per leg: (vid, pid, bitcast(eff_board_f32),
  // bitcast(eff_alight_f32)). Floats are bit-cast through int32 so the existing
  // int-typed Allgatherv ships them unchanged. Every rank receives the SAME
  // bytes the home rank packed → bit-identical float values everywhere, no
  // FP-recomputation drift.
  static_assert(sizeof(float) == sizeof(int32_t),
                "RuntimeBinAllocator assumes 32-bit float for window packing");
  std::vector<int32_t> local_packed;
  local_packed.reserve(local_legs.size() * 4);
  for (const auto& lg : local_legs) {
    int32_t eb_bits, ea_bits;
    std::memcpy(&eb_bits, &lg.eff_board, sizeof(int32_t));
    std::memcpy(&ea_bits, &lg.eff_alight, sizeof(int32_t));
    local_packed.push_back(static_cast<int32_t>(lg.venue_id));
    local_packed.push_back(static_cast<int32_t>(lg.person_id));
    local_packed.push_back(eb_bits);
    local_packed.push_back(ea_bits);
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
  // Phase 3 — decode the global 4-int-per-leg stream, group by venue,
  // canonical-sort, deal into bins. Also populates windows_by_vid_pid_ from
  // the broadcast float bytes (every rank sees identical windows for the
  // same rider — required for MPI determinism on cross-LGU venues).
  // -------------------------------------------------------------------------
  std::unordered_map<VenueId, std::vector<PersonId>> riders_by_venue;
  windows_by_vid_pid_.reserve(global_packed.size() / 4);
  for (size_t i = 0; i + 3 < global_packed.size(); i += 4) {
    VenueId v = static_cast<VenueId>(global_packed[i]);
    PersonId p = static_cast<PersonId>(global_packed[i + 1]);
    float eb, ea;
    std::memcpy(&eb, &global_packed[i + 2], sizeof(float));
    std::memcpy(&ea, &global_packed[i + 3], sizeof(float));
    riders_by_venue[v].push_back(p);
    const uint64_t key =
        (static_cast<uint64_t>(v) << 32) | static_cast<uint64_t>(p);
    windows_by_vid_pid_[key] = EffectiveWindow{eb, ea};
  }

  // (venue_id, person_id) → bin. The same person on multiple venues gets a
  // separate bin per venue (multi-leg). For local riders we then look this
  // up by their leg's (vid, pid) to populate bin_by_av_idx_.
  // bin_by_vid_pid_ is a member so the FOI loop (which only has
  // InteractionMember = {pid, ...}) can look up bins without the flat av_idx.
  bin_by_vid_pid_.reserve(global_packed.size() / 2);
  num_bins_by_venue_.reserve(riders_by_venue.size());

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
    num_bins_by_venue_[vid] = static_cast<uint16_t>(num_bins);

    for (size_t pos = 0; pos < riders.size(); ++pos) {
      const PersonId pid = riders[pos];
      const uint16_t bin = static_cast<uint16_t>(pos % num_bins);
      const uint64_t key =
          (static_cast<uint64_t>(vid) << 32) | static_cast<uint64_t>(pid);
      bin_by_vid_pid_[key] = bin;
    }
  }

  // -------------------------------------------------------------------------
  // Phase 4 — write per-leg bins into the sparse av_idx map.
  // -------------------------------------------------------------------------
  bin_by_av_idx_.reserve(local_legs.size());
  for (const auto& lg : local_legs) {
    const uint64_t key = (static_cast<uint64_t>(lg.venue_id) << 32) |
                         static_cast<uint64_t>(lg.person_id);
    auto it = bin_by_vid_pid_.find(key);
    bin_by_av_idx_[lg.av_idx] =
        (it == bin_by_vid_pid_.end()) ? kNoBin : it->second;
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
