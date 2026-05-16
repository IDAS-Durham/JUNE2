#include "simulation/simulator.h"
#ifdef USE_MPI
#include "parallel/domain_manager.h"
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include "utils/event_logging/event_writer.h"

namespace june {

// =============================================================================
// Implementation
// =============================================================================

Simulator::Simulator(WorldState& world, const Config& config,
                     DomainManager* domain_mgr,
                     const std::string& infection_seeds_file,
                     const std::string& output_filename)
    : world_(world),
      config_(config),
      domain_mgr_(domain_mgr),
      activity_manager_(world, config),
      current_day_num_(0),
      current_simulation_time_(0.0) {
  // GlobalRNG is seeded in main.cpp before any components are created
  // This ensures Domain, ActivityManager, etc. can use RNG during construction

  // Checkpointing is incompatible with the compartmental-model plugin: the
  // external ODE plugin's internal state is opaque to the engine, so a
  // resume would silently diverge. Fail fast on all ranks (no silent
  // fallback).
  if (config_.simulation.checkpoint.enabled &&
      !config_.simulation.compartmental_model_sidecar.empty()) {
    throw std::runtime_error(
        "Checkpointing is not supported for compartmental-model scenarios "
        "(compartmental_model_sidecar is set). Disable 'checkpoint.enabled' "
        "or remove the sidecar.");
  }

  // Parse start date
  if (!config_.simulation.start_date.empty()) {
    try {
      current_date_ = parseDate(config_.simulation.start_date);
    } catch (...) {
      current_date_ = {0, 0, 0, 1, 0, 120};  // 2020-01-01
    }
  } else {
    current_date_ = {0, 0, 0, 1, 0, 120};  // 2020-01-01
  }

  // Calculate total simulation days
  if (!config_.simulation.end_date.empty()) {
    try {
      std::tm end_date = parseDate(config_.simulation.end_date);
      total_days_ = daysBetween(current_date_, end_date);
    } catch (...) {
      total_days_ = 365;
    }
  } else {
    total_days_ = 365;
  }

  // Load disease configuration
  try {
    disease_ = std::make_unique<Disease>(DiseaseLoader::loadFromYAML(
        config_.simulation.disease_file, config_.simulation.verbose));
  } catch (const std::exception& e) {
    std::cerr << "FATAL: Failed to load disease: " << e.what() << std::endl;
    throw;
  }

  world_.symptom_names = disease_->getSymptomNames();

  // Initialize fomite state on venues before epidemiology
  initFomiteState();

  // Now that disease_ is loaded, initialize Epidemiology
  epidemiology_ =
      std::make_unique<Epidemiology>(world_, disease_.get(), &event_logger_);

  // Initialize infection seeder
  InfectionSeedConfig seed_config;
  try {
    seed_config = InfectionSeedConfigLoader::loadFromFile(infection_seeds_file);
  } catch (const std::exception& e) {
    std::cerr << "Warning: Could not load infection seeds: " << e.what()
              << std::endl;
    std::cerr << "Using empty infection seed configuration." << std::endl;
  }
  infection_seeder_ = std::make_unique<InfectionSeeder>(
      world_, disease_.get(), seed_config, &event_logger_,
      config_.simulation.random_seed);

  // ---------------------------------------------------------------------------
  // One-shot disease/seed audit dump (rank 0 only).
  // Prints a human-readable summary of every loaded disease parameter so we
  // can eyeball-confirm the right configuration is in effect. Output is
  // identical regardless of MPI rank count because only rank 0 ever writes.
  // ---------------------------------------------------------------------------
  {
    int audit_rank = 0;
#ifdef USE_MPI
    if (domain_mgr_) audit_rank = domain_mgr_->getRank();
#endif
    if (audit_rank == 0) {
      // Save and override stream state so inherited precision doesn't truncate.
      auto saved_flags = std::cout.flags();
      auto saved_prec = std::cout.precision();
      std::cout.unsetf(std::ios::floatfield);
      std::cout << std::setprecision(6);
      auto fmtDist = [](const DistributionParams& d) {
        std::ostringstream os;
        os << d.type << "(";
        bool first = true;
        for (const auto& [k, v] : d.params) {
          if (!first) os << ", ";
          os << k << "=" << v;
          first = false;
        }
        os << ")";
        return os.str();
      };

      const auto& tp = disease_->getTransmissionParams();
      std::cout << "\n=== [AUDIT] Disease config (rank 0) ===" << std::endl;
      std::cout << "  Disease name: " << disease_->getName() << std::endl;
      std::cout << "  Disease YAML: " << config_.simulation.disease_file
                << std::endl;
      const auto& tags = disease_->getSymptomTags();
      std::cout << "  symptom_tags (" << tags.size() << "):";
      for (const auto& t : tags) std::cout << " " << t.name;
      std::cout << std::endl;

      const auto& trajs = disease_->getTrajectories();
      std::cout << "  trajectories (" << trajs.size() << "):" << std::endl;
      for (const auto& t : trajs) {
        std::cout << "    - " << std::left << std::setw(14) << t.selection_key
                  << " sev=" << t.severity
                  << " inf_factor=" << t.infectiousness_factor
                  << " stages=" << t.stages.size() << "  path:";
        for (size_t si = 0; si < t.stages.size(); ++si) {
          std::cout << (si ? "→" : " ") << t.stages[si].symptom_tag;
        }
        if (!t.stages.empty()) {
          std::cout << "  exposed_dist="
                    << fmtDist(t.stages[0].completion_time);
        }
        std::cout << std::endl;
      }

      std::cout << "  transmission:" << std::endl;
      std::cout << "    mode="
                << (tp.mode == InfectiousnessMode::TRAJECTORY_DRIVEN
                        ? "Trajectory-Driven"
                        : "Stage-Driven")
                << "   type=" << tp.type << std::endl;
      if (tp.mode == InfectiousnessMode::TRAJECTORY_DRIVEN) {
        std::cout << "    max_infectiousness=" << fmtDist(tp.max_infectiousness)
                  << std::endl;
        std::cout << "    shape=" << fmtDist(tp.shape)
                  << "  rate=" << fmtDist(tp.rate)
                  << "  shift=" << fmtDist(tp.shift) << std::endl;
        // Predicted gamma peak for diagnostic only:
        // mode = (shape-1)/rate + shift, when shape > 1
        auto getLoc = [](const DistributionParams& d) -> double {
          auto it = d.params.find("loc");
          return it == d.params.end() ? 0.0 : it->second;
        };
        double sh = getLoc(tp.shape);
        double rt = getLoc(tp.rate);
        double sft = getLoc(tp.shift);
        if (sh > 1.0 && rt > 0.0) {
          std::cout << "    -> predicted peak day post-infection: "
                    << ((sh - 1.0) / rt + sft) << std::endl;
        }
      }
      std::cout << "    modes (" << tp.modes.size() << "):";
      for (const auto& tmode : tp.modes) {
        std::cout << "  " << tmode.name << "="
                  << tmode.susceptibility_multiplier;
      }
      std::cout << std::endl;

      std::cout << "  immunity: level=" << tp.natural_immunity.level
                << "  waning_rate=" << tp.natural_immunity.waning_rate
                << std::endl;

      const auto& orates = disease_->getOutcomeRates();
      std::cout << "  outcome_rates: " << orates.rows.size() << " rows loaded"
                << std::endl;
      auto dumpRow = [&](size_t i) {
        if (i >= orates.rows.size()) return;
        const auto& row = orates.rows[i];
        double total = 0.0;
        std::cout << "    row[" << i << "]:";
        for (const auto& [k, v] : row.probabilities) {
          std::cout << " " << k << "=" << v;
          total += v;
        }
        std::cout << "  sum=" << total << std::endl;
      };
      dumpRow(0);
      dumpRow(orates.rows.size() / 2);
      if (!orates.rows.empty()) dumpRow(orates.rows.size() - 1);

      std::cout << "\n=== [AUDIT] Infection-seed config (rank 0) ==="
                << std::endl;
      std::cout << "  base_cases_per_capita="
                << seed_config.global_params.base_cases_per_capita
                << "  default_strength="
                << seed_config.global_params.default_seed_strength << std::endl;
      std::cout << "  seeds (" << seed_config.seeds.size() << "):" << std::endl;
      // Helper: pretty-print one SelectionCriterion. Reused for both
      // global attribute_filters and per-target-group criteria, because
      // bulk-CSV exact/clustered seeds store their filters in
      // structured_config.target_groups[*].criteria — not attribute_filters.
      auto printCriterion = [](const SelectionCriterion& f,
                               const std::string& prefix) {
        std::cout << prefix << f.property_path << " " << f.operator_type << " ";
        if (std::holds_alternative<std::string>(f.value)) {
          std::cout << '"' << std::get<std::string>(f.value) << '"';
        } else if (std::holds_alternative<int>(f.value)) {
          std::cout << std::get<int>(f.value);
        } else if (std::holds_alternative<double>(f.value)) {
          std::cout << std::get<double>(f.value);
        } else if (std::holds_alternative<bool>(f.value)) {
          std::cout << (std::get<bool>(f.value) ? "true" : "false");
        } else {
          std::cout << "(complex)";
        }
        std::cout << std::endl;
      };

      for (const auto& s : seed_config.seeds) {
        size_t tg_crit_total = 0;
        for (const auto& g : s.structured_config.target_groups) {
          tg_crit_total += g.criteria.size();
        }
        std::cout << "    - " << s.name << "  type="
                  << (s.type == InfectionSeedType::UNIFORM ? "uniform"
                                                           : "structured")
                  << "  date=" << s.date_time
                  << "  filters=" << s.attribute_filters.size()
                  << "  target_group_criteria=" << tg_crit_total << std::endl;
        if (s.type == InfectionSeedType::UNIFORM) {
          std::cout << "      cases_per_capita="
                    << s.uniform_config.cases_per_capita
                    << "  seed_strength=" << s.seed_strength << std::endl;
        } else {
          std::cout << "      geo_level=" << s.structured_config.geo_level
                    << "  units=" << s.structured_config.unit_cases.size()
                    << "  target_groups="
                    << s.structured_config.target_groups.size() << std::endl;
          for (const auto& uc : s.structured_config.unit_cases) {
            int total = 0;
            for (int n : uc.cases_per_target_group) total += n;
            std::cout << "        unit=" << uc.unit_id << "  cases=" << total
                      << std::endl;
          }
        }
        for (const auto& f : s.attribute_filters) {
          printCriterion(f, "      filter: ");
        }
        for (size_t gi = 0; gi < s.structured_config.target_groups.size();
             ++gi) {
          for (const auto& c : s.structured_config.target_groups[gi].criteria) {
            std::cout << "      target_group[" << gi << "]: ";
            printCriterion(c, "");
          }
        }
      }
      std::cout << "==========================================\n" << std::endl;
      // Restore stream state.
      std::cout.flags(saved_flags);
      std::cout.precision(saved_prec);
    }
  }

  // Initialize interaction and transmission management
  interaction_manager_ = std::make_unique<InteractionManager>(
      world_, config_.contact_matrices, config_.simulation, config_.parallel,
      disease_.get(), &event_logger_);

  // Initialize coordinated encounter manager
  int rank = 0;
#ifdef USE_MPI
  if (domain_mgr_) rank = domain_mgr_->getRank();
#endif
  coordinated_encounter_manager_ =
      std::make_unique<CoordinatedEncounterManager>(world_, config_, rank);

  // Resolve infection seed selection criteria against the loaded world.
  infection_seeder_->resolveConfig(world_);

  // Initialize vaccination manager
  vaccination_manager_ =
      std::make_unique<VaccinationManager>(world_, config_, &event_logger_);

  // Initialize policy manager
  policy_manager_ = std::make_unique<PolicyManager>(world_);
  policy_manager_->setBaseSeed(config_.simulation.random_seed);
  try {
    PolicyLoader::loadPolicies(*policy_manager_,
                               config_.simulation.policies_file,
                               config_.simulation.start_date);
  } catch (const std::exception& e) {
    std::cerr << "Warning: Could not load policies: " << e.what() << std::endl;
    std::cerr << "Continuing without policies." << std::endl;
  }

  // Set disease in domain manager (if in parallel mode)
#ifdef USE_MPI
  if (domain_mgr_) {
    domain_mgr_->setDisease(disease_.get());
  }
#endif

  // Initialize locations (everyone starts at residence)
  activity_manager_.initializeLocations(locations_);

  // Initialize incremental lookup tracking
  // No pre-allocation needed for sets

  // Assign schedule types to people based on selection criteria
  activity_manager_.assignScheduleTypes();

#ifdef USE_MPI
  if (domain_mgr_) {
    domain_mgr_->exchangeScheduleTypes();
  }
#endif

  // Set policy manager in activity manager
  activity_manager_.setPolicyManager(policy_manager_.get());

  // Precompute which policies apply to each person (based on selection
  // criteria) This must be done AFTER schedules are assigned (person properties
  // are set)
  if (policy_manager_->getSymptomPolicyCount() > 0 ||
      policy_manager_->getTemporalPolicyCount() > 0) {
    policy_manager_->resolveAll(*disease_);
    policy_manager_->precomputePolicyApplicability(world_.people);
  }

  // Set policy manager in coordinated encounter manager

  // Pre-compute schedules
  activity_manager_.precomputeSchedules();

  // Initialize events filename based on rank
#ifdef USE_MPI
  if (domain_mgr_) {
    std::filesystem::path p(output_filename);
    std::string stem = p.stem().string();
    std::string ext = p.extension().string();
    std::string parent = p.parent_path().string();
    if (!parent.empty()) parent += "/";
    events_filename_ =
        parent + stem + "_rank" + std::to_string(domain_mgr_->getRank()) + ext;
  } else {
    events_filename_ = output_filename;
  }
#else
  events_filename_ = output_filename;
#endif

  // Ensure we start with a fresh file for this new simulation run
  if (std::filesystem::exists(events_filename_)) {
    std::filesystem::remove(events_filename_);
  }

  compartmental_model_manager_ = std::make_unique<CompartmentalModelManager>(
      config_.simulation.compartmental_model_sidecar, domain_mgr_);

  MemoryUtils::logGlobalMemoryStats("Simulator Initialized");
}

void Simulator::run() {
  int rank = 0;
#ifdef USE_MPI
  if (domain_mgr_) rank = domain_mgr_->getRank();
#endif

  if (rank == 0) {
    std::cout << "\n=== Starting Simulation ===" << std::endl;
    std::cout << std::string(50, '=') << std::endl;

    // Enable wall-clock profiling for high-level phases
    Profiler::instance().enable();

    // Checkpoint cadence: validate + announce the active mode once.
    const auto& cp = config_.simulation.checkpoint;
    if (cp.enabled) {
      if (cp.usesDates()) {
        if (cp.every_n_days.has_value()) {
          std::cout << "[checkpoint] on_dates is set; every_n_days ("
                    << *cp.every_n_days
                    << ") is IGNORED (dates take precedence)" << std::endl;
        }
        const std::string& sd = config_.simulation.start_date;
        const std::string& ed = config_.simulation.end_date;
        for (const auto& d : *cp.on_dates) {
          if (d < sd || d > ed) {
            std::cout << "[checkpoint] WARNING: on_dates entry '" << d
                      << "' is outside the simulation window [" << sd << ", "
                      << ed << "] and will never fire" << std::endl;
          }
        }
        std::cout << "[checkpoint] enabled: date-based cadence, "
                  << cp.on_dates->size() << " date(s), output_dir='"
                  << cp.output_dir << "', keep_last=" << cp.keep_last
                  << std::endl;
      } else if (cp.every_n_days.has_value() && *cp.every_n_days > 0) {
        std::cout << "[checkpoint] enabled: every " << *cp.every_n_days
                  << " day(s), output_dir='" << cp.output_dir
                  << "', keep_last=" << cp.keep_last << std::endl;
      } else {
        std::cout << "[checkpoint] enabled but no cadence configured "
                     "(every_n_days and on_dates both absent) — no "
                     "checkpoints will be written"
                  << std::endl;
      }
    }
  }

  if (resume_from_day_ > 0 && rank == 0) {
    std::cout << "[checkpoint] resuming simulation at day " << resume_from_day_
              << " of " << total_days_ << std::endl;
  }
  for (int day = resume_from_day_; day < total_days_; ++day) {
    current_day_num_ = day;
    current_date_ = addDays(parseDate(config_.simulation.start_date), day);

    if (rank == 0) {
      std::cout << "\nDay " << day << " (" << formatDate(current_date_) << ")"
                << std::endl;
    }
    MemoryUtils::logGlobalMemoryStats("Start of Day " + std::to_string(day));

    // 0. Update simulation time for start of day
    current_simulation_time_ = static_cast<double>(day);

    // 1. Run vaccination manager updates at START of day
    // This ensures efficacy applies to today's interactions
    if (vaccination_manager_) {
      vaccination_manager_->update(current_simulation_time_);
    }

    // 2. Sync death flags across ranks so relationship dissolution can
    //    detect partners who died on a remote rank during the previous day.
#ifdef USE_MPI
    if (domain_mgr_) {
      domain_mgr_->exchangeDeathFlags();
    }
#endif

    // 3. Negotiate Coordinated Encounters
    if (coordinated_encounter_manager_) {
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
          all_proposals, local_proposals, local_replies,
          encounter_day_type_idx);

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
      coordinated_encounter_manager_->finalizeEncounters(all_replies,
                                                         finalized);

      // Accumulate finalize stats for debug summary
      coordinated_encounter_manager_->accumulateFinalizeStats(finalized);

      // Log locally-finalized encounters to HDF5 BEFORE merging remote ones.
      // This prevents cross-rank encounters from being logged on both ranks.
      // group_id fans one unique uint64 per real encounter across every
      // pair-row belonging to it. Rank is packed into the high 16 bits so
      // counters stay unique across MPI ranks without coordination; the low
      // 48 bits are a per-rank monotonic counter (2^48 events / rank is
      // effectively unbounded for realistic simulations).
      {
        int rank = 0;
#ifdef USE_MPI
        if (domain_mgr_) rank = domain_mgr_->getRank();
#endif
        const uint64_t rank_prefix = static_cast<uint64_t>(rank) << 48;
        for (const auto& enc : finalized) {
          const uint64_t group_id = rank_prefix | (next_encounter_group_id_++ &
                                                   0x0000FFFFFFFFFFFFULL);
          for (PersonId pid : enc.participants) {
            if (pid != enc.host_id) {
              event_logger_.logCoordinatedEncounter(
                  enc.host_id, pid, current_simulation_time_,
                  enc.encounter_type_id, enc.slot, group_id);
            }
          }
        }
      }

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

    // Print daily encounter debug summary
    if (coordinated_encounter_manager_) {
      coordinated_encounter_manager_->printDailyEncounterSummary(day);
    }

    simulateDay(day);

    // --- DEBUG: per-day state hash (MPI-invariant XOR) ---
    // Set JUNE_DEBUG_DAY_HASH=1 to enable. Each rank hashes its local
    // people; XOR across ranks aggregates into a hash that is independent
    // of partition layout — so 1-rank and N-rank runs should produce the
    // exact same per-day hash stream if the simulation is deterministic.
    {
      static const bool debug_hash_enabled = []() {
        const char* e = std::getenv("JUNE_DEBUG_DAY_HASH");
        return e && std::string(e) != "0";
      }();
      if (debug_hash_enabled) {
        auto mix64 = [](uint64_t x) {
          x ^= x >> 30;
          x *= 0xbf58476d1ce4e5b9ULL;
          x ^= x >> 27;
          x *= 0x94d049bb133111ebULL;
          x ^= x >> 31;
          return x;
        };
        auto dbits = [](double d) {
          uint64_t u = 0;
          std::memcpy(&u, &d, sizeof(u));
          return u;
        };

        uint64_t h_dead = 0;
        uint64_t h_inf = 0;
        uint64_t h_imm = 0;
        int n_alive = 0;
        int n_infected = 0;
        int n_immune = 0;
        int n_dead = 0;

        for (size_t i = 0; i < world_.people.size(); ++i) {
          const auto& p = world_.people[i];
          uint64_t base =
              mix64(static_cast<uint64_t>(p.id) + 0x9e3779b97f4a7c15ULL);

          if (p.is_dead) {
            n_dead++;
            h_dead ^= mix64(base ^ 0xDEAD5A10ULL ^ dbits(p.death_time));
          } else {
            n_alive++;
          }

          if (p.infection) {
            n_infected++;
            h_inf ^= mix64(base ^ 0x11FEC7EDULL ^
                           dbits(p.infection->getInfectionTime()));
          }

          if (p.immunity.natural_acquisition_time >= 0.0) {
            n_immune++;
            h_imm ^= mix64(base ^ 0x1EEEEDEDULL ^
                           dbits(p.immunity.natural_acquisition_time));
          }
        }

        uint64_t local_h[3] = {h_dead, h_inf, h_imm};
        uint64_t global_h[3] = {h_dead, h_inf, h_imm};
        int local_n[4] = {n_alive, n_dead, n_infected, n_immune};
        int global_n[4] = {n_alive, n_dead, n_infected, n_immune};
#ifdef USE_MPI
        if (domain_mgr_) {
          MPI_Allreduce(local_h, global_h, 3, MPI_UINT64_T, MPI_BXOR,
                        MPI_COMM_WORLD);
          MPI_Allreduce(local_n, global_n, 4, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
        }
#endif
        if (rank == 0) {
          std::ios_base::fmtflags f = std::cout.flags();
          std::cout << "[DayHash] day=" << day << " alive=" << global_n[0]
                    << " dead=" << global_n[1] << " inf=" << global_n[2]
                    << " imm=" << global_n[3] << std::hex << std::setfill('0')
                    << " H_dead=" << std::setw(16) << global_h[0]
                    << " H_inf=" << std::setw(16) << global_h[1]
                    << " H_imm=" << std::setw(16) << global_h[2] << std::endl;
          std::cout.flags(f);
        }
      }
    }

    // Output statistics periodically
    if ((day + 1) % config_.simulation.stats_interval_days == 0) {
      outputStatistics();
    }

    // End-of-day flush check (triggers flush_interval_days)
    checkAndFlushEvents(true);

    // Checkpoint trigger (P2: detection only — P3 will write the delta).
    // Placed after the end-of-day flush so disease progression is done,
    // events are flushed, and cross-rank buffers are empty.
    if (config_.simulation.checkpoint.triggersOnDay(
            day, formatDate(current_date_))) {
      if (rank == 0) {
        const auto& cp = config_.simulation.checkpoint;
        std::cout << "[checkpoint] TRIGGER at end of day " << day << " ("
                  << formatDate(current_date_)
                  << "), sim_time=" << current_simulation_time_
                  << ", mode=" << (cp.usesDates() ? "on_dates" : "every_n_days")
                  << std::endl;
      }
      ScopedTimer timer("06_Checkpoint");
      writeCheckpoint(day, formatDate(current_date_));
    }
  }

  if (rank == 0) {
    std::cout << "\n=== Simulation Complete ===" << std::endl;
    std::cout << "Saving final epidemic events with lookup tables to "
              << events_filename_ << "..." << std::endl;
  }

  // Final save: write lookups for anyone still not written (e.g. if flushes
  // were missed or people infected in last slots)
  std::unordered_set<PersonId> remaining_ids;
  if (config_.simulation.save_full_person_details == "infected_only") {
    for (PersonId pid : event_logger_.getInfectedPersonIds()) {
      if (lookups_written_.find(pid) == lookups_written_.end()) {
        remaining_ids.insert(pid);
        lookups_written_.insert(pid);
      }
    }
  }

  // saveToHDF5WithLookups handles appending if file exists (from previous
  // flushes)
  {
    ScopedTimer timer("05_FinalHDF5Save");
    event_logger_.saveToHDF5WithLookups(
        events_filename_, world_, config_,
        remaining_ids.empty() &&
                config_.simulation.save_full_person_details == "infected_only"
            ? nullptr
            : &remaining_ids);
  }

  if (rank == 0) {
    // Print wall-clock summary AFTER everything is done
    Profiler::instance().printDetailedResults();

    // Print optimization stats
    activity_manager_.getStats().print();
    if (interaction_manager_) {
      interaction_manager_->getStats().print();
    }

    // Print encounter stats
    event_logger_.printEncounterStats(config_.schedule.day_type_names,
                                      day_type_counts_);
  }
}

void Simulator::simulateDay(int day_num) {
  int day_type_idx = config_.schedule.getDayTypeIndex(day_num);

  // Track per-day-type occurrence counts
  if (day_type_idx >= static_cast<int>(day_type_counts_.size()))
    day_type_counts_.resize(day_type_idx + 1, 0);
  day_type_counts_[day_type_idx]++;

  // Set simulation time to start of this day (redundant if already set in
  // run(), but safe)
  current_simulation_time_ = static_cast<double>(day_num);

  // Get slot list for this day type from the first schedule type (all schedule
  // types share aligned boundaries)
  if (config_.schedule.schedule_types.empty()) {
    std::cerr << "ERROR: No schedule types defined!" << std::endl;
    return;
  }
  const auto* slots_ptr =
      config_.schedule.schedule_types[0].slots_by_day_type_idx.empty()
          ? nullptr
          : config_.schedule.schedule_types[0]
                .slots_by_day_type_idx[day_type_idx];
  if (!slots_ptr) {
    std::cerr << "ERROR: No slots defined for day type index " << day_type_idx
              << std::endl;
    return;
  }
  const auto& schedule = *slots_ptr;

  // Simulate each time slot
  for (size_t slot_idx = 0; slot_idx < schedule.size(); ++slot_idx) {
    const auto& slot = schedule[slot_idx];
    double delta_hours = calculateDuration(slot.start, slot.end);
    simulateTimeSlot(slot, slot_idx, day_type_idx, delta_hours);
    current_simulation_time_ += delta_hours / 24.0;

    // Granular memory check after each time slot
    std::string mem_label = "Day " + std::to_string(current_day_num_) +
                            " Slot " + std::to_string(slot_idx);
    MemoryUtils::logGlobalMemoryStats(mem_label);

    // Granular flush check (triggers max_event_buffer_size mid-day)
    checkAndFlushEvents(false);
  }
}

void Simulator::simulateTimeSlot(const TimeSlot& slot, int time_slot_index,
                                 int day_type_idx, double delta_hours) {
  int rank = 0;
#ifdef USE_MPI
  if (domain_mgr_) rank = domain_mgr_->getRank();
#endif

  printSimulationState(slot.name, delta_hours);

  // Compartmental coupling sequence — ORDER IS LOAD-BEARING:
  // 1. advance()                    — plugin integrates ODE with previous
  // slot's inputs
  // 2. processTransmissions()       — humans exposed to FOI from plugin (reads
  // buffer lazily)
  // 3. computeDepositionWriteback() — aggregate infections → plugin inputs
  // 4. maybeSnapshot()              — record plugin state after full slot
  compartmental_model_manager_->advance(
      static_cast<float>(delta_hours / 24.0),
      static_cast<float>(current_simulation_time_));

  // Step 0: Apply infection seeds for this time
  std::string current_dt = formatDate(current_date_) + " " + slot.start;
  applyInfectionSeeds(current_dt);

  // Step 1: Assign people to activities using pre-computed schedules
  // Update current time for policy checks
  {
    ScopedTimer timer("01_ActivityAssignment");
    activity_manager_.setCurrentTime(current_simulation_time_);
    activity_manager_.assignActivitiesFromSchedule(time_slot_index,
                                                   day_type_idx, locations_);
  }

#ifdef USE_MPI
  // Clear per-slot virtual venue registry before encounter injection.
  // Also clear stale virtual venue rank assignments so that hash-colliding
  // venue IDs from previous slots don't prevent correct re-registration.
  if (domain_mgr_) {
    domain_mgr_->getDomain().clearVirtualVenues();
    domain_mgr_->clearVirtualVenueRanks();
  }
#endif

  // Inject Coordinated Encounters for this timeslot into the locations
  // array
  if (coordinated_encounter_manager_) {
    // Build encounter_type_id -> trigger activity indices map (once per slot,
    // cheap)
    std::unordered_map<uint8_t, std::vector<int16_t>>
        encounter_trigger_activities;
    for (const auto& def : config_.coordinated_encounters.encounters) {
      int type_id = world_.getEncounterTypeIndex(def.name);
      if (type_id >= 0) {
        std::vector<int16_t> indices;
        for (const auto& slot_name : def.trigger_slots) {
          int idx = world_.getActivityIndex(slot_name);
          if (idx >= 0) indices.push_back(static_cast<int16_t>(idx));
        }
        encounter_trigger_activities[static_cast<uint8_t>(type_id)] =
            std::move(indices);
      }
    }

    // Build encounter_type_id -> min_attendees lookup
    std::unordered_map<uint8_t, int> encounter_min_attendees;
    for (const auto& def : config_.coordinated_encounters.encounters) {
      int type_id = world_.getEncounterTypeIndex(def.name);
      if (type_id >= 0) {
        encounter_min_attendees[static_cast<uint8_t>(type_id)] =
            def.min_attendees;
      }
    }

    // Deduplicate encounters: when both A→B and B→A proposals are
    // accepted for the same slot, two encounters exist for the same pair.
    // Keep the one with the lowest encounter_id for determinism across ranks.
    auto daily_encounters =
        coordinated_encounter_manager_->getDailyEncounters();
    {
      // Build key = sorted participant set + slot -> best encounter_id index
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
      // Sort by encounter_id for deterministic injection order.
      // When a person has multiple encounters at the same slot,
      // the first one processed wins (later ones skip already-assigned people).
      std::sort(
          deduped.begin(), deduped.end(),
          [](const CoordinatedEncounter& a, const CoordinatedEncounter& b) {
            return a.encounter_id < b.encounter_id;
          });
      daily_encounters = std::move(deduped);
    }
    // === Two-pass encounter injection with MPI eligibility exchange ===
    // Pass 1: Compute local eligibility for ALL encounters in this slot.
    // In MPI mode, remote participants cannot be assumed eligible — they
    // may be policy-blocked or dead on their home rank. We exchange
    // local_eligible counts across ranks so every rank sees the true
    // global eligible count before making injection decisions.
    struct EncounterEligibility {
      int encounter_idx;                     // index into daily_encounters
      std::vector<size_t> eligible_indices;  // local people passing policy
      int local_eligible;
      int min_required;
    };
    std::vector<EncounterEligibility> slot_encounters;

    for (int ei = 0; ei < static_cast<int>(daily_encounters.size()); ++ei) {
      const auto& enc = daily_encounters[ei];
      if (enc.slot != time_slot_index) continue;

      auto trig_it = encounter_trigger_activities.find(enc.encounter_type_id);

      std::vector<size_t> eligible_indices;
      for (PersonId pid : enc.participants) {
        auto it = world_.person_index.find(pid);
        if (it == world_.person_index.end()) continue;

        size_t array_idx = it->second;
        if (array_idx >= locations_.size()) continue;

        const Person& person = world_.people[array_idx];
        if (person.is_dead) continue;

        bool policy_blocked = false;
        if (policy_manager_ && trig_it != encounter_trigger_activities.end()) {
          for (int16_t trigger_act_idx : trig_it->second) {
            auto override = policy_manager_->getOverride(
                const_cast<Person&>(world_.people[array_idx]), trigger_act_idx,
                locations_[array_idx].venue_id,
                locations_[array_idx].subset_index, current_simulation_time_,
                time_slot_index);
            if (override.has_value()) {
              policy_blocked = true;
              break;
            }
          }
        }
        if (!policy_blocked) {
          eligible_indices.push_back(array_idx);
        }
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

    // Build per-encounter global eligible counts. In MPI mode, each rank
    // only knows eligibility for its local participants. We exchange
    // (encounter_id, local_eligible) pairs so each rank can compute the
    // true global eligible count before making injection decisions.
    // This prevents the bug where a remote participant is assumed eligible
    // but is actually policy-blocked or dead on their home rank.
    std::unordered_map<int, int>
        global_eligible_map;  // encounter_id -> global count

#ifdef USE_MPI
    if (domain_mgr_) {
      // Collect (encounter_id, local_eligible) for encounters with any
      // remote participants — these are the only ones that need exchange.
      std::vector<int>
          local_pairs;  // flat array: [eid, count, eid, count, ...]
      for (const auto& ee : slot_encounters) {
        const auto& enc = daily_encounters[ee.encounter_idx];
        // Check if this encounter has any remote participants
        bool has_remote = false;
        for (PersonId pid : enc.participants) {
          if (world_.person_index.find(pid) == world_.person_index.end()) {
            has_remote = true;
            break;
          }
        }
        if (has_remote) {
          local_pairs.push_back(enc.encounter_id);
          local_pairs.push_back(ee.local_eligible);
        }
      }

      // MPI_Allgatherv to share eligibility data across all ranks
      int local_count = static_cast<int>(local_pairs.size());
      int num_ranks = domain_mgr_->getNumRanks();
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

      // Sum eligible counts per encounter_id across all ranks
      for (int i = 0; i < total; i += 2) {
        int eid = all_pairs[i];
        int eligible = all_pairs[i + 1];
        global_eligible_map[eid] += eligible;
      }
    }
#endif

    // Pass 2: Inject encounters using global eligible counts.
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

      if (total_eligible >= ee.min_required && ee.local_eligible > 0) {
        for (size_t array_idx : ee.eligible_indices) {
          locations_[array_idx].venue_id = enc.venue_id;
          locations_[array_idx].encounter_type_id = enc.encounter_type_id;
        }

#ifdef USE_MPI
        // Register virtual venue ownership so the visitor exchange can
        // route cross-rank encounter participants to the host's rank.
        // Physical venues already have ownership via the venue ownership
        // map; virtual venues (negative IDs) need explicit registration.
        if (domain_mgr_ && enc.venue_id < 0) {
          int host_rank = domain_mgr_->getPersonRank(enc.host_id);
          domain_mgr_->setVenueRank(enc.venue_id, host_rank);
          if (host_rank == domain_mgr_->getRank()) {
            domain_mgr_->getDomain().registerVirtualVenue(enc.venue_id);
          }
        }
#endif
      }
    }
  }

  // Per-slot venue distribution (aggregated across ranks).
  // Index by activity_index over the rank-consistent world_.activity_names
  // so the Allreduce buffer size is identical on every rank. Collapsing by
  // activity name via std::map produces a rank-local size (the map only
  // contains keys for activities locally present) and breaks Allreduce with
  // MPI_ERR_TRUNCATE as soon as ranks diverge — e.g. when one rank has
  // hospitalised people routed to medical_facility and the other doesn't.
  {
    const size_t num_activities = world_.activity_names.size();
    std::vector<int> local_counts(num_activities, 0);
    for (const auto& loc : locations_) {
      if (loc.activity_index >= 0 &&
          loc.activity_index < static_cast<int>(num_activities)) {
        local_counts[loc.activity_index]++;
      }
    }
    std::vector<int> global_counts(num_activities, 0);
#ifdef USE_MPI
    if (domain_mgr_) {
      // Rank-0 print only — Reduce instead of Allreduce; non-root ranks
      // don't need the aggregated result.
      MPI_Reduce(local_counts.data(), global_counts.data(),
                 static_cast<int>(num_activities), MPI_INT, MPI_SUM, 0,
                 MPI_COMM_WORLD);
    } else {
      global_counts = local_counts;
    }
#else
    global_counts = local_counts;
#endif
    if (rank == 0) {
      std::cout << "      → ";
      for (size_t i = 0; i < num_activities; ++i) {
        if (global_counts[i] > 0) {
          std::cout << world_.activity_names[i] << ": " << global_counts[i]
                    << "  ";
        }
      }
      std::cout << std::endl;
    }
  }

#ifdef USE_MPI
  // Step 2: Exchange visitors between domains (parallel mode only)
  std::vector<PersonLocation> augmented_locations;
  std::unordered_set<PersonId> visitor_ids;
  std::vector<PendingInfection> pending_infections;
  std::unordered_map<PersonId, VisitorInfo>
      visitor_data_map;  // Visitor data for transmission

  if (domain_mgr_ != nullptr) {
    try {
      ScopedTimer timer("02_MPI_VisitorExchange");
      domain_mgr_->exchangeVisitors(locations_, current_simulation_time_,
                                    delta_hours);

      Domain& domain = domain_mgr_->getDomain();

      // === Filter out outgoing visitors (local people at remote venues)
      // ===
      // We only keep local people at LOCAL venues. People at remote
      // venues are handled exclusively as visitors on the owning rank.

      augmented_locations.reserve(locations_.size() +
                                  domain.incoming_visitors.size());
      int outgoing_filtered = 0;
      for (const auto& loc : locations_) {
        if (loc.venue_id == -1) {
          augmented_locations.push_back(loc);  // unallocated, keep
          continue;
        }
        if (domain.ownsVenue(loc.venue_id)) {
          augmented_locations.push_back(loc);  // local venue, keep
        } else {
          outgoing_filtered++;  // remote venue, skip (handled as visitor)
        }
      }

      // Add incoming visitors to augmented locations
      size_t visitor_start = augmented_locations.size();
      for (const auto& visitor : domain.incoming_visitors) {
        PersonLocation visitor_loc;
        visitor_loc.person_id = visitor.person_id;
        visitor_loc.venue_id = visitor.venue_id;
        visitor_loc.subset_index = visitor.subset_idx;
        visitor_loc.activity_index =
            static_cast<int16_t>(world_.getActivityIndex("visiting"));
        visitor_loc.encounter_type_id = visitor.encounter_type_id;
        augmented_locations.push_back(visitor_loc);
      }

      // Sort visitor portion by person_id for deterministic processing order
      // (MPI message arrival order is non-deterministic)
      std::sort(augmented_locations.begin() + visitor_start,
                augmented_locations.end(),
                [](const PersonLocation& a, const PersonLocation& b) {
                  return a.person_id < b.person_id;
                });

      // Get visitor IDs for InteractionManager
      visitor_ids = domain_mgr_->getVisitorIds();

      // Populate visitor data map for transmission calculations
      for (const auto& visitor : domain.incoming_visitors) {
        VisitorInfo info;
        info.person_id = visitor.person_id;
        info.is_infected = visitor.is_infected;
        info.is_infectious = visitor.is_infectious;
        info.immunity_level = visitor.immunity_level;
        info.symptom_id = visitor.symptom_id;
        info.time_in_stage = visitor.time_in_stage;
        std::copy(std::begin(visitor.integrated_infectiousness),
                  std::end(visitor.integrated_infectiousness),
                  std::begin(info.integrated_infectiousness));
        visitor_data_map[visitor.person_id] = info;
      }
    } catch (const std::exception& e) {
      std::cerr << "[Step 2 MPI] Fatal error: " << e.what() << std::endl;
      throw;
    }
  } else {
    augmented_locations = locations_;
  }

  // Use augmented locations (locals + visitors) for transmission processing
  std::vector<PersonLocation>& transmission_locations =
      domain_mgr_ ? augmented_locations : locations_;
#else
  // Serial mode: use original locations
  std::vector<PersonLocation>& transmission_locations = locations_;
#endif

  // Step 3: Calculate contacts and transmission (pass active_infections)
  int local_new_infections = 0;
  {
    try {
      ScopedTimer timer("03_TransmissionProcessing");
      if (interaction_manager_) {
        interaction_manager_->setCurrentDayTypeIdx(day_type_idx);
      }
#ifdef USE_MPI
      if (domain_mgr_ != nullptr) {
        local_new_infections = interaction_manager_->processTransmissions(
            transmission_locations, current_simulation_time_, delta_hours,
            &epidemiology_->getActiveInfectionsMutable(), &visitor_ids,
            &pending_infections, &visitor_data_map,
            compartmental_model_manager_.get());
      } else {
        local_new_infections = interaction_manager_->processTransmissions(
            transmission_locations, current_simulation_time_, delta_hours,
            &epidemiology_->getActiveInfectionsMutable(), nullptr, nullptr,
            nullptr, compartmental_model_manager_.get());
      }
#else
      local_new_infections = interaction_manager_->processTransmissions(
          transmission_locations, current_simulation_time_, delta_hours,
          &epidemiology_->getActiveInfectionsMutable(), nullptr, nullptr,
          nullptr, compartmental_model_manager_.get());
#endif
    } catch (const std::exception& e) {
      std::cerr << "[Step 3 Transmission] Fatal error: " << e.what()
                << std::endl;
      throw;
    }
  }

#ifdef USE_MPI
  // Step 4: Send back pending infections to home ranks (parallel mode only)
  if (domain_mgr_ != nullptr) {
    int pending_queued = static_cast<int>(pending_infections.size());
    try {
      auto mpi_infected =
          domain_mgr_->receivePendingInfections(pending_infections);
      for (const auto& applied : mpi_infected) {
        epidemiology_->trackInfection(applied.person_id);
        event_logger_.logInfection(
            applied.person_id, applied.infector_id, applied.venue_id,
            applied.infection_time, applied.encounter_type_id,
            applied.infector_symptom_id, applied.transmission_mode_index);
      }

    } catch (const std::exception& e) {
      std::cerr << "[Step 4 Receive Pending] Fatal error: " << e.what()
                << std::endl;
      throw;
    }
  }
#endif

  // Step 5: Update infection states (symptom changes, recoveries, deaths)
  // This is done AFTER transmission so that:
  // - Newly infected people are tracked
  // - Death processing happens at end of time slot
  // - Fatal symptoms are non-infectious anyway (checked in isInfectiousStage)
  EpiSlotStats epi_stats;
  {
    try {
      ScopedTimer timer("04_InfectionStateUpdates");
      epi_stats = epidemiology_->updateInfectionStates(current_simulation_time_,
                                                       locations_);
    } catch (const std::exception& e) {
      std::cerr << "[Step 5 Infection Updates] Fatal error: " << e.what()
                << std::endl;
      throw;
    }
  }

  // Step 6: Update venue fomites (decay)
  try {
    epidemiology_->updateVenueFomites(current_simulation_time_, delta_hours);
  } catch (const std::exception& e) {
    std::cerr << "[Step 6 Fomites] Fatal error: " << e.what() << std::endl;
    throw;
  }

  // Per-slot transmission and epidemiology summary (aggregated across ranks)
  {
    int global_new_infections = local_new_infections;
    int local_epi[4] = {epi_stats.transitions, epi_stats.recoveries,
                        epi_stats.deaths, epi_stats.active_remaining};
    int global_epi[4];
#ifdef USE_MPI
    if (domain_mgr_) {
      MPI_Reduce(&local_new_infections, &global_new_infections, 1, MPI_INT,
                 MPI_SUM, 0, MPI_COMM_WORLD);
      MPI_Reduce(local_epi, global_epi, 4, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);
    } else {
      std::copy(std::begin(local_epi), std::end(local_epi),
                std::begin(global_epi));
    }
#else
    std::copy(std::begin(local_epi), std::end(local_epi),
              std::begin(global_epi));
#endif
    if (rank == 0) {
      if (global_new_infections > 0) {
        std::cout << "      [Transmission] " << global_new_infections
                  << " new infections (duration=" << delta_hours << "h)"
                  << std::endl;
      }
      if (global_epi[0] > 0 || global_epi[1] > 0 || global_epi[2] > 0) {
        std::cout << "      [Epidemiology] Processed " << global_epi[0]
                  << " symptom transitions. " << global_epi[1]
                  << " recoveries, " << global_epi[2] << " deaths. "
                  << "Active infections remaining: " << global_epi[3]
                  << std::endl;
      }
    }
  }

  // Deposition write-back: aggregate per-node contributions from infected
  // people at owned venues and forward to the plugin for the next advance()
  // call.
  compartmental_model_manager_->computeDepositionWriteback(
      locations_, world_, *disease_,
      current_simulation_time_ - delta_hours / 24.0, current_simulation_time_);

  compartmental_model_manager_->maybeSnapshot(
      static_cast<float>(current_simulation_time_));
}

void Simulator::outputStatistics() {
  int rank = 0;
  int size = 1;
#ifdef USE_MPI
  if (domain_mgr_) {
    rank = domain_mgr_->getRank();
    size = domain_mgr_->getNumRanks();
  }
#endif

#ifdef USE_MPI
  if (domain_mgr_) {
    domain_mgr_->reportDomainStats("Periodic Statistics (Day " +
                                   std::to_string(current_day_num_) + ")");
  }
#endif

  // Count local people by activity
  std::vector<int> local_activity_counts(world_.activity_names.size(), 0);
  for (const auto& loc : locations_) {
    if (loc.activity_index >= 0 &&
        loc.activity_index < (int)world_.activity_names.size()) {
      local_activity_counts[loc.activity_index]++;
    }
  }

  std::vector<int> global_activity_counts(world_.activity_names.size(), 0);
#ifdef USE_MPI
  if (domain_mgr_) {
    MPI_Reduce(local_activity_counts.data(), global_activity_counts.data(),
               static_cast<int>(local_activity_counts.size()), MPI_INT, MPI_SUM,
               0, MPI_COMM_WORLD);
  } else {
    global_activity_counts = local_activity_counts;
  }
#else
  global_activity_counts = local_activity_counts;
#endif

  if (rank == 0) {
    std::cout << "\n--- Statistics (Day " << current_day_num_ << ") ---"
              << std::endl;
    std::cout << "People by activity:" << std::endl;
    for (size_t i = 0; i < world_.activity_names.size(); ++i) {
      if (global_activity_counts[i] > 0) {
        std::cout << "  " << world_.activity_names[i] << ": "
                  << global_activity_counts[i] << std::endl;
      }
    }
  }

  // Output infection statistics (collective call — all ranks participate)
  outputInfectionStatistics();
}

void Simulator::printSimulationState(const std::string& time_slot_name,
                                     double delta_hours) {
  int rank = 0;
#ifdef USE_MPI
  if (domain_mgr_) rank = domain_mgr_->getRank();
#endif
  if (rank == 0) {
    std::cout << "    [" << time_slot_name << "] duration = " << std::fixed
              << std::setprecision(2) << delta_hours << " hours" << std::endl;
  }
}

void Simulator::applyInfectionSeeds(const std::string& current_datetime) {
  std::vector<PersonId> newly_infected = infection_seeder_->seedInfections(
      current_datetime, current_simulation_time_);
  int local_count = static_cast<int>(newly_infected.size());
  int global_count = local_count;
#ifdef USE_MPI
  if (domain_mgr_) {
    MPI_Allreduce(&local_count, &global_count, 1, MPI_INT, MPI_SUM,
                  MPI_COMM_WORLD);
  }
#endif

  if (global_count > 0) {
    int rank = 0;
#ifdef USE_MPI
    if (domain_mgr_) rank = domain_mgr_->getRank();
#endif
    if (rank == 0) {
      std::cout << "    [INFECTION SEED] Seeded " << global_count
                << " infections" << std::endl;
    }

    // Add seeded infections to active tracking set locally
    for (PersonId pid : newly_infected) {
      epidemiology_->trackInfection(pid);
    }
  }
}

void Simulator::outputInfectionStatistics() {
  int rank = 0;
  int size = 1;
#ifdef USE_MPI
  if (domain_mgr_) {
    rank = domain_mgr_->getRank();
    size = domain_mgr_->getNumRanks();
  }
#endif

  // Local counts and breakdown
  int local_infected = 0;
  int local_immune = 0;
  int local_dead = 0;

  const auto& s_tags = disease_->getSymptomTags();
  std::vector<int> local_symptom_counts(s_tags.size(), 0);

  for (const auto& person : world_.people) {
    if (person.is_dead) {
      local_dead++;
      continue;
    }

    if (person.infection != nullptr) {
      uint16_t s_id = person.infection->getTrajectory().getCurrentSymptomId(
          current_simulation_time_);
      if (s_id < s_tags.size()) {
        const auto& tag = s_tags[s_id];
        if (disease_->isFatalStage(tag.name)) {
          local_dead++;
        } else {
          local_infected++;
        }
        local_symptom_counts[s_id]++;
      }
    }
    if (person.immunity.getNaturalLevel(current_simulation_time_) > 0.01 ||
        person.vaccine_trajectory != nullptr) {
      local_immune++;
    }
  }

  // Prepare for aggregation
  int local_totals[3] = {local_infected, local_immune, local_dead};
  int global_totals[3] = {0, 0, 0};

  std::vector<int> global_symptom_counts(s_tags.size(), 0);

#ifdef USE_MPI
  if (domain_mgr_ && size > 1) {
    // Sanity check: ensure all ranks have the same number of symptom tags
    int local_s_size = static_cast<int>(s_tags.size());
    int min_s_size, max_s_size;
    MPI_Allreduce(&local_s_size, &min_s_size, 1, MPI_INT, MPI_MIN,
                  MPI_COMM_WORLD);
    MPI_Allreduce(&local_s_size, &max_s_size, 1, MPI_INT, MPI_MAX,
                  MPI_COMM_WORLD);

    if (min_s_size != max_s_size) {
      if (rank == 0)
        std::cerr << "MPI Error: Mismatch in symptom tag counts across ranks ("
                  << min_s_size << " vs " << max_s_size << ")" << std::endl;
      return;
    }

    MPI_Reduce(local_totals, global_totals, 3, MPI_INT, MPI_SUM, 0,
               MPI_COMM_WORLD);
    MPI_Reduce(local_symptom_counts.data(), global_symptom_counts.data(),
               static_cast<int>(local_symptom_counts.size()), MPI_INT, MPI_SUM,
               0, MPI_COMM_WORLD);
  } else {
    std::copy(std::begin(local_totals), std::end(local_totals),
              std::begin(global_totals));
    global_symptom_counts = local_symptom_counts;
  }
#else
  std::copy(std::begin(local_totals), std::end(local_totals),
            std::begin(global_totals));
  global_symptom_counts = local_symptom_counts;
#endif

  if (rank == 0) {
    std::cout << "\n--- Infection Statistics ---" << std::endl;
    std::cout << "  Total currently infected: " << global_totals[0]
              << std::endl;
    std::cout << "  Total with immunity: " << global_totals[1] << std::endl;
    std::cout << "  Total deaths: " << global_totals[2] << std::endl;

    bool has_symptoms = false;
    for (int c : global_symptom_counts)
      if (c > 0) {
        has_symptoms = true;
        break;
      }

    if (has_symptoms) {
      std::cout << "  Breakdown by symptom:" << std::endl;
      for (size_t i = 0; i < s_tags.size(); ++i) {
        if (global_symptom_counts[i] > 0) {
          std::cout << "    " << std::setw(20) << std::left << s_tags[i].name
                    << ": " << global_symptom_counts[i] << std::endl;
        }
      }
    }
    std::cout << std::endl;
  }
}

void Simulator::checkAndFlushEvents(bool is_day_end) {
  bool should_flush = false;

  // 1. Time-based trigger (days)
  if (is_day_end && config_.simulation.flush_interval_days > 0) {
    if ((current_day_num_ + 1) % config_.simulation.flush_interval_days == 0) {
      should_flush = true;
    }
  }

  // 2. Buffer-size based trigger (can happen anytime/mid-day)
  if (event_logger_.getTotalRecordCount() >=
      (size_t)config_.simulation.max_event_buffer_size) {
    should_flush = true;
  }

  if (should_flush && event_logger_.getTotalRecordCount() > 0) {
    int rank = 0;
#ifdef USE_MPI
    if (domain_mgr_) rank = domain_mgr_->getRank();
#endif
    // Streaming logger flush message removed — internal plumbing

    // Identify people who need their lookup records written (not already
    // written)
    std::unordered_set<PersonId> newly_infected;
    if (config_.simulation.save_full_person_details == "infected_only") {
      for (PersonId pid : event_logger_.getInfectedPersonIds()) {
        if (lookups_written_.find(pid) == lookups_written_.end()) {
          newly_infected.insert(pid);
          lookups_written_.insert(pid);
        }
      }
    }

    {
      ScopedTimer timer("05_HDF5_Flushing");
      event_logger_.flush(
          events_filename_, config_, world_,
          newly_infected.empty() &&
                  config_.simulation.save_full_person_details == "infected_only"
              ? nullptr
              : &newly_infected);
    }
  }
}
void Simulator::initFomiteState() {
  if (!disease_) return;
  const auto& modes = disease_->getTransmissionParams().modes;
  int num_fomite_modes = 0;
  for (const auto& tmode : modes) {
    if (tmode.type == TransmissionModeType::Fomite) ++num_fomite_modes;
  }
  if (num_fomite_modes == 0) return;
  for (auto& venue : world_.venues) {
    venue.fomite_history.assign(num_fomite_modes, {});
  }
}

}  // namespace june
