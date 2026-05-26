#include "simulation/simulator.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <vector>

namespace june {

namespace {

// Per-rank infection breakdown: walk the local population once and tally
// currently-infected / immune / dead totals plus a per-symptom-tag
// histogram. Pure read of world + disease state.
struct InfectionStatsLocal {
  int infected = 0;
  int immune = 0;
  int dead = 0;
  std::vector<int> symptom_counts;
};

InfectionStatsLocal tallyLocalInfectionStats(const WorldState& world,
                                             double current_simulation_time,
                                             const Disease& disease) {
  const auto& s_tags = disease.getSymptomTags();
  InfectionStatsLocal out;
  out.symptom_counts.assign(s_tags.size(), 0);
  for (const auto& person : world.people) {
    if (person.is_dead) {
      out.dead++;
      continue;
    }
    if (person.infection != nullptr) {
      uint16_t s_id =
          person.infection->getTrajectory().getCurrentSymptomId(
              current_simulation_time);
      if (s_id < s_tags.size()) {
        const auto& tag = s_tags[s_id];
        if (disease.isFatalStage(tag.name)) {
          out.dead++;
        } else {
          out.infected++;
        }
        out.symptom_counts[s_id]++;
      }
    }
    if (person.immunity.getNaturalLevel(current_simulation_time) > 0.01 ||
        person.vaccine_trajectory != nullptr) {
      out.immune++;
    }
  }
  return out;
}

}  // namespace

void Simulator::outputStatistics() {
  const int rank = getRank();

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

void Simulator::outputInfectionStatistics() {
  const int rank = getRank();
  int size = 1;
#ifdef USE_MPI
  if (domain_mgr_) size = domain_mgr_->getNumRanks();
#endif

  // Per-rank tally walks the local population once.
  const InfectionStatsLocal local_stats =
      tallyLocalInfectionStats(world_, current_simulation_time_, *disease_);
  const auto& s_tags = disease_->getSymptomTags();

  int local_totals[3] = {local_stats.infected, local_stats.immune,
                         local_stats.dead};
  int global_totals[3] = {0, 0, 0};
  const std::vector<int>& local_symptom_counts = local_stats.symptom_counts;
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

}  // namespace june
