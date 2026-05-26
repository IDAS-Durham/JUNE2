// Checkpoint writer (P3). Serializes all mutable simulation state into an
// atomic, rank-count-independent checkpoint directory. The restore/overlay
// path lands in P4.

#include <H5Cpp.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "simulation/simulator.h"

#ifdef USE_MPI
#include <mpi.h>
#endif

namespace june {
namespace {

namespace fs = std::filesystem;

std::string pad3(int d) {
  std::string s = std::to_string(d);
  while (s.size() < 3) s = "0" + s;
  return s;
}

std::string ymd(const std::string& iso) {  // "YYYY-MM-DD" -> "YYYYMMDD"
  std::string o;
  for (char c : iso)
    if (c != '-') o += c;
  return o;
}

// Create + write a 1-D numeric dataset. Handles the empty case (no chunk /
// no filter, which HDF5 requires for zero-extent datasets).
template <typename T>
void writeVec(H5::H5File& f, const std::string& name,
              const std::vector<T>& data, const H5::PredType& t,
              int compression) {
  hsize_t dims[1] = {data.size()};
  H5::DataSpace space(1, dims);
  if (data.empty()) {
    f.createDataSet(name, t, space);
    return;
  }
  H5::DSetCreatPropList plist;
  hsize_t chunk[1] = {std::min<hsize_t>(dims[0], 100000)};
  plist.setChunk(1, chunk);
  if (compression > 0) plist.setDeflate(compression);
  H5::DataSet ds = f.createDataSet(name, t, space, plist);
  ds.write(data.data(), t);
}

void writeStrs(H5::H5File& f, const std::string& name,
               const std::vector<std::string>& v) {
  H5::StrType st(H5::PredType::C_S1, H5T_VARIABLE);
  hsize_t dims[1] = {v.size()};
  H5::DataSpace space(1, dims);
  if (v.empty()) {
    f.createDataSet(name, st, space);
    return;
  }
  std::vector<const char*> ptrs;
  ptrs.reserve(v.size());
  for (auto& s : v) ptrs.push_back(s.c_str());
  H5::DataSet ds = f.createDataSet(name, st, space);
  ds.write(ptrs.data(), st);
}

// Stable, rank-count-independent order over this rank's owned people:
// (geo_unit_id, person_id). Contiguous geo_unit runs in the resulting order
// become the embedded partition_index in the shard, mirroring the world file
// so restore can selectively read.
std::vector<uint32_t> buildOwnedPersonOrder(const std::vector<Person>& people) {
  std::vector<uint32_t> ord(people.size());
  for (uint32_t i = 0; i < ord.size(); ++i) ord[i] = i;
  std::sort(ord.begin(), ord.end(), [&](uint32_t a, uint32_t b) {
    const auto& pa = people[a];
    const auto& pb = people[b];
    if (pa.geo_unit_id != pb.geo_unit_id)
      return pa.geo_unit_id < pb.geo_unit_id;
    return pa.id < pb.id;
  });
  return ord;
}

// Gather + write the dense per-person shard section under /population/, plus
// the embedded partition_index over contiguous geo_unit runs (so restore can
// selectively read by geo without a full population scan). Rows follow `ord`
// (sorted by geo_unit_id, person_id) so the run-based index aligns.
void writeShardPopulation(H5::H5File& f, const std::vector<uint32_t>& ord,
                          const std::vector<Person>& people, int comp) {
  std::vector<int32_t> ids, geo, hop, ret, tslot;
  std::vector<double> imm_l, imm_a, imm_w, death_t;
  std::vector<uint8_t> dead;
  std::vector<uint32_t> m_as, m_at, m_ps, m_pt, m_ds, m_dt;
  ids.reserve(ord.size());
  for (uint32_t idx : ord) {
    const Person& p = people[idx];
    ids.push_back(p.id);
    geo.push_back(p.geo_unit_id);
    imm_l.push_back(p.immunity.natural_level);
    imm_a.push_back(p.immunity.natural_acquisition_time);
    imm_w.push_back(p.immunity.natural_waning_rate);
    dead.push_back(p.is_dead ? 1 : 0);
    death_t.push_back(p.death_time);
    m_as.push_back(p.applicable_symptom_policy_mask);
    m_at.push_back(p.applicable_temporal_policy_mask);
    m_ps.push_back(p.active_symptom_policy_participation);
    m_pt.push_back(p.active_temporal_policy_participation);
    m_ds.push_back(p.symptom_policy_decisions);
    m_dt.push_back(p.temporal_policy_decisions);
    hop.push_back(p.hopped_schedule_id);
    ret.push_back(p.return_schedule_id);
    tslot.push_back(p.temp_slot_progress);
  }
  std::vector<int32_t> pi_gu, pi_start, pi_count;
  for (size_t i = 0; i < geo.size();) {
    size_t j = i;
    while (j < geo.size() && geo[j] == geo[i]) ++j;
    pi_gu.push_back(geo[i]);
    pi_start.push_back(static_cast<int32_t>(i));
    pi_count.push_back(static_cast<int32_t>(j - i));
    i = j;
  }
  const auto I32 = H5::PredType::NATIVE_INT32;
  const auto U32 = H5::PredType::NATIVE_UINT32;
  const auto U8 = H5::PredType::NATIVE_UINT8;
  const auto F64 = H5::PredType::NATIVE_DOUBLE;
  writeVec(f, "/population/ids", ids, I32, comp);
  writeVec(f, "/population/geo_unit_ids", geo, I32, comp);
  writeVec(f, "/population/partition_index/geo_unit_ids", pi_gu, I32, comp);
  writeVec(f, "/population/partition_index/start_indices", pi_start, I32, comp);
  writeVec(f, "/population/partition_index/counts", pi_count, I32, comp);
  writeVec(f, "/population/immunity_natural_level", imm_l, F64, comp);
  writeVec(f, "/population/immunity_natural_acquisition_time", imm_a, F64,
           comp);
  writeVec(f, "/population/immunity_natural_waning_rate", imm_w, F64, comp);
  writeVec(f, "/population/is_dead", dead, U8, comp);
  writeVec(f, "/population/death_time", death_t, F64, comp);
  writeVec(f, "/population/applicable_symptom_policy_mask", m_as, U32, comp);
  writeVec(f, "/population/applicable_temporal_policy_mask", m_at, U32, comp);
  writeVec(f, "/population/active_symptom_policy_participation", m_ps, U32,
           comp);
  writeVec(f, "/population/active_temporal_policy_participation", m_pt, U32,
           comp);
  writeVec(f, "/population/symptom_policy_decisions", m_ds, U32, comp);
  writeVec(f, "/population/temporal_policy_decisions", m_dt, U32, comp);
  writeVec(f, "/population/hopped_schedule_id", hop, I32, comp);
  writeVec(f, "/population/return_schedule_id", ret, I32, comp);
  writeVec(f, "/population/temp_slot_progress", tslot, I32, comp);
}

// Gather + write the sparse infection shard section. One record per infected
// person (in sorted `ord` order); the per-person symptom trajectory transitions
// sit at /infection/traj_times[offset..offset+count). Sorted person order is
// preserved so reads land in the same layout the partition_index advertises.
void writeShardInfection(H5::H5File& f, const std::vector<uint32_t>& ord,
                         const std::vector<Person>& people, int comp) {
  std::vector<int32_t> inf_pid;
  std::vector<double> inf_t, inf_mi, inf_sh, inf_ra, inf_sf, inf_tt, inf_tf,
      inf_lc, inf_cs;
  std::vector<uint16_t> inf_csym;
  std::vector<int64_t> traj_off;
  std::vector<int32_t> traj_cnt;
  std::vector<double> traj_time;
  std::vector<uint16_t> traj_sym;
  for (uint32_t idx : ord) {
    const Person& p = people[idx];
    if (!p.infection) continue;
    const Infection& in = *p.infection;
    const auto& tr = in.getTrajectory();
    inf_pid.push_back(p.id);
    inf_t.push_back(in.getInfectionTime());
    inf_mi.push_back(in.ckptMaxInfectiousness());
    inf_sh.push_back(in.ckptTransmissionShape());
    inf_ra.push_back(in.ckptTransmissionRate());
    inf_sf.push_back(in.ckptTransmissionShift());
    inf_tt.push_back(tr.infection_time);
    inf_tf.push_back(tr.infectiousness_factor);
    inf_lc.push_back(in.ckptLastCheckedTime());
    inf_csym.push_back(in.ckptCachedSymptomId());
    inf_cs.push_back(in.ckptCachedSymptomStartTime());
    traj_off.push_back(static_cast<int64_t>(traj_time.size()));
    traj_cnt.push_back(static_cast<int32_t>(tr.transitions.size()));
    for (auto& [tt, sid] : tr.transitions) {
      traj_time.push_back(tt);
      traj_sym.push_back(sid);
    }
  }
  const auto I32 = H5::PredType::NATIVE_INT32;
  const auto I64 = H5::PredType::NATIVE_INT64;
  const auto U16 = H5::PredType::NATIVE_UINT16;
  const auto F64 = H5::PredType::NATIVE_DOUBLE;
  writeVec(f, "/infection/person_id", inf_pid, I32, comp);
  writeVec(f, "/infection/infection_time", inf_t, F64, comp);
  writeVec(f, "/infection/max_infectiousness", inf_mi, F64, comp);
  writeVec(f, "/infection/transmission_shape", inf_sh, F64, comp);
  writeVec(f, "/infection/transmission_rate", inf_ra, F64, comp);
  writeVec(f, "/infection/transmission_shift", inf_sf, F64, comp);
  writeVec(f, "/infection/traj_infection_time", inf_tt, F64, comp);
  writeVec(f, "/infection/traj_infectiousness_factor", inf_tf, F64, comp);
  writeVec(f, "/infection/last_checked_time", inf_lc, F64, comp);
  writeVec(f, "/infection/cached_symptom_id", inf_csym, U16, comp);
  writeVec(f, "/infection/cached_symptom_start_time", inf_cs, F64, comp);
  writeVec(f, "/infection/traj_offsets", traj_off, I64, comp);
  writeVec(f, "/infection/traj_counts", traj_cnt, I32, comp);
  writeVec(f, "/infection/traj_times", traj_time, F64, comp);
  writeVec(f, "/infection/traj_symptom_ids", traj_sym, U16, comp);
}

// Gather + write the sparse vaccine-trajectory shard section. One record per
// vaccinated person (in sorted `ord` order); per-dose fields sit at
// /vaccine/dose_*[offset..offset+count). Efficacy is re-derived from config
// on restore, so it is not written here.
void writeShardVaccine(H5::H5File& f, const std::vector<uint32_t>& ord,
                       const std::vector<Person>& people, int comp) {
  std::vector<int32_t> vx_pid;
  std::vector<std::string> vx_name;
  std::vector<int64_t> dose_off;
  std::vector<int32_t> dose_cnt, dose_num;
  std::vector<double> d_admin, d_eff, d_wane, d_fin, d_wfac;
  for (uint32_t idx : ord) {
    const Person& p = people[idx];
    if (!p.vaccine_trajectory) continue;
    const auto& vt = *p.vaccine_trajectory;
    vx_pid.push_back(p.id);
    vx_name.push_back(vt.vaccine_name);
    dose_off.push_back(static_cast<int64_t>(dose_num.size()));
    dose_cnt.push_back(static_cast<int32_t>(vt.doses.size()));
    for (const auto& d : vt.doses) {
      dose_num.push_back(d.number);
      d_admin.push_back(d.day_administered);
      d_eff.push_back(d.days_to_effective);
      d_wane.push_back(d.days_to_waning);
      d_fin.push_back(d.days_to_finished);
      d_wfac.push_back(d.waning_factor);
    }
  }
  const auto I32 = H5::PredType::NATIVE_INT32;
  const auto I64 = H5::PredType::NATIVE_INT64;
  const auto F64 = H5::PredType::NATIVE_DOUBLE;
  writeVec(f, "/vaccine/person_id", vx_pid, I32, comp);
  writeStrs(f, "/vaccine/vaccine_name", vx_name);
  writeVec(f, "/vaccine/dose_offsets", dose_off, I64, comp);
  writeVec(f, "/vaccine/dose_counts", dose_cnt, I32, comp);
  writeVec(f, "/vaccine/dose_number", dose_num, I32, comp);
  writeVec(f, "/vaccine/dose_day_administered", d_admin, F64, comp);
  writeVec(f, "/vaccine/dose_days_to_effective", d_eff, F64, comp);
  writeVec(f, "/vaccine/dose_days_to_waning", d_wane, F64, comp);
  writeVec(f, "/vaccine/dose_days_to_finished", d_fin, F64, comp);
  writeVec(f, "/vaccine/dose_waning_factor", d_wfac, F64, comp);
}

// Gather + write last_processed_transition_time_ as two parallel arrays under
// /epidemiology/. Per-rank, global-id-keyed manager state — lives in the
// shard, not state.h5, so resume at a different rank count keeps every
// rank's entries.
void writeShardEpidemiology(H5::H5File& f, Epidemiology* epi, int comp) {
  std::vector<int32_t> lpt_pid;
  std::vector<double> lpt_t;
  if (epi) {
    for (const auto& [pid, t] : epi->getLastProcessedTransitionTimes()) {
      lpt_pid.push_back(pid);
      lpt_t.push_back(t);
    }
  }
  f.createGroup("/epidemiology");
  writeVec(f, "/epidemiology/lpt_person_id", lpt_pid,
           H5::PredType::NATIVE_INT32, comp);
  writeVec(f, "/epidemiology/lpt_time", lpt_t, H5::PredType::NATIVE_DOUBLE,
           comp);
}

// Gather + write the policy manager's frozen_states_ under /policy_frozen_states.
// Per-rank, global-id-keyed manager state — lives in the shard, not state.h5.
void writeShardFrozenStates(H5::H5File& f, PolicyManager* pm, int comp) {
  std::vector<int32_t> fz_pid, fz_hop, fz_ret, fz_venue, fz_subset;
  std::vector<uint8_t> fz_pol;
  if (pm) {
    for (const auto& [pid, st] : pm->getFrozenStates()) {
      fz_pid.push_back(pid);
      fz_pol.push_back(st.triggering_policy_index);
      fz_hop.push_back(st.paused_hopped_schedule_id);
      fz_ret.push_back(st.paused_return_schedule_id);
      fz_venue.push_back(st.pin_venue_id);
      fz_subset.push_back(st.pin_subset_index);
    }
  }
  const auto I32 = H5::PredType::NATIVE_INT32;
  const auto U8 = H5::PredType::NATIVE_UINT8;
  f.createGroup("/policy_frozen_states");
  writeVec(f, "/policy_frozen_states/person_id", fz_pid, I32, comp);
  writeVec(f, "/policy_frozen_states/triggering_policy_index", fz_pol, U8,
           comp);
  writeVec(f, "/policy_frozen_states/paused_hopped_schedule_id", fz_hop, I32,
           comp);
  writeVec(f, "/policy_frozen_states/paused_return_schedule_id", fz_ret, I32,
           comp);
  writeVec(f, "/policy_frozen_states/pin_venue_id", fz_venue, I32, comp);
  writeVec(f, "/policy_frozen_states/pin_subset_index", fz_subset, I32, comp);
}

// Gather + write the sparse venue fomite-history shard section. One record
// per (venue, mode) with a non-empty deposit deque; the contiguous deposits
// for each record sit at /venue_fomite/deposit_time[offset..offset+count).
void writeShardFomite(H5::H5File& f, const std::vector<Venue>& venues,
                      int comp) {
  std::vector<int32_t> fo_vid, fo_mode;
  std::vector<int64_t> fo_off;
  std::vector<int32_t> fo_cnt;
  std::vector<double> fo_time, fo_amt;
  for (const Venue& v : venues) {
    for (size_t m = 0; m < v.fomite_history.size(); ++m) {
      const auto& dq = v.fomite_history[m];
      if (dq.empty()) continue;
      fo_vid.push_back(v.id);
      fo_mode.push_back(static_cast<int32_t>(m));
      fo_off.push_back(static_cast<int64_t>(fo_time.size()));
      fo_cnt.push_back(static_cast<int32_t>(dq.size()));
      for (const auto& de : dq) {
        fo_time.push_back(de.time);
        fo_amt.push_back(de.amount);
      }
    }
  }
  const auto I32 = H5::PredType::NATIVE_INT32;
  const auto I64 = H5::PredType::NATIVE_INT64;
  const auto F64 = H5::PredType::NATIVE_DOUBLE;
  writeVec(f, "/venue_fomite/venue_id", fo_vid, I32, comp);
  writeVec(f, "/venue_fomite/mode_index", fo_mode, I32, comp);
  writeVec(f, "/venue_fomite/offsets", fo_off, I64, comp);
  writeVec(f, "/venue_fomite/counts", fo_cnt, I32, comp);
  writeVec(f, "/venue_fomite/deposit_time", fo_time, F64, comp);
  writeVec(f, "/venue_fomite/deposit_amount", fo_amt, F64, comp);
}

// Atomic commit: rename the staging "<root>.tmp" dir into place, refresh the
// "latest" symlink, then drop the oldest checkpoints down to keep_last.
// Returns true on a successful commit (rename succeeded); false otherwise so
// the caller can skip the post-commit log line.
bool commitAndRotate(const fs::path& tmp, const fs::path& cp_root,
                     const fs::path& cp_parent, const std::string& cp_name,
                     int keep_last) {
  std::error_code ec;
  fs::remove_all(cp_root, ec);
  fs::rename(tmp, cp_root, ec);
  if (ec) {
    std::cerr << "[checkpoint] ERROR: commit rename failed: " << ec.message()
              << std::endl;
    return false;
  }
  fs::path latest = cp_parent / "latest";
  fs::remove(latest, ec);
  fs::create_symlink(cp_name, latest, ec);

  if (keep_last > 0) {
    std::vector<fs::path> cps;
    for (auto& de : fs::directory_iterator(cp_parent)) {
      if (!de.is_directory()) continue;
      std::string n = de.path().filename().string();
      if (n.rfind("checkpoint_", 0) == 0 && n.find(".tmp") == std::string::npos)
        cps.push_back(de.path());
    }
    std::sort(cps.begin(), cps.end());
    int remove_n = static_cast<int>(cps.size()) - keep_last;
    for (int i = 0; i < remove_n; ++i) fs::remove_all(cps[i], ec);
  }
  return true;
}

// Re-reads each shard's embedded partition_index and emits a YAML mapping of
// (rank -> [(geo_unit, start, count), ...]) so restore can locate per-geo
// rows in any shard without scanning the population dataset.
void writeShardIndex(const fs::path& tmp, int nranks) {
  YAML::Emitter e;
  e << YAML::BeginMap;
  e << YAML::Key << "num_ranks" << YAML::Value << nranks;
  e << YAML::Key << "shards" << YAML::Value << YAML::BeginSeq;
  for (int r = 0; r < nranks; ++r) {
    std::string sf = "delta_rank" + std::to_string(r) + ".h5";
    H5::H5File f((tmp / sf).string(), H5F_ACC_RDONLY);
    auto rd = [&](const std::string& n) {
      H5::DataSet d = f.openDataSet(n);
      hsize_t dim[1];
      d.getSpace().getSimpleExtentDims(dim);
      std::vector<int32_t> v(dim[0]);
      if (dim[0]) d.read(v.data(), H5::PredType::NATIVE_INT32);
      return v;
    };
    auto gu = rd("/population/partition_index/geo_unit_ids");
    auto st = rd("/population/partition_index/start_indices");
    auto ct = rd("/population/partition_index/counts");
    f.close();
    e << YAML::BeginMap;
    e << YAML::Key << "file" << YAML::Value << sf;
    e << YAML::Key << "geo_units" << YAML::Value << YAML::BeginSeq;
    for (size_t i = 0; i < gu.size(); ++i) {
      e << YAML::Flow << YAML::BeginSeq << gu[i] << st[i] << ct[i]
        << YAML::EndSeq;
    }
    e << YAML::EndSeq << YAML::EndMap;
  }
  e << YAML::EndSeq << YAML::EndMap;
  std::ofstream(tmp / "shard_index.yaml") << e.c_str() << "\n";
}

// manifest.yaml is written LAST in the checkpoint dir — its presence is the
// atomic commit marker for a complete checkpoint (see restoreFromCheckpoint).
// world_path / world_sha256 are populated in P4 (restore validation);
// intentionally empty here rather than a misleading placeholder.
void writeManifest(const fs::path& tmp, int completed_day,
                   const std::string& date_iso, int nranks,
                   double current_simulation_time, unsigned int random_seed) {
  YAML::Emitter m;
  m << YAML::BeginMap;
  m << YAML::Key << "format_version" << YAML::Value << 1;
  m << YAML::Key << "completed_day" << YAML::Value << completed_day;
  m << YAML::Key << "date" << YAML::Value << date_iso;
  m << YAML::Key << "current_simulation_time" << YAML::Value
    << current_simulation_time;
  m << YAML::Key << "num_ranks" << YAML::Value << nranks;
  m << YAML::Key << "effective_random_seed" << YAML::Value
    << static_cast<unsigned long long>(random_seed);
  m << YAML::Key << "world_path" << YAML::Value << "";
  m << YAML::Key << "world_sha256" << YAML::Value << "";
  m << YAML::Key << "shard_index" << YAML::Value << "shard_index.yaml";
  m << YAML::Key << "state_file" << YAML::Value << "state.h5";
  m << YAML::EndMap;
  std::ofstream(tmp / "manifest.yaml") << m.c_str() << "\n";
}

}  // namespace

void Simulator::writeCheckpoint(int completed_day,
                                const std::string& date_iso) {
  const int rank = getRank();
  const int nranks = getNumRanks();
  const auto& ck = config_.simulation.checkpoint;
  const int comp = config_.simulation.compression_level;

  fs::path run_dir = fs::path(events_filename_).parent_path();
  std::string cp_name =
      "checkpoint_" + ymd(date_iso) + "_day" + pad3(completed_day);
  fs::path cp_parent = run_dir / ck.output_dir;
  fs::path cp_root = cp_parent / cp_name;
  fs::path tmp = cp_parent / (cp_name + ".tmp");

  if (rank == 0) {
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);
  }
#ifdef USE_MPI
  if (domain_mgr_) MPI_Barrier(MPI_COMM_WORLD);
#endif

  writeCheckpointRankShard(tmp, rank, comp);

#ifdef USE_MPI
  if (domain_mgr_) MPI_Barrier(MPI_COMM_WORLD);
#endif

  if (rank != 0) return;

  writeCheckpointStateFile(tmp, completed_day, nranks);

  writeShardIndex(tmp, nranks);

  writeManifest(tmp, completed_day, date_iso, nranks,
                current_simulation_time_, config_.simulation.random_seed);

  if (!commitAndRotate(tmp, cp_root, cp_parent, cp_name, ck.keep_last)) return;

  std::cout << "[checkpoint] wrote " << cp_root.string() << " (day "
            << completed_day << ", " << date_iso << ", " << nranks
            << " shard(s))" << std::endl;
}

void Simulator::writeCheckpointRankShard(const fs::path& tmp, int rank,
                                         int comp) {
  const std::vector<uint32_t> ord = buildOwnedPersonOrder(world_.people);

  fs::path shard = tmp / ("delta_rank" + std::to_string(rank) + ".h5");
  H5::H5File f(shard.string(), H5F_ACC_TRUNC);
  f.createGroup("/population");
  f.createGroup("/population/partition_index");
  f.createGroup("/infection");
  f.createGroup("/vaccine");
  f.createGroup("/venue_fomite");

  writeShardPopulation(f, ord, world_.people, comp);
  writeShardInfection(f, ord, world_.people, comp);
  writeShardVaccine(f, ord, world_.people, comp);
  writeShardFomite(f, world_.venues, comp);

  writeShardEpidemiology(f, epidemiology_.get(), comp);
  writeShardFrozenStates(f, policy_manager_.get(), comp);
  f.close();
}

void Simulator::writeCheckpointStateFile(const fs::path& tmp, int completed_day,
                                         int nranks) {
  H5::H5File f((tmp / "state.h5").string(), H5F_ACC_TRUNC);
  f.createGroup("/scalars");
  f.createGroup("/infection_seeder");
  f.createGroup("/event_log");
  const auto I32 = H5::PredType::NATIVE_INT32;

  writeVec(f, "/scalars/completed_day", std::vector<int32_t>{completed_day},
           I32, 0);
  writeVec(f, "/scalars/num_ranks", std::vector<int32_t>{nranks}, I32, 0);
  writeVec(f, "/scalars/current_simulation_time",
           std::vector<double>{current_simulation_time_},
           H5::PredType::NATIVE_DOUBLE, 0);
  writeVec(f, "/scalars/next_encounter_group_id",
           std::vector<uint64_t>{next_encounter_group_id_},
           H5::PredType::NATIVE_UINT64, 0);
  writeVec(f, "/scalars/effective_random_seed",
           std::vector<uint32_t>{config_.simulation.random_seed},
           H5::PredType::NATIVE_UINT32, 0);
  writeVec(f, "/scalars/day_type_counts", day_type_counts_, I32, 0);

  // applied_seeds_ (must not re-fire on resume)
  std::vector<std::string> seeds;
  if (infection_seeder_)
    for (const auto& s : infection_seeder_->getAppliedSeeds())
      seeds.push_back(s);
  writeStrs(f, "/infection_seeder/applied_seeds", seeds);

  // event-log on-disk durability marker (rank-0 logger record counts)
  std::vector<int64_t> ec = {
      (int64_t)event_logger_.getInfectionCount(),
      (int64_t)event_logger_.getSymptomChangeCount(),
      (int64_t)event_logger_.getDeathCount(),
      (int64_t)event_logger_.getHospitalAdmissionCount(),
      (int64_t)event_logger_.getICUAdmissionCount(),
      (int64_t)event_logger_.getHospitalDischargeCount(),
      (int64_t)event_logger_.getVaccinationCount(),
      (int64_t)event_logger_.getRelationshipCount(),
      (int64_t)event_logger_.getCoordinatedEncounterCount()};
  writeVec(f, "/event_log/rank0_buffered_counts", ec,
           H5::PredType::NATIVE_INT64, 0);
  f.close();
}

namespace {

template <typename T>
std::vector<T> readVec(H5::H5File& f, const std::string& n,
                       const H5::PredType& t) {
  if (!H5Lexists(f.getId(), n.c_str(), H5P_DEFAULT)) return {};
  H5::DataSet d = f.openDataSet(n);
  hsize_t dim[1];
  d.getSpace().getSimpleExtentDims(dim);
  std::vector<T> v(dim[0]);
  if (dim[0]) d.read(v.data(), t);
  return v;
}

std::vector<std::string> readStrs(H5::H5File& f, const std::string& n) {
  if (!H5Lexists(f.getId(), n.c_str(), H5P_DEFAULT)) return {};
  H5::DataSet d = f.openDataSet(n);
  hsize_t dim[1];
  d.getSpace().getSimpleExtentDims(dim);
  std::vector<std::string> out;
  if (!dim[0]) return out;
  H5::StrType st(H5::PredType::C_S1, H5T_VARIABLE);
  std::vector<char*> raw(dim[0]);
  d.read(raw.data(), st);
  for (hsize_t i = 0; i < dim[0]; ++i) out.emplace_back(raw[i] ? raw[i] : "");
  H5::DataSpace sp = d.getSpace();
  H5Dvlen_reclaim(st.getId(), sp.getId(), H5P_DEFAULT, raw.data());
  return out;
}

// Overlay one shard's /infection/ records onto world_.people owned by this
// rank. Each record rebuilds the symptom trajectory from the flat
// traj_times/traj_symptom_ids arrays, then materialises the Infection via
// Infection::fromCheckpoint so the disease config is rebound to the
// currently-loaded Disease pointer. Returns the number of records consumed.
size_t overlayShardInfection(H5::H5File& f, WorldState& world,
                             Disease* disease) {
  const auto I32 = H5::PredType::NATIVE_INT32;
  const auto I64 = H5::PredType::NATIVE_INT64;
  const auto U16 = H5::PredType::NATIVE_UINT16;
  const auto F64 = H5::PredType::NATIVE_DOUBLE;
  auto ip = readVec<int32_t>(f, "/infection/person_id", I32);
  auto it_ = readVec<double>(f, "/infection/infection_time", F64);
  auto mi = readVec<double>(f, "/infection/max_infectiousness", F64);
  auto sh = readVec<double>(f, "/infection/transmission_shape", F64);
  auto ra = readVec<double>(f, "/infection/transmission_rate", F64);
  auto sft = readVec<double>(f, "/infection/transmission_shift", F64);
  auto tt = readVec<double>(f, "/infection/traj_infection_time", F64);
  auto tf = readVec<double>(f, "/infection/traj_infectiousness_factor", F64);
  auto lc = readVec<double>(f, "/infection/last_checked_time", F64);
  auto csy = readVec<uint16_t>(f, "/infection/cached_symptom_id", U16);
  auto css = readVec<double>(f, "/infection/cached_symptom_start_time", F64);
  auto toff = readVec<int64_t>(f, "/infection/traj_offsets", I64);
  auto tcnt = readVec<int32_t>(f, "/infection/traj_counts", I32);
  auto ttime = readVec<double>(f, "/infection/traj_times", F64);
  auto tsym = readVec<uint16_t>(f, "/infection/traj_symptom_ids", U16);
  size_t n = 0;
  for (size_t i = 0; i < ip.size(); ++i) {
    Person* p = world.getPerson(ip[i]);
    if (!p) continue;
    InfectionTrajectory tr;
    tr.infection_time = tt[i];
    tr.infectiousness_factor = tf[i];
    int64_t off = toff[i];
    int32_t cnt = tcnt[i];
    tr.transitions.reserve(cnt);
    for (int32_t k = 0; k < cnt; ++k)
      tr.transitions.emplace_back(ttime[off + k], tsym[off + k]);
    p->infection =
        Infection::fromCheckpoint(disease, it_[i], tr, mi[i], sh[i], ra[i],
                                  sft[i], lc[i], csy[i], css[i]);
    ++n;
  }
  return n;
}

// Overlay one shard's /vaccine/ records onto world_.people owned by this
// rank. Efficacy is NOT persisted in the shard — it is re-derived from the
// active VaccinationConfig by dose position so a resume can apply updated
// efficacy maps. Returns the number of records this rank consumed.
size_t overlayShardVaccine(H5::H5File& f, WorldState& world,
                           const VaccinationConfig& vax_cfg) {
  const auto I32 = H5::PredType::NATIVE_INT32;
  const auto I64 = H5::PredType::NATIVE_INT64;
  const auto F64 = H5::PredType::NATIVE_DOUBLE;
  auto vp = readVec<int32_t>(f, "/vaccine/person_id", I32);
  auto vname = readStrs(f, "/vaccine/vaccine_name");
  auto doff = readVec<int64_t>(f, "/vaccine/dose_offsets", I64);
  auto dcnt = readVec<int32_t>(f, "/vaccine/dose_counts", I32);
  auto dnum = readVec<int32_t>(f, "/vaccine/dose_number", I32);
  auto dad = readVec<double>(f, "/vaccine/dose_day_administered", F64);
  auto deff = readVec<double>(f, "/vaccine/dose_days_to_effective", F64);
  auto dwan = readVec<double>(f, "/vaccine/dose_days_to_waning", F64);
  auto dfin = readVec<double>(f, "/vaccine/dose_days_to_finished", F64);
  auto dwf = readVec<double>(f, "/vaccine/dose_waning_factor", F64);
  size_t n = 0;
  for (size_t i = 0; i < vp.size(); ++i) {
    Person* p = world.getPerson(vp[i]);
    if (!p) continue;
    p->vaccine_trajectory = std::make_unique<VaccineTrajectory>();
    p->vaccine_trajectory->vaccine_name = vname[i];
    auto vit = vax_cfg.vaccines.find(vname[i]);
    int64_t off = doff[i];
    int32_t cnt = dcnt[i];
    for (int32_t k = 0; k < cnt; ++k) {
      Dose d;
      d.number = dnum[off + k];
      d.day_administered = dad[off + k];
      d.days_to_effective = deff[off + k];
      d.days_to_waning = dwan[off + k];
      d.days_to_finished = dfin[off + k];
      d.waning_factor = dwf[off + k];
      if (vit != vax_cfg.vaccines.end() &&
          k < static_cast<int32_t>(vit->second.doses.size())) {
        d.infection_efficacy = vit->second.doses[k].infection_efficacy;
        d.symptom_efficacy = vit->second.doses[k].symptom_efficacy;
      }
      p->vaccine_trajectory->addDose(d);
    }
    ++n;
  }
  return n;
}

// Overlay one shard's /venue_fomite/ records onto world_.venues owned by this
// rank. Each record names (venue_id, mode, offset, count); the mode-keyed
// deque is resized + cleared + repopulated from the flat deposit arrays.
// Returns the number of records this rank consumed (for diagnostics).
size_t overlayShardFomite(H5::H5File& f, WorldState& world) {
  const auto I32 = H5::PredType::NATIVE_INT32;
  const auto I64 = H5::PredType::NATIVE_INT64;
  const auto F64 = H5::PredType::NATIVE_DOUBLE;
  auto fv = readVec<int32_t>(f, "/venue_fomite/venue_id", I32);
  auto fm = readVec<int32_t>(f, "/venue_fomite/mode_index", I32);
  auto fo = readVec<int64_t>(f, "/venue_fomite/offsets", I64);
  auto fc = readVec<int32_t>(f, "/venue_fomite/counts", I32);
  auto ftime = readVec<double>(f, "/venue_fomite/deposit_time", F64);
  auto famt = readVec<double>(f, "/venue_fomite/deposit_amount", F64);
  size_t n = 0;
  for (size_t i = 0; i < fv.size(); ++i) {
    Venue* v = world.getVenue(fv[i]);
    if (!v) continue;
    int m = fm[i];
    if (static_cast<int>(v->fomite_history.size()) <= m)
      v->fomite_history.resize(m + 1);
    auto& dq = v->fomite_history[m];
    dq.clear();
    for (int32_t k = 0; k < fc[i]; ++k)
      dq.push_back({ftime[fo[i] + k], famt[fo[i] + k]});
    ++n;
  }
  return n;
}

// Open the checkpoint's manifest.yaml, enforce that the checkpoint's seed
// matches the configured seed (no silent override), and return the
// completed_day to resume from. The caller has already canonicalised cp.
int readAndValidateManifest(const fs::path& cp, unsigned int config_seed) {
  if (!fs::exists(cp / "manifest.yaml"))
    throw std::runtime_error(
        "restoreFromCheckpoint: no manifest.yaml (incomplete/invalid "
        "checkpoint): " +
        cp.string());
  YAML::Node man = YAML::LoadFile((cp / "manifest.yaml").string());
  int completed_day = man["completed_day"].as<int>();
  unsigned int cp_seed = man["effective_random_seed"].as<unsigned int>();
  if (cp_seed != config_seed) {
    throw std::runtime_error(
        "restoreFromCheckpoint: effective_random_seed mismatch (checkpoint=" +
        std::to_string(cp_seed) +
        ", config=" + std::to_string(config_seed) +
        "). Resume with the checkpoint's seed (no silent override).");
  }
  return completed_day;
}

}  // namespace

void Simulator::restoreCheckpointStateFile(const fs::path& cp) {
  H5::H5File f((cp / "state.h5").string(), H5F_ACC_RDONLY);
  auto F64 = H5::PredType::NATIVE_DOUBLE;
  auto I32 = H5::PredType::NATIVE_INT32;
  auto cst = readVec<double>(f, "/scalars/current_simulation_time", F64);
  if (!cst.empty()) current_simulation_time_ = cst[0];
  auto ng = readVec<uint64_t>(f, "/scalars/next_encounter_group_id",
                              H5::PredType::NATIVE_UINT64);
  if (!ng.empty()) next_encounter_group_id_ = ng[0];
  auto dtc = readVec<int32_t>(f, "/scalars/day_type_counts", I32);
  day_type_counts_.assign(dtc.begin(), dtc.end());

  if (infection_seeder_) {
    auto seeds = readStrs(f, "/infection_seeder/applied_seeds");
    std::set<std::string> s(seeds.begin(), seeds.end());
    infection_seeder_->setAppliedSeeds(s);
  }
}

void Simulator::restoreFromCheckpoint(const std::string& checkpoint_dir) {
  const int rank = getRank();
  fs::path cp = fs::canonical(checkpoint_dir);  // resolves 'latest' symlink
  int completed_day =
      readAndValidateManifest(cp, config_.simulation.random_seed);

  restoreCheckpointStateFile(cp);

  // frozen_states_ + lpt are per-rank: accumulated from the shards below.
  std::unordered_map<PersonId, double> lpt_map;
  std::unordered_map<PersonId, FrozenPersonState> frozen_accum;

  // ---- delta shards: overlay onto this rank's owned world_ by global id ----
  YAML::Node si = YAML::LoadFile((cp / "shard_index.yaml").string());
  int n_shards = si["num_ranks"].as<int>();
  auto F64 = H5::PredType::NATIVE_DOUBLE;
  auto I32 = H5::PredType::NATIVE_INT32;
  auto I64 = H5::PredType::NATIVE_INT64;
  auto U16 = H5::PredType::NATIVE_UINT16;
  auto U8 = H5::PredType::NATIVE_UINT8;
  auto U32 = H5::PredType::NATIVE_UINT32;
  size_t n_people = 0, n_inf = 0, n_vax = 0, n_fom = 0;

  for (int s = 0; s < n_shards; ++s) {
    fs::path sf = cp / ("delta_rank" + std::to_string(s) + ".h5");
    if (!fs::exists(sf)) continue;
    H5::H5File f(sf.string(), H5F_ACC_RDONLY);

    auto ids = readVec<int32_t>(f, "/population/ids", I32);
    auto imm_l = readVec<double>(f, "/population/immunity_natural_level", F64);
    auto imm_a = readVec<double>(
        f, "/population/immunity_natural_acquisition_time", F64);
    auto imm_w =
        readVec<double>(f, "/population/immunity_natural_waning_rate", F64);
    auto dead = readVec<uint8_t>(f, "/population/is_dead", U8);
    auto dt = readVec<double>(f, "/population/death_time", F64);
    auto m_as =
        readVec<uint32_t>(f, "/population/applicable_symptom_policy_mask", U32);
    auto m_at = readVec<uint32_t>(
        f, "/population/applicable_temporal_policy_mask", U32);
    auto m_ps = readVec<uint32_t>(
        f, "/population/active_symptom_policy_participation", U32);
    auto m_pt = readVec<uint32_t>(
        f, "/population/active_temporal_policy_participation", U32);
    auto m_ds =
        readVec<uint32_t>(f, "/population/symptom_policy_decisions", U32);
    auto m_dt =
        readVec<uint32_t>(f, "/population/temporal_policy_decisions", U32);
    auto hop = readVec<int32_t>(f, "/population/hopped_schedule_id", I32);
    auto ret = readVec<int32_t>(f, "/population/return_schedule_id", I32);
    auto tsl = readVec<int32_t>(f, "/population/temp_slot_progress", I32);

    for (size_t i = 0; i < ids.size(); ++i) {
      Person* p = world_.getPerson(ids[i]);
      if (!p) continue;  // not owned by this rank
      ++n_people;
      p->immunity.natural_level = imm_l[i];
      p->immunity.natural_acquisition_time = imm_a[i];
      p->immunity.natural_waning_rate = imm_w[i];
      p->is_dead = dead[i] != 0;
      p->death_time = dt[i];
      p->applicable_symptom_policy_mask = m_as[i];
      p->applicable_temporal_policy_mask = m_at[i];
      p->active_symptom_policy_participation = m_ps[i];
      p->active_temporal_policy_participation = m_pt[i];
      p->symptom_policy_decisions = m_ds[i];
      p->temporal_policy_decisions = m_dt[i];
      p->hopped_schedule_id = static_cast<int16_t>(hop[i]);
      p->return_schedule_id = static_cast<int16_t>(ret[i]);
      p->temp_slot_progress = static_cast<int16_t>(tsl[i]);
      // Clear any default-constructed infection/vaccine; reinstated below.
      p->infection.reset();
      p->vaccine_trajectory.reset();
    }

    n_inf += overlayShardInfection(f, world_, disease_.get());

    n_vax += overlayShardVaccine(f, world_, config_.vaccination);

    n_fom += overlayShardFomite(f, world_);

    // per-rank manager state: keep only entries for people THIS rank owns
    // (rank-count independent — keyed on global PersonId).
    auto lp = readVec<int32_t>(f, "/epidemiology/lpt_person_id", I32);
    auto lt = readVec<double>(f, "/epidemiology/lpt_time", F64);
    for (size_t i = 0; i < lp.size(); ++i)
      if (world_.getPerson(lp[i])) lpt_map[lp[i]] = lt[i];

    auto fzp = readVec<int32_t>(f, "/policy_frozen_states/person_id", I32);
    auto fzpol = readVec<uint8_t>(
        f, "/policy_frozen_states/triggering_policy_index", U8);
    auto fzh = readVec<int32_t>(
        f, "/policy_frozen_states/paused_hopped_schedule_id", I32);
    auto fzr = readVec<int32_t>(
        f, "/policy_frozen_states/paused_return_schedule_id", I32);
    auto fzv = readVec<int32_t>(f, "/policy_frozen_states/pin_venue_id", I32);
    auto fzs =
        readVec<int32_t>(f, "/policy_frozen_states/pin_subset_index", I32);
    for (size_t i = 0; i < fzp.size(); ++i) {
      if (!world_.getPerson(fzp[i])) continue;
      FrozenPersonState st;
      st.triggering_policy_index = fzpol[i];
      st.paused_hopped_schedule_id = static_cast<int16_t>(fzh[i]);
      st.paused_return_schedule_id = static_cast<int16_t>(fzr[i]);
      st.pin_venue_id = fzv[i];
      st.pin_subset_index = fzs[i];
      frozen_accum[fzp[i]] = st;
    }
  }

  if (policy_manager_) policy_manager_->setFrozenStates(frozen_accum);

  // ---- rebuild derived caches; set resume point ----
  if (epidemiology_) epidemiology_->restoreAfterCheckpoint(lpt_map);
  resume_from_day_ = completed_day + 1;

  // --days / end_date is anchored to the ORIGINAL start_date, not to the
  // checkpoint. If the requested end is at or before the resume day the
  // run loop would simply do nothing — fail loudly instead of a silent
  // no-op. To run M more days after a checkpoint completed at day D, use
  // --days (D + 1 + M)  (i.e. total_days must exceed completed_day + 1).
  if (resume_from_day_ >= total_days_) {
    throw std::runtime_error(
        "checkpoint resume: requested simulation length leaves nothing to "
        "simulate. Checkpoint completed day " +
        std::to_string(completed_day) + " so the run resumes at day " +
        std::to_string(resume_from_day_) +
        ", but the configured end is total_days=" +
        std::to_string(total_days_) +
        " (end_date=" + config_.simulation.end_date +
        "). --days/end_date counts from the original start_date, not from "
        "the checkpoint. To run M more day(s) after this checkpoint, pass "
        "--days " +
        std::to_string(completed_day + 2) +
        " or greater (= "
        "completed_day + 1 + M).");
  }

  if (rank == 0) {
    std::cout << "[checkpoint] restored from " << cp.string()
              << ": day=" << completed_day
              << " sim_time=" << current_simulation_time_
              << " people=" << n_people << " infections=" << n_inf
              << " vaccinated=" << n_vax << " fomite_venues=" << n_fom
              << " — resuming at day " << resume_from_day_ << std::endl;
  }
}

}  // namespace june
