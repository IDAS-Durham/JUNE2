#pragma once

#include <yaml-cpp/yaml.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifdef USE_MPI
#include <mpi.h>
#endif

#include <unistd.h>

#include "../core/config.h"

namespace june::run_dir {

// Generate a UTC timestamp run id of the form "YYYYMMDD-HHMMSS".
// Rank 0 should call this and broadcast the result so all ranks agree.
inline std::string generateRunIdUtc() {
  using namespace std::chrono;
  auto now = system_clock::to_time_t(system_clock::now());
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &now);
#else
  gmtime_r(&now, &utc);
#endif
  std::ostringstream oss;
  oss << std::put_time(&utc, "%Y%m%d-%H%M%S");
  return oss.str();
}

// Broadcast the run id from rank 0 to all other MPI ranks. No-op without MPI.
// Run ids are short fixed-shape strings ("YYYYMMDD-HHMMSS" = 15 chars), but
// users can override via --run-id, so we send a length first.
inline void broadcastRunId(std::string& run_id) {
#ifdef USE_MPI
  int initialized = 0;
  MPI_Initialized(&initialized);
  if (!initialized) return;
  int len = static_cast<int>(run_id.size());
  MPI_Bcast(&len, 1, MPI_INT, 0, MPI_COMM_WORLD);
  std::vector<char> buf(len);
  if (!run_id.empty()) std::copy(run_id.begin(), run_id.end(), buf.begin());
  MPI_Bcast(buf.data(), len, MPI_CHAR, 0, MPI_COMM_WORLD);
  run_id.assign(buf.data(), buf.data() + len);
#else
  (void)run_id;
#endif
}

// Broadcast the resolved RNG seed from rank 0 to all ranks. No-op without MPI.
// Ensures every rank (and any future checkpoint resume) uses the identical
// stream even when the seed was auto-generated on rank 0.
inline void broadcastSeed(unsigned int& seed) {
#ifdef USE_MPI
  int initialized = 0;
  MPI_Initialized(&initialized);
  if (!initialized) return;
  MPI_Bcast(&seed, 1, MPI_UNSIGNED, 0, MPI_COMM_WORLD);
#else
  (void)seed;
#endif
}

// Collect every config / data file path referenced by a loaded Config plus
// the simulation.yaml that anchors it. Order is stable; duplicates removed.
inline std::vector<std::string> collectConfigPaths(
    const Config& config, const std::string& sim_yaml_path) {
  std::vector<std::string> out;
  std::set<std::string> seen;
  auto push = [&](const std::string& p) {
    if (p.empty()) return;
    if (!seen.insert(p).second) return;
    out.push_back(p);
  };

  // The simulation.yaml itself first.
  push(sim_yaml_path);

  // Top-level YAMLs declared in config_paths.
  const auto& sim = config.simulation;
  push(sim.disease_file);
  push(sim.contact_matrices_file);
  push(sim.schedules_file);
  push(sim.vaccines_file);
  push(sim.activity_preferences_file);
  push(sim.coordinated_encounters_file);
  push(sim.performance_file);
  push(sim.parallel_file);
  push(sim.policies_file);
  push(sim.infection_seeds_file);

  // Referenced data / CSV files.
  push(sim.regional_risk.regional_risk_file);
  push(config.schedule.csv_path);
  for (const auto& [_, fg] : config.coordinated_encounters.frequency_groups) {
    push(fg.csv_path);
  }

  return out;
}

// Resolve where a referenced file should live inside the run directory.
//   <run_dir>/configs/<rel> for files under dirname(sim_yaml_path)
//   <run_dir>/<original-cwd-path> for files outside that root but inside CWD
//   <run_dir>/_external/<basename> for absolute paths or paths escaping CWD
inline std::filesystem::path snapshotDestination(
    const std::filesystem::path& run_dir,
    const std::filesystem::path& config_root,
    const std::filesystem::path& source) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path rel_to_root = fs::relative(source, config_root, ec);
  if (!ec && !rel_to_root.empty() && !rel_to_root.string().starts_with("..") &&
      !rel_to_root.is_absolute()) {
    return run_dir / "configs" / rel_to_root;
  }
  if (source.is_relative()) {
    fs::path rel = source.lexically_normal();
    if (!rel.string().starts_with("..")) {
      return run_dir / rel;
    }
  }
  return run_dir / "_external" / source.filename();
}

// Snapshot every loaded config/data file into the run directory and write
// a manifest.yaml describing the run. Call only on rank 0. Throws on
// any I/O failure (no silent fallback).
inline void snapshotRun(const std::filesystem::path& run_dir,
                        const std::string& sim_yaml_path, const Config& config,
                        int mpi_size, const std::vector<std::string>& cli_args,
                        unsigned int effective_random_seed,
                        const std::string& world_path) {
  namespace fs = std::filesystem;
  fs::create_directories(run_dir);

  fs::path config_root = fs::path(sim_yaml_path).parent_path();
  if (config_root.empty()) config_root = ".";

  std::vector<std::string> sources = collectConfigPaths(config, sim_yaml_path);

  YAML::Emitter manifest;
  manifest << YAML::BeginMap;
  manifest << YAML::Key << "run_id" << YAML::Value
           << run_dir.filename().string();
  manifest << YAML::Key << "started_utc" << YAML::Value;
  {
    using namespace std::chrono;
    auto now = system_clock::to_time_t(system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::ostringstream oss;
    oss << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    manifest << oss.str();
  }
  manifest << YAML::Key << "mpi_size" << YAML::Value << mpi_size;

  // Hostname (best-effort).
  {
    char host[256] = {0};
    if (gethostname(host, sizeof(host) - 1) == 0) {
      manifest << YAML::Key << "hostname" << YAML::Value << std::string(host);
    }
  }

  // Git provenance (best-effort; absent fields if not in a git tree).
  auto popen_first_line = [](const std::string& cmd) -> std::string {
    std::string out;
    if (FILE* f = popen(cmd.c_str(), "r")) {
      char buf[256];
      if (fgets(buf, sizeof(buf), f)) {
        out = buf;
        if (!out.empty() && out.back() == '\n') out.pop_back();
      }
      pclose(f);
    }
    return out;
  };
  std::string git_sha = popen_first_line("git rev-parse HEAD 2>/dev/null");
  if (!git_sha.empty()) {
    manifest << YAML::Key << "git_sha" << YAML::Value << git_sha;
    std::string dirty =
        popen_first_line("git status --porcelain 2>/dev/null | head -1");
    manifest << YAML::Key << "git_dirty" << YAML::Value << !dirty.empty();
  }

  manifest << YAML::Key << "cli_args" << YAML::Value << YAML::Flow
           << YAML::BeginSeq;
  for (const auto& a : cli_args) manifest << a;
  manifest << YAML::EndSeq;

  manifest << YAML::Key << "config_root" << YAML::Value << config_root.string();

  // Lineage: everything needed to reproduce or resume this run. The effective
  // seed is the resolved value actually fed to the RNG (incl. the
  // auto-generated case), so a checkpoint restart can reuse the identical
  // stream. resumed_from is null for an original run; populated on restart.
  manifest << YAML::Key << "lineage" << YAML::Value << YAML::BeginMap;
  manifest << YAML::Key << "world_path" << YAML::Value << world_path;
  manifest << YAML::Key << "effective_random_seed" << YAML::Value
           << static_cast<unsigned long long>(effective_random_seed);
  manifest << YAML::Key << "resumed_from" << YAML::Value << YAML::Null;
  manifest << YAML::EndMap;

  manifest << YAML::Key << "files" << YAML::Value << YAML::BeginSeq;
  for (const auto& src : sources) {
    if (!fs::exists(src)) {
      throw std::runtime_error(
          "snapshotRun: referenced config/data file does not exist: '" + src +
          "'");
    }
    fs::path dst = snapshotDestination(run_dir, config_root, src);
    fs::create_directories(dst.parent_path());
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing);

    auto sz = fs::file_size(dst);
    manifest << YAML::BeginMap;
    manifest << YAML::Key << "original" << YAML::Value << src;
    manifest << YAML::Key << "snapshot" << YAML::Value
             << fs::relative(dst, run_dir).string();
    manifest << YAML::Key << "size" << YAML::Value
             << static_cast<unsigned long long>(sz);
    manifest << YAML::EndMap;
  }
  manifest << YAML::EndSeq;
  manifest << YAML::EndMap;

  std::ofstream mf(run_dir / "manifest.yaml");
  if (!mf) {
    throw std::runtime_error("snapshotRun: cannot write manifest.yaml in '" +
                             run_dir.string() + "'");
  }
  mf << manifest.c_str() << "\n";
}

}  // namespace june::run_dir
