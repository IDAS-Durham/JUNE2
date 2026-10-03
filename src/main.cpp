#include <getopt.h>
#include <yaml-cpp/yaml.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "loaders/config_loader.h"
#include "loaders/hdf5_loader.h"
#include "simulation/compartmental_model_manager.h"
#include "simulation/simulator.h"
#include "utils/event_logging/event_logger.h"
#include "utils/event_logging/event_merger.h"
#include "utils/memory_utils.h"
#include "utils/run_dir.h"

#ifdef USE_MPI
#include <mpi.h>

#include "parallel/domain_manager.h"
#endif

// gperftools CPU profiler - optional
#ifdef USE_GPERFTOOLS
#include <gperftools/profiler.h>
#endif

using namespace june;

#ifdef USE_GPERFTOOLS
namespace {

void startCpuProfiler(int rank, const std::filesystem::path& profile_path,
                      bool mpi_mode) {
  if (rank != 0) return;

  const char* profile_env = std::getenv("CPUPROFILE");
  if (profile_env) {
    std::cout << "\n[CPU profiling active (via CPUPROFILE env var): "
              << profile_env << "]" << std::endl;
    return;
  }

  if (mpi_mode) {
    std::cout << "\n[Starting CPU profiling for Rank 0 to "
              << profile_path.string() << "...]" << std::endl;
  } else {
    std::cout << "\n[Starting CPU profiling to " << profile_path.string()
              << "...]" << std::endl;
  }
  ProfilerStart(profile_path.string().c_str());
}

void stopCpuProfiler(int rank, const std::filesystem::path& profile_path,
                     bool mpi_mode) {
  if (rank != 0) return;

  ProfilerStop();
  if (!mpi_mode) std::cout << "\n";
  std::cout << "[Profiling stopped. Saved to " << profile_path.string() << "]"
            << std::endl;
}

}  // namespace
#endif

int main(int argc, char* argv[]) {
#ifdef USE_MPI
  MPI_Init(&argc, &argv);

  int rank, size;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);
#else
  int rank = 0;
  int size = 1;
#endif

  std::string filename;
  std::string sim_config_file;
  std::string infection_seeds_file =
      "";  // empty = use value from simulation.yaml
  bool infection_seeds_cli_override = false;
  std::string runs_dir = "runs";
  std::string run_id_override = "";
  std::string restart_from = "";  // checkpoint dir to resume from (P4)
  int days_override = -1;
  // Wide enough to hold the full unsigned 32-bit seed range while keeping -1
  // as the "not provided" sentinel. Auto-generated seeds routinely exceed
  // INT_MAX, and must be feedable back via --seed for reproducible restart.
  long long seed_override = -1;
  std::vector<std::string> cli_args(argv + 1, argv + argc);

  static const struct option long_options[] = {
      {"infection_seeds", required_argument, nullptr, 'i'},
      {"config", required_argument, nullptr, 'c'},
      {"seed", required_argument, nullptr, 's'},
      {"runs-dir", required_argument, nullptr, 'r'},
      {"run-id", required_argument, nullptr, 'u'},
      {"restart-from", required_argument, nullptr, 'R'},
      {"days", required_argument, nullptr, 'd'},
      {"world", required_argument, nullptr, 'w'},
      {nullptr, 0, nullptr, 0}};

  bool option_error = false;
  opterr = 0;
  optind = 1;
  int option = 0;
  while ((option = getopt_long(argc, argv, "", long_options, nullptr)) != -1) {
    if (option == 'i') {
      infection_seeds_file = optarg;
      infection_seeds_cli_override = true;
    } else if (option == 'c') {
      sim_config_file = optarg;
    } else if (option == 's') {
      try {
        long long v = std::stoll(optarg);
        if (v < 0 || v > 0xFFFFFFFFLL) {
          throw std::out_of_range("seed must be in [0, 4294967295]");
        }
        seed_override = v;
      } catch (...) {
        if (rank == 0)
          std::cerr << "Warning: Invalid value for --seed: " << optarg
                    << std::endl;
      }
    } else if (option == 'r') {
      runs_dir = optarg;
    } else if (option == 'u') {
      run_id_override = optarg;
    } else if (option == 'R') {
      restart_from = optarg;
    } else if (option == 'd') {
      try {
        days_override = std::stoi(optarg);
      } catch (...) {
        if (rank == 0)
          std::cerr << "Warning: Invalid value for --days: " << optarg
                    << std::endl;
      }
    } else if (option == 'w') {
      filename = optarg;
    } else {
      option_error = true;
      if (rank == 0) {
        std::cerr << "Error: unknown or incomplete command-line option"
                  << std::endl;
      }
    }
  }

  if (option_error || sim_config_file.empty() || filename.empty()) {
    if (rank == 0) {
      if (sim_config_file.empty())
        std::cerr << "Error: --config <path/to/simulation.yaml> is required."
                  << std::endl;
      if (filename.empty())
        std::cerr << "Error: --world <path/to/world.h5> is required."
                  << std::endl;
    }
#ifdef USE_MPI
    MPI_Finalize();
#endif
    return 1;
  }

  // Hoisted out of the try so the top-level H5::Exception handler can tell
  // whether a compartmental plugin (the only thing that dlopen's a second
  // libhdf5_cpp) was in play — see the catch below.
  std::string compartmental_sidecar_path;

  try {
    // Load configuration (all ranks load config)
    Config config = ConfigLoader::loadAll(sim_config_file);
    compartmental_sidecar_path = config.simulation.compartmental_model_sidecar;

    // Resolve infection_seeds_file: CLI arg takes priority, then
    // simulation.yaml, then hardcoded default
    if (!infection_seeds_cli_override) {
      infection_seeds_file = config.simulation.infection_seeds_file;
    }

    // Resolve run id (UTC timestamp by default; --run-id overrides). Rank 0
    // generates and broadcasts so every rank lands on the same path.
    std::string run_id = run_id_override;
    if (rank == 0 && run_id.empty()) {
      run_id = run_dir::generateRunIdUtc();
    }
    run_dir::broadcastRunId(run_id);
    std::filesystem::path run_path = std::filesystem::path(runs_dir) / run_id;
    std::string output_path = (run_path / "simulation_events.h5").string();

    // Checkpoint resume: the checkpoint's recorded effective seed is
    // authoritative. Use it unless the caller supplied --seed; an explicit
    // mismatch is rejected during checkpoint restore. Path is normalised here
    // ('latest' symlink resolved).
    if (!restart_from.empty()) {
      std::filesystem::path cpdir = std::filesystem::canonical(restart_from);
      YAML::Node cman = YAML::LoadFile((cpdir / "manifest.yaml").string());
      unsigned int cseed = cman["effective_random_seed"].as<unsigned int>();
      if (seed_override < 0) {
        seed_override = static_cast<long long>(cseed);
        if (rank == 0)
          std::cout << "Resuming from checkpoint " << cpdir << " (seed "
                    << cseed << ")" << std::endl;
      }
      restart_from = cpdir.string();
    }

    // Resolve the effective RNG seed exactly once, before snapshotting, so it
    // is recorded for reproducible restart. Precedence: CLI --seed overrides
    // config; a zero/absent seed is auto-generated on rank 0 and broadcast so
    // every rank and subsequent checkpoint resume uses the identical stream.
    if (seed_override >= 0) {
      config.simulation.random_seed = static_cast<unsigned int>(seed_override);
      if (rank == 0)
        std::cout << "Overriding random seed: " << seed_override << std::endl;
    }
    unsigned int effective_seed = config.simulation.random_seed;
    const bool seed_autogenerated = (effective_seed == 0);
    if (seed_autogenerated && rank == 0) {
      effective_seed = std::random_device{}();
      if (effective_seed == 0) effective_seed = 1;  // 0 is the "unset" sentinel
    }
    run_dir::broadcastSeed(effective_seed);
    config.simulation.random_seed = effective_seed;

    if (rank == 0) {
      std::cout << "Loading configuration..." << std::endl;
      std::cout << "  Config:  " << sim_config_file << std::endl;
      std::cout << "  Seeds:   " << infection_seeds_file << std::endl;
      std::cout << "  Run dir: " << run_path.string() << std::endl;
    }

    // (random seed already resolved + recorded above, before snapshotRun)
    // Apply days override if provided
    if (days_override > 0) {
      std::tm start = parseDate(config.simulation.start_date);
      std::tm end = addDays(start, days_override);
      config.simulation.end_date = formatDate(end);
      if (rank == 0) {
        std::cout << "Overriding simulation duration: " << days_override
                  << " days (End date: " << config.simulation.end_date << ")"
                  << std::endl;
      }
    }

    if (rank == 0) {
      std::cout << "\nSimulation Parameters:" << std::endl;
      std::cout << "  Start: " << config.simulation.start_date
                << "  End: " << config.simulation.end_date
                << "  Seed: " << config.simulation.random_seed << std::endl;
    }

    if (seed_autogenerated && rank == 0) {
      std::cout << "No random seed configured; generated effective seed "
                << effective_seed
                << " (recorded in manifest.yaml lineage for reproducible "
                   "restart)"
                << std::endl;
    }

#ifdef USE_MPI
    if (config.parallel.enabled && size > 1) {
      // PARALLEL MODE: Use domain decomposition with distributed memory
      if (rank == 0) {
        std::cout << "\nRunning in PARALLEL mode with " << size << " MPI ranks"
                  << std::endl;
      }

      // All ranks need empty world initially - will be populated during
      // initialize()
      WorldState world;

      // Rank 0 loads geography only (lightweight - just geo units, no
      // people/venues) DomainManager::initialize() will load person metadata
      // separately
      if (rank == 0) {
        world = HDF5Loader::loadGeographyOnly(filename);
      }

      MPI_Barrier(MPI_COMM_WORLD);

      // Create domain manager and partition world
      DomainManager domain_mgr(world, config);
      domain_mgr.setWorldStateFile(filename);
      domain_mgr.initialize();  // This will load domain-specific data
      domain_mgr.synchronizeRegistries();  // MPI determinism: align property
                                           // value indices
      config.resolve(world);

      // Get the local domain's world state
      Domain& domain = domain_mgr.getDomain();

      // Regional Risk: Load factors
      if (config.simulation.regional_risk.enabled) {
        if (rank == 0)
          std::cout << "Loading regional risk factors for parallel domains..."
                    << std::endl;
        domain.world->loadRegionalRiskFactors(
            config.simulation.regional_risk.regional_risk_file);
      }

      MPI_Barrier(MPI_COMM_WORLD);

      Simulator simulator(*domain.world, config, &domain_mgr,
                          infection_seeds_file, output_path);

      // The loaders have now recorded any nested data files they actually
      // consumed; snapshot that authoritative list without reparsing YAML.
      if (rank == 0) {
        run_dir::snapshotRun(run_path, sim_config_file, config, size, cli_args,
                             effective_seed, filename);
      }
      MPI_Barrier(MPI_COMM_WORLD);

      if (!restart_from.empty()) simulator.restoreFromCheckpoint(restart_from);

      // Start CPU profiling
#ifdef USE_GPERFTOOLS
      const std::string prof_name =
          size > 1 ? "cpu_profile_rank0.prof" : "cpu_profile.prof";
      startCpuProfiler(rank, run_path / prof_name, true);
#endif

      // Run simulation on this domain with cross-domain visitor exchange
      simulator.run();

      // Stop profiling and save results
      if (rank == 0) {
#ifdef USE_GPERFTOOLS
        stopCpuProfiler(rank, run_path / prof_name, true);
#endif
      }

      MPI_Barrier(MPI_COMM_WORLD);

      // Merge event files from all ranks into a single file
      if (rank == 0) {
        std::cout << "\n[Parallel simulation complete!]" << std::endl;
        std::cout << "[Cross-domain visitor exchange enabled]" << std::endl;

        // Collect all rank event files
        std::vector<std::string> rank_files;
        const std::string& final_output = output_path;
        std::filesystem::path p(final_output);
        std::string stem = p.stem().string();
        std::string ext = p.extension().string();
        std::string parent = p.parent_path().string();
        if (!parent.empty()) parent += "/";

        for (int r = 0; r < size; ++r) {
          rank_files.push_back(parent + stem + "_rank" + std::to_string(r) +
                               ext);
        }

        // Merge into a single file
        mergeEventFiles(rank_files, final_output);
      }
    } else
#endif
    {
      // SERIAL MODE: Load full world
      if (rank == 0) {
        std::cout << "\n" << std::string(50, '=') << std::endl;
        std::cout << "Loading world from: " << filename << std::endl;
        std::cout << std::string(50, '=') << std::endl;
      }

      WorldState world = HDF5Loader::load(filename, config);
      config.resolve(world);

      // Regional Risk: Load factors
      if (config.simulation.regional_risk.enabled) {
        std::cout << "Loading regional risk factors..." << std::endl;
        world.loadRegionalRiskFactors(
            config.simulation.regional_risk.regional_risk_file);
      }

      if (rank == 0) {
        std::cout << std::string(50, '=') << std::endl;
        world.printSummary();
        std::cout << "\n" << std::string(50, '=') << std::endl;
        std::cout << "Running in SERIAL mode" << std::endl;
        std::cout << std::string(50, '=') << std::endl;

        std::cout << "Loading infection seeds from: " << infection_seeds_file
                  << std::endl;
        Simulator simulator(world, config, nullptr, infection_seeds_file,
                            output_path);

        // The loaders have now recorded any nested data files they actually
        // consumed; snapshot that authoritative list without reparsing YAML.
        run_dir::snapshotRun(run_path, sim_config_file, config, size, cli_args,
                             effective_seed, filename);

        if (!restart_from.empty())
          simulator.restoreFromCheckpoint(restart_from);

        // Start CPU profiling
#ifdef USE_GPERFTOOLS
        startCpuProfiler(rank, run_path / "cpu_profile.prof", false);
#endif

        simulator.run();
        memory::logMemory("Post-Simulation-Serial");

        // Stop profiling
#ifdef USE_GPERFTOOLS
        stopCpuProfiler(rank, run_path / "cpu_profile.prof", false);
#endif
      }
    }

  } catch (H5::Exception& e) {
    // Special-case the duplicate-libhdf5_cpp ABI clash: when a compartmental
    // plugin is dlopen'd against a different libhdf5_cpp than this binary, HDF5
    // initialises its global DataSpace::ALL constant twice and throws from
    // inside ld.so's call_init. That throw cannot be caught at the dlopen site
    // (a handler there is bypassed or escalates to std::terminate), but it does
    // unwind cleanly to here — so this is where we convert it into actionable
    // guidance. Gated on a configured sidecar so it fires iff a plugin (the
    // only second libhdf5_cpp) was actually loaded.
    if (!compartmental_sidecar_path.empty() &&
        june::isHdf5DuplicateConstantError(e.getFuncName(), e.getDetailMsg())) {
      if (rank == 0) {
        std::string plugin_path;
        try {
          plugin_path = june::CompartmentalModelManager::realLoadSidecar(
                            compartmental_sidecar_path)
                            .plugin_so_path;
        } catch (...) {
          // best-effort: fall back to the sidecar path in the message
          plugin_path = compartmental_sidecar_path;
        }
        std::cerr << june::formatHdf5AbiMismatchMessage(plugin_path,
                                                        e.getDetailMsg());
      }
    } else {
      std::cerr << "[Rank " << rank
                << "] FATAL: HDF5 error: " << e.getCDetailMsg() << std::endl;
    }
#ifdef USE_MPI
    MPI_Abort(MPI_COMM_WORLD, 1);
#endif
    return 1;
  } catch (std::exception& e) {
    // FATAL, not Error: this handler is unconditionally followed by MPI_Abort,
    // so everything it prints ends the run for every rank.
    std::cerr << "[Rank " << rank << "] FATAL: " << e.what() << std::endl;
#ifdef USE_MPI
    MPI_Abort(MPI_COMM_WORLD, 1);
#endif
    return 1;
  }

#ifdef USE_MPI
  MPI_Finalize();
#endif

  return 0;
}
