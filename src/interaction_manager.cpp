// #define DEBUG_INTERACTION_MANAGER
// #define DEBUG_TRANSMISSION
#include "epidemiology/interaction_manager.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numeric>

#include "activity/presence_window.h"
#include "activity/runtime_bin_allocator.h"
#include "simulation/compartmental_model_manager.h"
#include "utils/deterministic_rng.h"
#include "utils/event_logging/event_types.h"
#include "utils/profiler.h"
#include "utils/random.h"

namespace june {

InteractionManager::InteractionManager(
    WorldState& world, const ContactMatrixConfig& contact_matrices,
    const SimulationConfig& simulation_config,
    const ParallelConfig& parallel_config, Disease* disease,
    EventLogger* event_logger)
    : world_(world),
      contact_matrices_(contact_matrices),
      simulation_config_(simulation_config),
      parallel_config_(parallel_config),
      disease_(disease),
      event_logger_(event_logger),
      base_seed_(simulation_config.random_seed) {
  // NOTE: encounter_subset_overrides_ is left empty — the port's
  // CoordinatedEncounterDef no longer carries a `subset` field.
  // Binning falls through to stage 2/3 when the map is empty.

  if (const char* dbg = std::getenv("JUNE_DEBUG_PARENT_MIXING")) {
    debug_parent_mixing_ = (dbg[0] != '\0' && dbg[0] != '0');
  }
}

// Mirror of the STEP 1 binning logic in processVenueTransmissions, but
// indexes against an arbitrary matrix (e.g. the parent's matrix in the
// sibling-mixing pre-pass). Returns 0 on failure to match — same fallback
// the main loop uses for safety.
int InteractionManager::computeBinIndexForMatrix(const Person* person,
                                                 const Venue* venue,
                                                 SubsetIndex subset_index,
                                                 uint8_t encounter_type_id,
                                                 const ContactMatrix* matrix,
                                                 int num_bins) const {
  if (!matrix || num_bins <= 1) return 0;

  int bin_index = -1;

  // Stage 1a: encounter-subset override (currently unused, kept for parity).
  if (encounter_type_id != 255) {
    auto it = encounter_subset_overrides_.find(encounter_type_id);
    if (it != encounter_subset_overrides_.end()) {
      bin_index = matrix->findBinIndex(it->second);
    }
  }

  // Stage 2: subset-based binning. The parent matrix may have NO entries
  // for the child's subset_type_names (e.g. school matrix has bins=[all]
  // while classroom subsets are student/worker). In that case
  // bin_by_subset_type[...] is -1 and we fall through to stage 3.
  if (bin_index < 0 && venue && subset_index >= 0) {
    auto subsets = world_.getSubsets(*venue);
    if (subset_index < (int)subsets.size()) {
      const auto& subset = subsets[subset_index];
      if (subset.subset_type_id < matrix->bin_by_subset_type.size()) {
        bin_index = matrix->bin_by_subset_type[subset.subset_type_id];
      }
    }
  }

  // Stage 3: property-based (sex first, then age).
  if (bin_index < 0 && person) {
    int sex_bin = (person->sex == Sex::MALE)     ? matrix->male_bin
                  : (person->sex == Sex::FEMALE) ? matrix->female_bin
                                                 : -1;
    if (sex_bin >= 0) {
      bin_index = sex_bin;
    } else {
      int age_int = std::min(static_cast<int>(person->age), 99);
      if (age_int >= 0) bin_index = matrix->age_to_bin[age_int];
    }
  }

  if (bin_index < 0 || bin_index >= num_bins) bin_index = 0;
  return bin_index;
}

void InteractionManager::buildParentAggregates(
    double current_time, double delta_hours,
    const std::unordered_map<PersonId, VisitorInfo>* visitor_data) {
  parent_aggregates_.clear();

  if (active_locations_buffer_.empty()) return;

  int num_modes = disease_->numModes();
  if (num_modes == 0) num_modes = 1;

  // Walk venue groups in the SAME order the main loop uses, so debug
  // output and any FP accumulations line up.
  size_t i = 0;
  while (i < active_locations_buffer_.size()) {
    size_t group_start = i;
    const auto& first = active_locations_buffer_[group_start];
    while (
        i < active_locations_buffer_.size() &&
        active_locations_buffer_[i].venue_id == first.venue_id &&
        (first.venue_id >= 0 || active_locations_buffer_[i].encounter_type_id ==
                                    first.encounter_type_id)) {
      i++;
    }

    // Virtual encounters (venue_id < 0) have no parent.
    if (first.venue_id < 0) continue;

    Venue* venue = world_.getVenue(first.venue_id);
    if (!venue) continue;
    if (venue->parent_id < 0) continue;

    Venue* parent_venue = world_.getVenue(venue->parent_id);
    if (!parent_venue) continue;
    uint8_t parent_type_id = parent_venue->type_id;
    const ContactMatrix* parent_matrix =
        contact_matrices_.getMatrix(parent_type_id);
    if (!parent_matrix) continue;
    int parent_num_bins =
        std::max(1, static_cast<int>(parent_matrix->bins.size()));

    auto& agg = parent_aggregates_[venue->parent_id];
    if (agg.total_inf_by_bin_mode.empty()) {
      agg.total_inf_by_bin_mode.assign(parent_num_bins,
                                       std::vector<double>(num_modes, 0.0));
      agg.size_by_bin.assign(parent_num_bins, 0);
      agg.infectors_by_bin.assign(parent_num_bins, {});
      agg.parent_venue_type_id = parent_type_id;
    }

    auto& csize = agg.child_size_by_bin[first.venue_id];
    if (csize.empty()) csize.assign(parent_num_bins, 0);
    auto& cinf = agg.child_inf_by_bin_mode[first.venue_id];
    if (cinf.empty()) {
      cinf.assign(parent_num_bins, std::vector<double>(num_modes, 0.0));
    }

    // Walk members of this venue group. The locations buffer is already
    // sorted by venue_id but NOT by person_id within a venue — match the
    // main loop's STEP 1 ordering by sorting member ids here too, so the
    // sibling FP sum order is identical across np configurations.
    std::vector<PersonLocation> mem_sorted;
    mem_sorted.reserve(i - group_start);
    for (size_t k = group_start; k < i; ++k) {
      mem_sorted.push_back(active_locations_buffer_[k]);
    }
    std::sort(mem_sorted.begin(), mem_sorted.end(),
              [](const PersonLocation& a, const PersonLocation& b) {
                return a.person_id < b.person_id;
              });

    for (const auto& loc : mem_sorted) {
      PersonId pid = loc.person_id;
      Person* person = nullptr;
      if (loc.person_array_index < world_.people.size()) {
        person = &world_.people[loc.person_array_index];
        if (person->id != pid) person = world_.getPerson(pid);
      } else {
        person = world_.getPerson(pid);
      }

      const VisitorInfo* visitor = nullptr;
      if (!person && visitor_data) {
        auto vit = visitor_data->find(pid);
        if (vit != visitor_data->end()) visitor = &vit->second;
      }
      if (!person && !visitor) continue;
      if (person && person->is_dead) continue;

      int parent_bin = computeBinIndexForMatrix(person, venue, loc.subset_index,
                                                loc.encounter_type_id,
                                                parent_matrix, parent_num_bins);

      // Headcount (matches BinGroup::total_size convention)
      agg.size_by_bin[parent_bin]++;
      csize[parent_bin]++;

      // Infectiousness contribution
      bool is_infectious = false;
      std::vector<double> inf_by_mode(num_modes, 0.0);
      if (visitor) {
        if (visitor->is_infectious) {
          double total = 0.0;
          for (int m = 0; m < num_modes; ++m) {
            inf_by_mode[m] = (m < VisitorInfo::MAX_MODES)
                                 ? visitor->integrated_infectiousness[m]
                                 : 0.0;
            total += inf_by_mode[m];
          }
          is_infectious = total > 0.0;
        }
      } else if (person && person->infection &&
                 person->infection->isInfectious(current_time)) {
        const double t1 = current_time + delta_hours / 24.0;
        double total = 0.0;
        for (int m = 0; m < num_modes; ++m) {
          inf_by_mode[m] = person->infection->getIntegratedInfectiousness(
              m, current_time, t1);
          total += inf_by_mode[m];
        }
        is_infectious = total > 0.0;
      }

      if (is_infectious) {
        for (int m = 0; m < num_modes; ++m) {
          agg.total_inf_by_bin_mode[parent_bin][m] += inf_by_mode[m];
          cinf[parent_bin][m] += inf_by_mode[m];
        }
        ParentInfectorEntry entry;
        entry.person_id = pid;
        entry.child_venue_id = first.venue_id;
        entry.inf_by_mode = std::move(inf_by_mode);
        agg.infectors_by_bin[parent_bin].push_back(std::move(entry));
      }
    }
  }

  if (debug_parent_mixing_) {
    // Summary: per-parent totals. Sort keys for deterministic print order.
    std::vector<VenueId> keys;
    keys.reserve(parent_aggregates_.size());
    for (const auto& kv : parent_aggregates_) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());

    int nonzero = 0;
    for (VenueId pid : keys) {
      const auto& a = parent_aggregates_[pid];
      double total_inf = 0.0;
      int total_people = 0;
      for (int b = 0; b < (int)a.total_inf_by_bin_mode.size(); ++b) {
        total_people += a.size_by_bin[b];
        for (double v : a.total_inf_by_bin_mode[b]) total_inf += v;
      }
      if (total_inf > 0.0) nonzero++;
    }
    std::cerr << "[PMIX] t=" << current_time << " dt=" << delta_hours
              << " n_parents=" << parent_aggregates_.size()
              << " n_parents_with_infectious=" << nonzero << std::endl;

    // Print details of up to 3 parents with non-zero infectiousness
    int printed = 0;
    for (VenueId pid : keys) {
      if (printed >= 3) break;
      const auto& a = parent_aggregates_[pid];
      double total_inf = 0.0;
      for (auto& v : a.total_inf_by_bin_mode)
        for (double x : v) total_inf += x;
      if (total_inf <= 0.0) continue;
      std::cerr << "[PMIX]   parent_venue=" << pid
                << " parent_type=" << (int)a.parent_venue_type_id
                << " n_children=" << a.child_size_by_bin.size();
      for (int b = 0; b < (int)a.total_inf_by_bin_mode.size(); ++b) {
        std::cerr << " bin[" << b << "](size=" << a.size_by_bin[b]
                  << ",inf=" << a.infectors_by_bin[b].size() << ",I=[";
        for (int m = 0; m < (int)a.total_inf_by_bin_mode[b].size(); ++m) {
          if (m) std::cerr << ",";
          std::cerr << a.total_inf_by_bin_mode[b][m];
        }
        std::cerr << "])";
      }
      std::cerr << std::endl;
      printed++;
    }
  }
}

int InteractionManager::processTransmissions(
    const std::vector<PersonLocation>& locations, double current_time,
    double delta_hours, std::unordered_set<PersonId>* active_infections,
    const std::unordered_set<PersonId>* visitor_ids,
    std::vector<PendingInfection>* pending_infections,
    const std::unordered_map<PersonId, VisitorInfo>* visitor_data,
    const CompartmentalModelManager* comp_model) {
  int total_new_infections = 0;

  // 1. Filter out unallocated people into the active buffer
  active_locations_buffer_.clear();
  active_locations_buffer_.reserve(locations.size());
  for (const auto& loc : locations) {
    if (loc.venue_id != -1 || loc.encounter_type_id != 255) {
      active_locations_buffer_.push_back(loc);
    }
  }

  if (active_locations_buffer_.empty()) return 0;

  // 2. Sort by interaction site: venue_id or encounter_type_id
  stats_.grouping_ops++;
  std::sort(active_locations_buffer_.begin(), active_locations_buffer_.end(),
            [](const PersonLocation& a, const PersonLocation& b) {
              if (a.venue_id != b.venue_id) return a.venue_id < b.venue_id;
              // Group by encounter_type_id ONLY for virtual venues (venue_id <
              // 0)
              if (a.venue_id < 0)
                return a.encounter_type_id < b.encounter_type_id;
              return false;
            });

  // 2b. Build parent-venue aggregates (sibling-mixing). Walks the same
  // venue groups but under each PARENT's contact matrix. All child venues
  // of a parent share its MGU, so this is rank-local — see
  // project_venue_hierarchy_mgu memory.
  dbg_sibling_infections_ = 0;
  dbg_sample_susc_prints_ = 0;
  dbg_sample_infection_prints_ = 0;
  buildParentAggregates(current_time, delta_hours, visitor_data);

  // 3. Process groups linearly
  size_t i = 0;
  while (i < active_locations_buffer_.size()) {
    size_t group_start = i;
    const auto& first = active_locations_buffer_[group_start];

    // Find end of this group
    group_members_buffer_.clear();
    while (
        i < active_locations_buffer_.size() &&
        active_locations_buffer_[i].venue_id == first.venue_id &&
        (first.venue_id >= 0 || active_locations_buffer_[i].encounter_type_id ==
                                    first.encounter_type_id)) {
      group_members_buffer_.push_back(
          {active_locations_buffer_[i].person_id,
           active_locations_buffer_[i].person_array_index,
           active_locations_buffer_[i].subset_index,
           active_locations_buffer_[i].encounter_type_id});
      i++;
    }

    if (group_members_buffer_.size() > 0) {
      // Sort members by person_id BEFORE binning to ensure deterministic
      // floating-point accumulation order for total_infectiousness_by_mode.
      // In MPI mode, locals and visitors arrive in different order than
      // single-rank mode — without this sort, the sum of infectiousness
      // values can differ in the last bits due to FP non-associativity.
      std::sort(group_members_buffer_.begin(), group_members_buffer_.end(),
                [](const InteractionMember& a, const InteractionMember& b) {
                  return a.id < b.id;
                });

      Venue* venue = world_.getVenue(first.venue_id);

      // Skip if person has no venue AND no encounter type
      if (!venue && first.encounter_type_id == 255) {
        continue;
      }

      // --- Logging Coordinated Encounters ---
      if (event_logger_) {
        int encounter_participants = 0;
        for (size_t idx = group_start; idx < i; ++idx) {
          PersonId pid = active_locations_buffer_[idx].person_id;
          bool is_visitor = (visitor_ids && visitor_ids->count(pid) > 0);
          if (!is_visitor && active_locations_buffer_[idx].encounter_type_id <
                                 world_.encounter_type_names.size()) {
            encounter_participants++;
          }
        }
        if (encounter_participants > 0) {
          event_logger_->logEncounterStats(current_day_type_idx_, true,
                                           encounter_participants);
        }
      }

      // Fast pre-check: skip venue entirely if no transmission is possible.
      // Fomite check scans mode deques; infectious check is O(N) direct array
      // access, short-circuiting on the first infectious person found.
      // Also skip if compartmental uptake could apply to this venue.
      bool has_fomite = venue && !venue->fomite_history.empty() &&
                        std::any_of(venue->fomite_history.begin(),
                                    venue->fomite_history.end(),
                                    [](const auto& v) { return !v.empty(); });
      bool venue_has_comp_uptake =
          comp_model != nullptr && comp_model->venueToLocalNodeIndex(
                                       static_cast<int>(first.venue_id)) >= 0;
      if (!has_fomite && !venue_has_comp_uptake) {
        bool any_infectious = false;
        for (const auto& m : group_members_buffer_) {
          if (m.array_index < world_.people.size() &&
              world_.people[m.array_index].id == m.id &&
              world_.people[m.array_index].infection != nullptr) {
            any_infectious = true;
            break;
          }
          if (visitor_data) {
            auto vit = visitor_data->find(m.id);
            if (vit != visitor_data->end() && vit->second.is_infectious) {
              any_infectious = true;
              break;
            }
          }
        }
        if (!any_infectious) continue;
      }

      try {
        total_new_infections += processVenueTransmissions(
            group_members_buffer_, venue, first.venue_id, current_time,
            delta_hours, active_infections, visitor_ids, pending_infections,
            visitor_data, first.encounter_type_id, comp_model);
      } catch (const std::exception& e) {
        std::cerr << "[Rank 0] Fatal error in venue=" << first.venue_id
                  << " encounter_type=" << (int)first.encounter_type_id << ": "
                  << e.what() << std::endl;
        throw;  // Rethrow to abort
      }
    }
  }

  // Per-slot transmission count removed — captured in DAY_SUMMARY

  if (debug_parent_mixing_ && total_new_infections > 0) {
    int non_sibling = total_new_infections - dbg_sibling_infections_;
    std::cerr << "[PMIX] t=" << current_time
              << " tick_summary: total=" << total_new_infections
              << " non_sibling=" << non_sibling
              << " sibling=" << dbg_sibling_infections_
              << " (sibling counts person-sourced cross-child only;"
              << " non_sibling includes own-venue, fomite, and"
              << " compartmental sources)" << std::endl;
  }

  return total_new_infections;
}

int InteractionManager::processVenueTransmissions(
    const std::vector<InteractionMember>& members, Venue* venue,
    VenueId actual_venue_id, double current_time, double delta_hours,
    std::unordered_set<PersonId>* active_infections,
    const std::unordered_set<PersonId>* visitor_ids,
    std::vector<PendingInfection>* pending_infections,
    const std::unordered_map<PersonId, VisitorInfo>* visitor_data,
    uint8_t encounter_type_id, const CompartmentalModelManager* comp_model) {
  // Partial-presence venues (commute lines, etc.) take a different FOI path:
  // sub-interval-aware, carriage-grouped, with per-rider effective presence
  // windows. The gate is a single bit-mask test; venue types not declared in
  // SimulationConfig::partial_presence pay no cost here. The existing
  // function body below is unchanged for every other venue type.
  if (runtime_bin_allocator_ && actual_venue_id >= 0 && venue) {
    const uint8_t vt = venue->type_id;
    const uint64_t mask =
        simulation_config_.partial_presence.enabled_venue_type_mask;
    if (vt < 64 && ((mask >> vt) & 1ULL) != 0) {
      return processPartialPresenceVenue(
          members, venue, actual_venue_id, current_time, delta_hours,
          active_infections, visitor_ids, pending_infections, visitor_data,
          encounter_type_id, comp_model);
    }
  }

  int new_infections = 0;

  // Determine venue type for parameters
  std::string venue_type = "unknown";
  uint8_t venue_type_id = 255;
  const ContactMatrix* matrix = nullptr;

  // For VIRTUAL encounters (venue_id < 0), use the encounter type's contact
  // matrix — these are purpose-built venues with no physical type.
  // Virtual-encounter matrices are keyed by encounter_type_id via
  // virtual_matrices_by_encounter_id, NOT by venue_type_id; aliasing the
  // latter produced a silent bug where every sexual-channel transmission
  // pulled contact rates from whatever unrelated venue happened to sit at
  // that integer slot.
  //
  // For PHYSICAL venues (venue_id >= 0), always use the venue's own type
  // for contact matrix selection, even if some members are encounter
  // participants. This is critical for MPI reproducibility: the encounter
  // group at a physical venue is mixed with regular patrons, and the
  // encounter_type_id of the "first" person in the group is arbitrary
  // (depends on person order, which varies with rank count).
  const bool is_virtual_encounter = actual_venue_id < 0;
  if (is_virtual_encounter &&
      encounter_type_id < world_.encounter_type_names.size()) {
    venue_type_id = encounter_type_id;
    auto vm_it = contact_matrices_.virtual_matrix_names.find(encounter_type_id);
    if (vm_it != contact_matrices_.virtual_matrix_names.end()) {
      venue_type = vm_it->second;
    } else {
      venue_type = world_.encounter_type_names[encounter_type_id];
    }
    matrix = contact_matrices_.getVirtualMatrix(encounter_type_id);
  } else if (venue) {
    venue_type_id = venue->type_id;
    if (venue_type_id < world_.venue_type_names.size()) {
      venue_type = world_.venue_type_names[venue_type_id];
    }
    stats_.matrix_lookups++;
    matrix = contact_matrices_.getMatrix(venue_type_id);
  }

  // Determine number of bins needed
  int num_bins_needed = matrix ? static_cast<int>(matrix->bins.size()) : 1;
  if (num_bins_needed == 0) num_bins_needed = 1;

  int num_modes = disease_->numModes();
  if (num_modes == 0) num_modes = 1;

  const auto& trans_params = disease_->getTransmissionParams();

  // Build ordered lists of fomite and compartmental uptake mode refs.
  struct FomiteModeRef {
    int mode_index;
    const FomiteConfig* cfg;
  };
  std::vector<FomiteModeRef> fomite_modes;
  std::vector<int> comp_uptake_modes;
  for (int midx = 0; midx < (int)trans_params.modes.size(); ++midx) {
    const auto& tmode = trans_params.modes[midx];
    if (tmode.type == TransmissionModeType::Fomite) {
      fomite_modes.push_back(
          FomiteModeRef{midx, &std::get<FomiteConfig>(tmode.config)});
    } else if (tmode.type == TransmissionModeType::CompartmentalUptake) {
      comp_uptake_modes.push_back(midx);
    }
  }
  int num_fomite_modes = static_cast<int>(fomite_modes.size());

  // Compute n_sub per fomite mode from sub_bin_time and delta_hours
  std::vector<int> n_sub_per_mode(num_fomite_modes, 1);
  for (int local_fm = 0; local_fm < num_fomite_modes; ++local_fm) {
    double sbt = fomite_modes[local_fm].cfg->sub_bin_time;
    n_sub_per_mode[local_fm] =
        (sbt > 0.0) ? std::max(1, (int)(delta_hours / sbt)) : 1;
  }

  // Grow buffer if needed; new BinGroup objects are default-initialised.
  if (static_cast<int>(bins_buffer_.size()) < num_bins_needed) {
    bins_buffer_.resize(num_bins_needed);
  }
  // Ensure all bins needed this call have correctly-sized person-data vectors.
  // Bins reused from a prior call were cleared by clearAfterUse at the end of
  // that call, but newly created bins (from the resize above) have empty
  // vectors and must be initialised before the binning loop indexes into them.
  for (int b = 0; b < num_bins_needed; ++b) {
    auto& bin = bins_buffer_[b];
    if (static_cast<int>(bin.infectiousness_by_mode.size()) != num_modes) {
      bin.clearAfterUse(num_modes);
    }
    bin.initFomiteSubBins(num_fomite_modes, n_sub_per_mode);
  }

  // === STEP 1: Group people by bin (single pass) ===
  for (const auto& member : members) {
    PersonId pid = member.id;
    Person* person = nullptr;
    if (member.array_index < world_.people.size()) {
      person = &world_.people[member.array_index];
      if (person->id != pid)
        person = world_.getPerson(pid);  // Reliability fallback
    } else {
      person = world_.getPerson(pid);
    }

    int bin_index = -1;

    // --- SUBSET BINNING ---
    if (matrix) {
      stats_.bin_lookups++;
      // 1. STAGE 1a: Explicit Encounter Subset Override (High Priority)
      if (encounter_type_id != 255) {
        auto it = encounter_subset_overrides_.find(encounter_type_id);
        if (it != encounter_subset_overrides_.end()) {
          bin_index = matrix->findBinIndex(it->second);
        }
      }

      // 2. STAGE 2: Location-Based Subset Index (Inheritance from host)
      if ((bin_index < 0) && venue && member.subset_index >= 0) {
        auto subsets = world_.getSubsets(*venue);
        if (member.subset_index < (int)subsets.size()) {
          const auto& subset = subsets[member.subset_index];
          // Use pre-resolved bin_by_subset_type instead of string lookup
          if (subset.subset_type_id < matrix->bin_by_subset_type.size()) {
            bin_index = matrix->bin_by_subset_type[subset.subset_type_id];
          }
        }
      }

      // 3. STAGE 3: Property-Based Binning Fallback (Age/Sex)
      if (bin_index < 0) {
        if (person) {
          // Try sex-based matching using pre-resolved bins
          int sex_bin = (person->sex == Sex::MALE)     ? matrix->male_bin
                        : (person->sex == Sex::FEMALE) ? matrix->female_bin
                                                       : -1;
          if (sex_bin >= 0) {
            bin_index = sex_bin;
          } else {
            // Use pre-computed age-to-bin lookup table (no string allocation)
            // Clamp ages >= 100 to 99 so they map to the highest age band
            int age_int = std::min(static_cast<int>(person->age), 99);
            if (age_int >= 0) {
              bin_index = matrix->age_to_bin[age_int];
            }
          }
        }
      }
    }

    // Safety check for bin_index - if still not found, default to 0
    if (bin_index < 0 || bin_index >= num_bins_needed) {
#ifdef DEBUG_TRANSMISSION
      static int bin_fallback_count = 0;
      if (bin_fallback_count < 20) {
        std::cerr << "[DEBUG_TRANSMISSION] bin_index fallback to 0: "
                  << "person=" << pid
                  << " age=" << (person ? (int)person->age : -1)
                  << " sex=" << (person ? (int)person->sex : -1)
                  << " venue_type=" << venue_type
                  << " venue_type_id=" << (int)venue_type_id
                  << " num_bins=" << num_bins_needed
                  << " original_bin=" << bin_index
                  << " encounter_type=" << (int)encounter_type_id << std::endl;
        bin_fallback_count++;
        if (bin_fallback_count == 20)
          std::cerr << "[DEBUG_TRANSMISSION] (suppressing further bin_index "
                       "fallback warnings)"
                    << std::endl;
      }
#endif
      bin_index = 0;
    }

    // Track which bins are used (for selective clearing next call)
    // Only add if this bin's total_size is 0 (first person in this bin)
    if (bins_buffer_[bin_index].total_size == 0) {
      used_bins_.push_back(bin_index);
    }

    // Track which bins are used (for selective clearing next call)
    // Only add if this bin's total_size is 0 (first person in this bin)
    if (bins_buffer_[bin_index].total_size == 0) {
      used_bins_.push_back(bin_index);
    }

    // Track total bin size (for frequency-dependent transmission denominator)
    if (!person || !person->is_dead) {
      bins_buffer_[bin_index].total_size++;
    }

    double susceptibility = 0.0;
    const VisitorInfo* visitor = nullptr;

    if (!person && visitor_data != nullptr) {
      auto visitor_it = visitor_data->find(pid);
      if (visitor_it != visitor_data->end()) {
        visitor = &visitor_it->second;

        if (visitor->is_infectious) {
          // Use pre-computed integrated infectiousness from the sending rank.
          // These values were computed using the identical code path as local
          // people (Infection::getIntegratedInfectiousness), guaranteeing
          // bit-identical FP results regardless of local vs visitor status.
          im_scratch_buffer_.assign(num_modes, 0.0);
          double total_im_visitor = 0.0;
          for (int m = 0; m < num_modes; ++m) {
            im_scratch_buffer_[m] = (m < VisitorInfo::MAX_MODES)
                                        ? visitor->integrated_infectiousness[m]
                                        : 0.0;
            total_im_visitor += im_scratch_buffer_[m];
          }
          if (total_im_visitor > 0.0) {
            bins_buffer_[bin_index].infectious_ids.push_back(pid);
            for (int m = 0; m < num_modes; ++m) {
              bins_buffer_[bin_index].infectiousness_by_mode[m].push_back(
                  im_scratch_buffer_[m]);
              bins_buffer_[bin_index].total_infectiousness_by_mode[m] +=
                  im_scratch_buffer_[m];
            }
          }
          // Fomite deposition for visitors (per temporal sub-bin)
          for (int local_fm = 0; local_fm < num_fomite_modes; ++local_fm) {
            int n_sub = n_sub_per_mode[local_fm];
            double dt_sub_stage = delta_hours / n_sub / 24.0;
            for (int k = 0; k < n_sub; ++k) {
              double t_stage_k_s = visitor->time_in_stage + k * dt_sub_stage;
              double t_stage_k_e =
                  visitor->time_in_stage + (k + 1) * dt_sub_stage;
              double dep_k = disease_->integrateFomiteDeposition(
                  fomite_modes[local_fm].mode_index, visitor->symptom_id,
                  t_stage_k_s, t_stage_k_e);
              if (dep_k > 0.0)
                bins_buffer_[bin_index]
                    .total_fomite_deposition_sub[local_fm][k] += dep_k;
            }
          }
        } else if (!visitor->is_infected && visitor->immunity_level < 1.0) {
          susceptibility = 1.0 - visitor->immunity_level;
          bins_buffer_[bin_index].susceptible.push_back(
              {pid, susceptibility, visitor, member.encounter_type_id});
        }
      }
    } else if (person && !person->is_dead) {
      const double t1 = current_time + delta_hours / 24.0;
      // Classify as infectious or susceptible
      if (person->infection && person->infection->isInfectious(current_time)) {
        // Compute per-mode integrated infectiousness (hour-units: 24*∫I dt)
        im_scratch_buffer_.resize(num_modes);
        double infectiousness_total = 0.0;
        for (int m = 0; m < num_modes; ++m) {
          im_scratch_buffer_[m] =
              person->infection->getIntegratedInfectiousness(m, current_time,
                                                             t1);
          infectiousness_total += im_scratch_buffer_[m];
        }
        if (infectiousness_total > 0.0) {
          bins_buffer_[bin_index].infectious_ids.push_back(pid);
          for (int m = 0; m < num_modes; ++m) {
            bins_buffer_[bin_index].infectiousness_by_mode[m].push_back(
                im_scratch_buffer_[m]);
            bins_buffer_[bin_index].total_infectiousness_by_mode[m] +=
                im_scratch_buffer_[m];
          }
        }
      } else if (!person->infection) {
        susceptibility =
            person->getSusceptibility(current_time, disease_->getName());
        if (susceptibility > 0.0) {
          bins_buffer_[bin_index].susceptible.push_back(
              {pid, susceptibility, visitor, member.encounter_type_id});
        }
      }
      // Fomite deposition: independent of direct-contact infectiousness (per
      // temporal sub-bin)
      if (person->infection && num_fomite_modes > 0) {
        for (int local_fm = 0; local_fm < num_fomite_modes; ++local_fm) {
          int n_sub = n_sub_per_mode[local_fm];
          double dt_sub = (t1 - current_time) / n_sub;
          for (int k = 0; k < n_sub; ++k) {
            double t_sub_s = current_time + k * dt_sub;
            double t_sub_e = current_time + (k + 1) * dt_sub;
            double dep_k = person->infection->getIntegratedFomiteDeposition(
                fomite_modes[local_fm].mode_index, t_sub_s, t_sub_e);
            if (dep_k > 0.0)
              bins_buffer_[bin_index]
                  .total_fomite_deposition_sub[local_fm][k] += dep_k;
          }
        }
      }
    }
  }

  // === STEP 1b: Sort infectious lists by person_id for MPI reproducibility ===
  // Ensures discrete_distribution index k always maps to the same person
  // regardless of which rank processes this venue or in what order.
  for (int b = 0; b < num_bins_needed; ++b) {
    auto& group = bins_buffer_[b];
    size_t n = group.infectious_ids.size();
    if (n <= 1) continue;

    // Build index permutation sorted by person_id
    std::vector<size_t> perm(n);
    std::iota(perm.begin(), perm.end(), 0);
    std::sort(perm.begin(), perm.end(), [&](size_t a, size_t b_idx) {
      return group.infectious_ids[a] < group.infectious_ids[b_idx];
    });

    // Apply permutation to infectious_ids and each mode's infectiousness
    std::vector<PersonId> sorted_ids(n);
    for (size_t i = 0; i < n; ++i)
      sorted_ids[i] = group.infectious_ids[perm[i]];
    group.infectious_ids = std::move(sorted_ids);

    for (int m = 0; m < num_modes; ++m) {
      auto& inf_m = group.infectiousness_by_mode[m];
      if (inf_m.size() != n) continue;
      std::vector<double> sorted_inf(n);
      for (size_t i = 0; i < n; ++i) sorted_inf[i] = inf_m[perm[i]];
      inf_m = std::move(sorted_inf);
    }
  }

  // === STEP 1c: Sort susceptibles by person_id for deterministic order ===
  for (int b = 0; b < num_bins_needed; ++b) {
    auto& susc = bins_buffer_[b].susceptible;
    if (susc.size() > 1) {
      std::sort(susc.begin(), susc.end(),
                [](const SusceptibleMember& a, const SusceptibleMember& b) {
                  return a.id < b.id;
                });
    }
  }

  // === STEP 2: Pre-calculate per-mode cumulative weight arrays for
  // infectious bins. cumulative_by_mode[m] is sampled with
  // sampleFromCumulative in STEP 3b. Replaces std::discrete_distribution
  // construction — buildCumulative is the same O(k) work but avoids the
  // distribution's internal vector allocation/destruction.
  for (int b = 0; b < num_bins_needed; ++b) {
    auto& group = bins_buffer_[b];
    if (group.infectious_ids.empty()) continue;

    for (int m = 0; m < num_modes; ++m) {
      const auto& w = group.infectiousness_by_mode[m];
      double total = buildCumulative(w, group.cumulative_by_mode[m]);
      if (!(total > 0.0)) {
        // No positive weight — clear so callers know to skip sampling.
        group.cumulative_by_mode[m].clear();
      }
    }
  }

  // === STEP 2b: Handle Fomite Deposition and Compute Lambda ===
  std::vector<double> lambda_fomite_by_mode(num_fomite_modes, 0.0);
  if (num_fomite_modes > 0 && venue) {
    for (int local_fm = 0; local_fm < num_fomite_modes; ++local_fm) {
      const FomiteConfig& fcfg = *fomite_modes[local_fm].cfg;
      auto& history = venue->fomite_history;
      if (local_fm >= (int)history.size()) continue;

      // Sum deposition per sub-bin and append one history entry per sub-bin
      int n_sub = n_sub_per_mode[local_fm];
      double dt_sub = delta_hours / n_sub / 24.0;
      for (int k = 0; k < n_sub; ++k) {
        double amount_k = 0.0;
        for (int b = 0; b < num_bins_needed; ++b)
          amount_k += bins_buffer_[b].total_fomite_deposition_sub[local_fm][k];
        if (amount_k > 0.0) {
          double tau_k = current_time + (k + 0.5) * dt_sub;
          history[local_fm].push_back({tau_k, amount_k});
        }
      }

      // lambda = sum_k amount_k * ∫_{age_k}^{age_k+Δ} Q(a) da
      if (fcfg.infectiousness_curve) {
        const double delta_days = delta_hours / 24.0;
        for (const auto& event : history[local_fm]) {
          double age = current_time - event.time;
          lambda_fomite_by_mode[local_fm] +=
              event.amount *
              fcfg.infectiousness_curve->integrate(age, age + delta_days) /
              24.0;
        }
      }
    }
  }

  // Early exit if no transmission possible
  bool has_infectious = false;
  bool has_susceptible = false;
  for (int b = 0; b < num_bins_needed; ++b) {
    if (!bins_buffer_[b].infectious_ids.empty()) has_infectious = true;
    if (!bins_buffer_[b].susceptible.empty()) has_susceptible = true;
  }
  double total_lambda_fomite = 0.0;
  for (double lf : lambda_fomite_by_mode) total_lambda_fomite += lf;

  // Check whether any compartmental uptake FOI could exist for this venue.
  bool has_comp_uptake_potential =
      !comp_uptake_modes.empty() && comp_model != nullptr &&
      comp_model->venueToLocalNodeIndex(static_cast<int>(actual_venue_id)) >= 0;

  if (!has_susceptible || (!has_infectious && total_lambda_fomite <= 0.0 &&
                           !has_comp_uptake_potential)) {
    for (int b : used_bins_) bins_buffer_[b].clearAfterUse(num_modes);
    used_bins_.clear();
    return 0;
  }

  // Sibling-mixing setup. If this venue has a parent and that parent has
  // an aggregate from buildParentAggregates, we add a per-mode "sibling"
  // source representing infectious people in OTHER children of the same
  // parent (e.g. other classrooms of the same school). The parent's own
  // contact matrix (e.g. `school` entry in contact_matrices.yaml) governs
  // contacts/beta for this term.
  const ParentAggregate* parent_agg = nullptr;
  const ContactMatrix* parent_flat_matrix = nullptr;
  if (venue && venue->parent_id >= 0 && !is_virtual_encounter) {
    auto pit = parent_aggregates_.find(venue->parent_id);
    if (pit != parent_aggregates_.end()) {
      parent_flat_matrix =
          contact_matrices_.getMatrix(pit->second.parent_venue_type_id);
      if (parent_flat_matrix) {
        // V1 supports single-bin parent matrices only. The currently shipped
        // YAML (school/company/university) all use `bins: [all]`. A multi-
        // bin parent matrix would require per-susceptible parent_bin lookup
        // in the hot path — defer to V2. Refuse loudly per the no-silent-
        // fallbacks rule rather than silently mis-applying contacts[0][0].
        if (parent_flat_matrix->bins.size() > 1) {
          throw std::runtime_error(
              "parent_mixing: multi-bin parent contact matrices are not "
              "supported yet; parent venue type id=" +
              std::to_string(pit->second.parent_venue_type_id) + " has " +
              std::to_string(parent_flat_matrix->bins.size()) + " bins");
        }
        parent_agg = &pit->second;
      }
    }
  }

  // === STEP 3: Process transmissions (Mixing Model) — mode-aware ===
  for (int susc_bin = 0; susc_bin < num_bins_needed; ++susc_bin) {
    const auto& susc_group = bins_buffer_[susc_bin];
    if (susc_group.susceptible.empty()) continue;

    // 3a. Pre-calculate total Force of Infection across all modes and bins
    // Reuse member buffers
    sources_buffer_.clear();
    source_weights_buffer_.clear();
    double total_lambda_eff = 0.0;

    for (int m = 0; m < num_modes; ++m) {
      // Get mode-specific contact matrix. Virtual encounters are keyed by
      // encounter_type_id; physical venues by venue_type_id. The split
      // matters: these are disjoint integer spaces and aliasing them pulls
      // the wrong matrix.
      const ContactMatrix* mode_matrix =
          is_virtual_encounter
              ? contact_matrices_.getVirtualMatrix(encounter_type_id, m)
              : contact_matrices_.getMatrix(venue_type_id, m);
      double mode_susc_mult =
          (m < (int)trans_params.modes.size())
              ? trans_params.modes[m].susceptibility_multiplier
              : 1.0;

      for (int inf_bin = 0; inf_bin < num_bins_needed; ++inf_bin) {
        const auto& inf_group = bins_buffer_[inf_bin];
        if (inf_group.total_infectiousness_by_mode[m] <= 0.0) continue;

        // Get contacts for this (susc_bin, inf_bin) pair
        double contacts = contact_matrices_.default_contacts;

        if (mode_matrix && susc_bin < (int)mode_matrix->contacts.size() &&
            inf_bin < (int)mode_matrix->contacts[susc_bin].size()) {
          contacts = mode_matrix->contacts[susc_bin][inf_bin];
        } else if (matrix && susc_bin < (int)matrix->contacts.size() &&
                   inf_bin < (int)matrix->contacts[susc_bin].size()) {
          contacts = matrix->getContacts(susc_bin, inf_bin);
        }

        if (contacts <= 0.0) continue;

        int bin_size = bins_buffer_[inf_bin].total_size;
        if (susc_bin == inf_bin) bin_size = std::max(1, bin_size - 1);

        // Force of infection: omega = C / N_bin
        // (delta_hours is already absorbed into the integrated infectiousness
        // values stored in total_infectiousness_by_mode)
        double omega = contacts / bin_size;
        double contrib = omega * inf_group.total_infectiousness_by_mode[m];

        double weighted = contrib * mode_susc_mult;

        if (weighted > 0.0) {
          total_lambda_eff += weighted;
          sources_buffer_.push_back({m, inf_bin});
          source_weights_buffer_.push_back(weighted);
        }
      }
    }

    // 3a.bis Sibling-mixing contributions (cross-child-venue interaction).
    // Adds one source per mode summarising infectiousness from all OTHER
    // children of the same parent. Mathematically:
    //
    //   lambda_sibling(m) = (contacts_parent[0][0] /
    //                       max(1, N_parent_bin0 - N_child_own_bin0)) *
    //                       (I_parent_bin0_m - I_child_own_bin0_m) *
    //                       susc_mult(m)
    //
    // The parent matrix's contacts already absorb β at load time
    // (parseContactMatrix multiplies them).
    if (parent_agg) {
      const int pbin = 0;  // single-bin parent assumption (enforced above)
      auto cs_it = parent_agg->child_size_by_bin.find(actual_venue_id);
      auto ci_it = parent_agg->child_inf_by_bin_mode.find(actual_venue_id);
      int parent_size = (pbin < (int)parent_agg->size_by_bin.size())
                            ? parent_agg->size_by_bin[pbin]
                            : 0;
      int own_size = (cs_it != parent_agg->child_size_by_bin.end() &&
                      pbin < (int)cs_it->second.size())
                         ? cs_it->second[pbin]
                         : 0;
      int sibling_size = std::max(1, parent_size - own_size);

      for (int m = 0; m < num_modes; ++m) {
        const ContactMatrix* pmm =
            contact_matrices_.getMatrix(parent_agg->parent_venue_type_id, m);
        if (!pmm) pmm = parent_flat_matrix;
        if (!pmm || pmm->contacts.empty() || pmm->contacts[0].empty()) continue;
        double contacts = pmm->contacts[0][0];
        if (contacts <= 0.0) continue;

        double parent_inf =
            (pbin < (int)parent_agg->total_inf_by_bin_mode.size())
                ? parent_agg->total_inf_by_bin_mode[pbin][m]
                : 0.0;
        double own_inf = (ci_it != parent_agg->child_inf_by_bin_mode.end() &&
                          pbin < (int)ci_it->second.size())
                             ? ci_it->second[pbin][m]
                             : 0.0;
        double sibling_inf = parent_inf - own_inf;
        if (sibling_inf <= 0.0) continue;

        double mode_susc_mult =
            (m < (int)trans_params.modes.size())
                ? trans_params.modes[m].susceptibility_multiplier
                : 1.0;
        double omega = contacts / sibling_size;
        double weighted = omega * sibling_inf * mode_susc_mult;
        if (weighted > 0.0) {
          total_lambda_eff += weighted;
          SourceEntry se;
          se.mode = m;
          se.inf_bin = SIBLING_INF_BIN_SENTINEL;
          se.sibling_parent_inf_bin = pbin;
          sources_buffer_.push_back(se);
          source_weights_buffer_.push_back(weighted);

          if (debug_parent_mixing_ && dbg_sample_susc_prints_ < 20) {
            std::cerr << "[PMIX] sibling_FOI venue=" << actual_venue_id
                      << " parent=" << venue->parent_id << " parent_type="
                      << (int)parent_agg->parent_venue_type_id
                      << " susc_bin=" << susc_bin << " mode=" << m
                      << " contacts=" << contacts
                      << " parent_inf=" << parent_inf << " own_inf=" << own_inf
                      << " sibling_inf=" << sibling_inf
                      << " parent_size=" << parent_size
                      << " own_size=" << own_size
                      << " sibling_size=" << sibling_size << " omega=" << omega
                      << " weighted=" << weighted << std::endl;
            dbg_sample_susc_prints_++;
          }
        }
      }
    }

    // Add per-fomite-mode contributions as sentinel sources
    // (FOMITE_INFECTOR_BIN)
    for (int local_fm = 0; local_fm < num_fomite_modes; ++local_fm) {
      if (lambda_fomite_by_mode[local_fm] > 0.0) {
        int fomite_mode_idx = fomite_modes[local_fm].mode_index;
        double mode_susc_mult =
            (fomite_mode_idx < (int)trans_params.modes.size())
                ? trans_params.modes[fomite_mode_idx].susceptibility_multiplier
                : 1.0;
        double weighted = lambda_fomite_by_mode[local_fm] * mode_susc_mult;
        total_lambda_eff += weighted;
        sources_buffer_.push_back(SourceEntry{fomite_mode_idx, -1});
        source_weights_buffer_.push_back(weighted);
      }
    }

    // Add compartmental uptake FOI for this venue node.
    // Raw plugin output scaled by per-venue-type foi_scale before applying
    // susc_mult.
    if (comp_model && !comp_uptake_modes.empty()) {
      const float* buf = comp_model->readCouplingOutputs();
      int node_idx =
          comp_model->venueToLocalNodeIndex(static_cast<int>(actual_venue_id));
      if (buf && node_idx >= 0) {
        float foi_scale =
            comp_model->getOutputFOIScale(static_cast<int>(venue_type_id), 0);
        float node_output = buf[node_idx] * foi_scale;
        for (int mode_idx : comp_uptake_modes) {
          double mode_susc_mult =
              (mode_idx < (int)trans_params.modes.size())
                  ? trans_params.modes[mode_idx].susceptibility_multiplier
                  : 1.0;
          double weighted = node_output * mode_susc_mult;
          if (weighted > 0.0) {
            total_lambda_eff += weighted;
            sources_buffer_.push_back(SourceEntry{mode_idx, -2});
            source_weights_buffer_.push_back(weighted);
          }
        }
      }
    }

    double total_risk = total_lambda_eff;

    // Regional Risk: Apply transmission factor if enabled
    if (simulation_config_.regional_risk.enabled && venue) {
      total_risk *= venue->transmission_factor;
    }

#ifdef DEBUG_TRANSMISSION
    // Log actual force-of-infection when infectious people are present
    if (has_infectious) {
      static int foi_log_count = 0;
      if (foi_log_count < 50) {
        // Gather per-mode infectiousness totals for this bin combination
        std::cerr << "[DEBUG_TRANSMISSION] FOI: venue_type=" << venue_type
                  << " susc_bin=" << susc_bin
                  << " n_susceptible=" << susc_group.susceptible.size()
                  << " total_lambda_eff=" << total_lambda_eff
                  << " total_risk=" << total_risk << " delta_h=" << delta_hours;
        for (int m = 0; m < num_modes; ++m) {
          double mode_inf_total = 0.0;
          for (int b = 0; b < num_bins_needed; ++b)
            mode_inf_total += bins_buffer_[b].total_infectiousness_by_mode[m];
          if (mode_inf_total > 0.0) {
            std::cerr << " mode[" << m << "](" << disease_->getModeName(m)
                      << ")_inf=" << mode_inf_total;
          }
        }
        // Show contact rates used
        for (size_t si = 0; si < source_weights_buffer_.size(); ++si) {
          std::cerr << " src[m=" << sources_buffer_[si].mode
                    << ",b=" << sources_buffer_[si].inf_bin
                    << "]=" << source_weights_buffer_[si];
        }
        // Show example susceptibility
        if (!susc_group.susceptible.empty()) {
          std::cerr << " example_susc="
                    << susc_group.susceptible[0].susceptibility;
          double p = 1.0 - std::exp(-total_risk *
                                    susc_group.susceptible[0].susceptibility);
          std::cerr << " example_prob=" << p;
        }
        std::cerr << std::endl;
        foi_log_count++;
        if (foi_log_count == 50)
          std::cerr << "[DEBUG_TRANSMISSION] (suppressing further FOI logs)"
                    << std::endl;
      }
    }
#endif

    if (total_risk <= 0.0) continue;

    // Build cumulative source weights once per susc_bin; each susceptible
    // samples from it via sampleFromCumulative below. Avoids the per-bin
    // std::discrete_distribution construction that dominated the 60M run.
    bool have_source_dist = source_weights_buffer_.size() > 1 &&
                            buildCumulative(source_weights_buffer_,
                                            source_cumulative_buffer_) > 0.0;

    // 3b. Process each susceptible in this bin
    uint64_t time_bits = static_cast<uint64_t>(current_time * 1000);
    for (const auto& susc_mem : susc_group.susceptible) {
      PersonId susceptible_id = susc_mem.id;
      double susceptibility = susc_mem.susceptibility;
      double prob = 1.0 - std::exp(-total_risk * susceptibility);

      // Per-susceptible deterministic RNG for MPI reproducibility.
      // For virtual venues (id <= -1000), extract the host's person_id
      // so the RNG seed is deterministic regardless of which rank hosts
      // the encounter.
      uint64_t venue_key = static_cast<uint64_t>(actual_venue_id);
      if (actual_venue_id <= -1000) {
        // Virtual venue IDs encode the host's person_id: id = -1000 - pid.
        // Extract the person_id for deterministic RNG seeding.
        venue_key = static_cast<uint64_t>(
            -static_cast<int64_t>(actual_venue_id) - 1000);
      }
      SplitMix64 susc_rng(
          mix_seed(base_seed_, susceptible_id, venue_key, time_bits));

      double rng_roll = uniform_dist_(susc_rng);
      bool got_infected = prob > 1e-12 && rng_roll < prob;

      if (got_infected) {
        // Joint (mode, infector) sample
        int src_idx =
            have_source_dist
                ? sampleFromCumulative(source_cumulative_buffer_, susc_rng)
                : 0;
        if (src_idx < 0) src_idx = 0;
        int sampled_mode = sources_buffer_[src_idx].mode;
        int sampled_inf_bin = sources_buffer_[src_idx].inf_bin;

        PersonId infector_id = -1;
        uint8_t transmission_mode_index = 0;
        uint16_t infector_symptom_id = 0;
        InfectionSource infection_source = InfectionSource::Person;

        if (sampled_inf_bin == -2) {
          // Compartmental uptake source
          infection_source = InfectionSource::Compartmental;
          transmission_mode_index = static_cast<uint8_t>(sampled_mode);
        } else if (sampled_inf_bin == -1) {
          // Fomite source
          infection_source = InfectionSource::Fomite;
          transmission_mode_index = static_cast<uint8_t>(sampled_mode);
        } else if (sampled_inf_bin == SIBLING_INF_BIN_SENTINEL) {
          // Sibling-venue source: still a Person infector, but in a DIFFERENT
          // child venue under the same parent. Two-stage sample:
          //   1) build cumulative over the parent's infector pool for this
          //      mode, EXCLUDING entries from the susceptible's own venue;
          //   2) sample one entry with the same susc_rng — the same RNG
          //      already used for source-selection, so the byte stream
          //      depends only on (susceptible_id, venue_id, time).
          infection_source = InfectionSource::Person;
          transmission_mode_index = static_cast<uint8_t>(sampled_mode);

          if (parent_agg) {
            int pbin = sources_buffer_[src_idx].sibling_parent_inf_bin;
            if (pbin >= 0 && pbin < (int)parent_agg->infectors_by_bin.size()) {
              const auto& pool = parent_agg->infectors_by_bin[pbin];
              sibling_cum_buffer_.clear();
              sibling_pool_indices_buffer_.clear();
              double acc = 0.0;
              for (size_t pe_idx = 0; pe_idx < pool.size(); ++pe_idx) {
                const auto& pe = pool[pe_idx];
                if (pe.child_venue_id == actual_venue_id) continue;
                double w = (sampled_mode < (int)pe.inf_by_mode.size())
                               ? pe.inf_by_mode[sampled_mode]
                               : 0.0;
                if (w <= 0.0) continue;
                acc += w;
                sibling_cum_buffer_.push_back(acc);
                sibling_pool_indices_buffer_.push_back(pe_idx);
              }
              if (acc > 0.0 && !sibling_cum_buffer_.empty()) {
                int idx = sampleFromCumulative(sibling_cum_buffer_, susc_rng);
                if (idx >= 0 &&
                    idx < (int)sibling_pool_indices_buffer_.size()) {
                  infector_id =
                      pool[sibling_pool_indices_buffer_[idx]].person_id;
                }
              }
            }
          }

          if (debug_parent_mixing_ && dbg_sample_infection_prints_ < 20) {
            std::cerr << "[PMIX] sibling_infection susc=" << susceptible_id
                      << " venue=" << actual_venue_id
                      << " parent=" << (venue ? venue->parent_id : -1)
                      << " mode=" << sampled_mode << " infector=" << infector_id
                      << " infector_pool_size="
                      << sibling_pool_indices_buffer_.size()
                      << " susc_bin=" << susc_bin << std::endl;
            dbg_sample_infection_prints_++;
          }
          dbg_sibling_infections_++;
        } else {
          transmission_mode_index = static_cast<uint8_t>(sampled_mode);
          auto& inf_group = bins_buffer_[sampled_inf_bin];
          const auto& cum = inf_group.cumulative_by_mode[sampled_mode];
          if (!cum.empty() && !inf_group.infectious_ids.empty()) {
            int person_idx = sampleFromCumulative(cum, susc_rng);
            if (person_idx >= 0 &&
                person_idx < (int)inf_group.infectious_ids.size()) {
              infector_id = inf_group.infectious_ids[person_idx];
            }
          } else if (!inf_group.infectious_ids.empty()) {
            infector_id = inf_group.infectious_ids[0];
          }
        }

        // Determine infector's current symptom for pathway routing.
        if (infector_id >= 0) {
          Person* infector = world_.getPerson(infector_id);
          if (infector && infector->infection) {
            infector_symptom_id =
                infector->infection->getTrajectory().getCurrentSymptomId(
                    current_time);
          } else if (!infector && visitor_data) {
            // Infector is a visitor from another rank
            auto vit = visitor_data->find(infector_id);
            if (vit != visitor_data->end()) {
              infector_symptom_id = vit->second.symptom_id;
            }
          }
        }

        // Record and log infection
        const VisitorInfo* visitor = susc_mem.visitor;
        bool is_visitor = (visitor != nullptr);

        // Check if infector is a visitor (cross-rank infector)
        bool infector_is_visitor = false;
        if (visitor_data && infector_id >= 0) {
          infector_is_visitor = (visitor_data->count(infector_id) > 0);
        }

        if (is_visitor && pending_infections != nullptr) {
          // Visitor infection - queue for home rank.
          // The home rank logs the InfectionEvent after applying the pending
          // infection, so /lookups/people (built from world.people on the home
          // rank, filtered by getInfectedPersonIds()) includes the infectee.
          PendingInfection pending;
          pending.person_id = susceptible_id;
          pending.infector_id = infector_id;
          pending.infection_time = current_time;
          pending.venue_type_id = venue_type_id;
          pending.encounter_type_id = susc_mem.encounter_type_id;
          pending.venue_id = actual_venue_id;
          pending.infector_symptom_id = infector_symptom_id;
          pending.transmission_mode_index = transmission_mode_index;

          // Get home_array_index if available
          if (visitor) pending.home_array_index = visitor->home_array_index;

          pending_infections->push_back(pending);
        } else {
          // Local person infection - create immediately
          Person* susc_person = world_.getPerson(susceptible_id);
          if (susc_person && !susc_person->infection && disease_ != nullptr) {
            float severity_factor = 1.0f;
            auto* gu = world_.getGeoUnit(susc_person->geo_unit_id);
            if (gu) severity_factor = gu->severity_factor;

            std::string venue_type_name = "";
            if (venue_type_id < world_.venue_type_names.size()) {
              venue_type_name = world_.venue_type_names[venue_type_id];
            }

            uint64_t infection_seed =
                mix_seed(base_seed_, susceptible_id,
                         static_cast<uint64_t>(current_time * 1000), venue_key);
            susc_person->infection = std::make_unique<Infection>(
                disease_, current_time, susc_person,
                static_cast<unsigned int>(infection_seed), &world_,
                venue_type_name, actual_venue_id, severity_factor,
                infector_symptom_id, "", "", transmission_mode_index);

            if (event_logger_ != nullptr) {
              event_logger_->logInfection(
                  susceptible_id, infector_id, actual_venue_id, current_time,
                  susc_mem.encounter_type_id, infector_symptom_id,
                  transmission_mode_index, infection_source);
            }

            if (active_infections != nullptr) {
              active_infections->insert(susceptible_id);
            }
          }
        }

        new_infections++;
      }
    }
  }

  // Clear person-proportional fields while bins are still cache-hot.
  for (int b : used_bins_) {
    bins_buffer_[b].clearAfterUse(num_modes);
  }
  used_bins_.clear();

  return new_infections;
}

// =============================================================================
// processPartialPresenceVenue — sub-interval FOI for partial-presence venues.
// =============================================================================
//
// Architecture: each partial-presence venue (e.g. a train line) is partitioned
// into runtime bins ("carriages") by RuntimeBinAllocator. Within each carriage,
// FOI is computed over sub-intervals delimited by riders' effective
// (eff_board, eff_alight) windows so that two riders share air only on the
// overlap of their journeys.
//
// Effective presence windows come from the membership_metadata side-table
// (per-leg t_board_min / t_alight_min) routed through computePresenceWindows,
// which applies the proportional policy for long-distance commuters whose
// raw journey doesn't fit the slot.
//
// Single Bernoulli draw per susceptible at slot end; sources are accumulated
// across all (carriage × sub-interval) contributions and sampled once.
InteractionManager::PartialPresenceLambdaResult
InteractionManager::computePartialPresenceLambda(
    const std::vector<InteractionMember>& members, Venue* venue,
    VenueId actual_venue_id, double current_time, double delta_hours,
    const std::unordered_map<PersonId, VisitorInfo>* visitor_data,
    uint8_t encounter_type_id) {
  PartialPresenceLambdaResult result;

  // v1 preconditions. Throw rather than silently fall through.
  if (actual_venue_id < 0)
    throw std::runtime_error(
        "computePartialPresenceLambda: virtual encounter venues not supported");
  if (!venue)
    throw std::runtime_error("computePartialPresenceLambda: null venue");
  if (encounter_type_id != 255)
    throw std::runtime_error(
        "computePartialPresenceLambda: coordinated-encounter venues not "
        "supported on partial-presence types in v1");
  if (venue->parent_id >= 0)
    throw std::runtime_error(
        "computePartialPresenceLambda: parent-venue mixing not supported on "
        "partial-presence types in v1");
  if (!runtime_bin_allocator_)
    throw std::runtime_error(
        "computePartialPresenceLambda: runtime_bin_allocator_ is null (gate "
        "should have prevented this call)");

  const uint8_t venue_type_id = venue->type_id;
  const ContactMatrix* matrix = contact_matrices_.getMatrix(venue_type_id);
  if (!matrix)
    throw std::runtime_error(
        "computePartialPresenceLambda: no contact matrix for venue_type_id=" +
        std::to_string(static_cast<int>(venue_type_id)));
  const int num_bins_needed =
      std::max(1, static_cast<int>(matrix->bins.size()));

  int num_modes = disease_->numModes();
  if (num_modes == 0) num_modes = 1;
  const auto& trans_params = disease_->getTransmissionParams();

  const float slot_duration_min = static_cast<float>(delta_hours * 60.0);
  if (!(slot_duration_min > 0.0f)) return result;

  // Presence windows live on the allocator. It computes them on each
  // rider's home rank (proportional vs clamped policy applied to the full
  // leg list) and broadcasts globally, so a cross-rank visitor's window is
  // identical to what the home rank would have computed locally.
  const uint16_t num_bins = runtime_bin_allocator_->getNumBins(actual_venue_id);
  if (num_bins == 0) return result;

  // ---------------------------------------------------------------------------
  // Step 1: resolve each member's (carriage, matrix_bin, eff_board, eff_alight)
  // and group by carriage.
  // ---------------------------------------------------------------------------
  struct CarriageMember {
    PersonId pid;
    size_t array_index;
    SubsetIndex subset_index;
    uint8_t enc_type_id;
    Person* person;  // null for visitors
    const VisitorInfo* visitor;
    float eff_board;
    float eff_alight;
    int matrix_bin;
  };

  std::vector<std::vector<CarriageMember>> carriages(num_bins);

  for (const auto& m : members) {
    const uint16_t carriage =
        runtime_bin_allocator_->getBinIndex(actual_venue_id, m.id);
    if (carriage == RuntimeBinAllocator::kNoBin || carriage >= num_bins)
      continue;

    Person* person = nullptr;
    if (m.array_index < world_.people.size()) {
      person = &world_.people[m.array_index];
      if (person->id != m.id) person = world_.getPerson(m.id);
    } else {
      person = world_.getPerson(m.id);
    }

    const VisitorInfo* visitor = nullptr;
    if (!person && visitor_data) {
      auto it = visitor_data->find(m.id);
      if (it != visitor_data->end()) visitor = &it->second;
    }
    if (!person && !visitor) continue;

    // Window from the allocator's global broadcast — identical on every
    // rank for the same (venue, person) pair.
    const EffectiveWindow win =
        runtime_bin_allocator_->getPresenceWindow(actual_venue_id, m.id);

    int matrix_bin =
        computeBinIndexForMatrix(person, venue, m.subset_index,
                                 m.encounter_type_id, matrix, num_bins_needed);
    if (matrix_bin < 0 || matrix_bin >= num_bins_needed) matrix_bin = 0;

    carriages[carriage].push_back(CarriageMember{
        m.id, m.array_index, m.subset_index, m.encounter_type_id, person,
        visitor, win.eff_board, win.eff_alight, matrix_bin});
  }

  // Deterministic per-carriage order (FP-stable accumulation across ranks).
  for (auto& car : carriages) {
    std::sort(car.begin(), car.end(),
              [](const CarriageMember& a, const CarriageMember& b) {
                return a.pid < b.pid;
              });
  }

  // ---------------------------------------------------------------------------
  // Step 2: walk (carriage × sub-interval), accumulate per-susceptible λ and
  // per-source attribution weights.
  // ---------------------------------------------------------------------------
  using AccumSource = PartialPresenceAccumSource;
  auto& susc_lambda = result.susc_lambda;
  auto& susc_sources = result.susc_sources;

  // Per-bin scratch reused across sub-intervals (cleared per sub-interval).
  struct SubBin {
    std::vector<double> total_inf_by_mode;  // size num_modes
    std::vector<PersonId> infectious_ids;
    // Per (mode) → flat list aligned with infectious_ids for sampling.
    std::vector<std::vector<double>> inf_per_person_by_mode;  // [mode][i]
    int total_size = 0;
    void reset(int num_modes_) {
      total_inf_by_mode.assign(num_modes_, 0.0);
      infectious_ids.clear();
      inf_per_person_by_mode.assign(num_modes_, {});
      total_size = 0;
    }
  };
  std::vector<SubBin> sub_bins(num_bins_needed);

  for (uint16_t c = 0; c < num_bins; ++c) {
    const auto& car = carriages[c];
    if (car.empty()) continue;

    // Collect unique event times in this carriage.
    std::vector<float> events;
    events.reserve(2 * car.size() + 2);
    events.push_back(0.0f);
    events.push_back(slot_duration_min);
    for (const auto& m : car) {
      if (m.eff_board > 0.0f && m.eff_board < slot_duration_min)
        events.push_back(m.eff_board);
      if (m.eff_alight > 0.0f && m.eff_alight < slot_duration_min)
        events.push_back(m.eff_alight);
    }
    std::sort(events.begin(), events.end());
    events.erase(
        std::unique(events.begin(), events.end(),
                    [](float a, float b) { return std::abs(a - b) < 1e-5f; }),
        events.end());
    if (events.size() < 2) continue;

    for (size_t si = 0; si + 1 < events.size(); ++si) {
      const float t0 = events[si];
      const float t1 = events[si + 1];
      const float sub_dur = t1 - t0;
      if (!(sub_dur > 0.0f)) continue;
      const double scale = static_cast<double>(sub_dur) / slot_duration_min;

      for (auto& sb : sub_bins) sb.reset(num_modes);

      // Track per-bin susceptibles in this sub-interval (rebuilt each pass).
      std::vector<std::vector<const CarriageMember*>> susc_by_bin(
          num_bins_needed);

      for (const auto& m : car) {
        // Present iff [eff_board, eff_alight) covers [t0, t1).
        if (!(m.eff_board <= t0 + 1e-5f && m.eff_alight + 1e-5f >= t1))
          continue;

        const int bin = m.matrix_bin;
        const bool dead = (m.person && m.person->is_dead);
        if (!dead) sub_bins[bin].total_size++;

        // Infectious?
        bool added_inf = false;
        if (m.visitor && m.visitor->is_infectious) {
          for (int mode = 0; mode < num_modes; ++mode) {
            double inf_full = (mode < VisitorInfo::MAX_MODES)
                                  ? m.visitor->integrated_infectiousness[mode]
                                  : 0.0;
            double inf_sub = inf_full * scale;
            if (inf_sub > 0.0) {
              if (!added_inf) {
                sub_bins[bin].infectious_ids.push_back(m.pid);
                added_inf = true;
              }
              sub_bins[bin].inf_per_person_by_mode[mode].push_back(inf_sub);
              sub_bins[bin].total_inf_by_mode[mode] += inf_sub;
            } else if (added_inf) {
              // Keep arrays aligned across modes.
              sub_bins[bin].inf_per_person_by_mode[mode].push_back(0.0);
            }
          }
        } else if (m.person && m.person->infection &&
                   m.person->infection->isInfectious(current_time)) {
          const double t_end_d = current_time + delta_hours / 24.0;
          for (int mode = 0; mode < num_modes; ++mode) {
            double inf_full = m.person->infection->getIntegratedInfectiousness(
                mode, current_time, t_end_d);
            double inf_sub = inf_full * scale;
            if (inf_sub > 0.0) {
              if (!added_inf) {
                sub_bins[bin].infectious_ids.push_back(m.pid);
                added_inf = true;
              }
              sub_bins[bin].inf_per_person_by_mode[mode].push_back(inf_sub);
              sub_bins[bin].total_inf_by_mode[mode] += inf_sub;
            } else if (added_inf) {
              sub_bins[bin].inf_per_person_by_mode[mode].push_back(0.0);
            }
          }
        } else if (m.person && !m.person->infection && !dead) {
          double susc =
              m.person->getSusceptibility(current_time, disease_->getName());
          if (susc > 0.0) susc_by_bin[bin].push_back(&m);
        } else if (m.visitor && !m.visitor->is_infected &&
                   m.visitor->immunity_level < 1.0) {
          susc_by_bin[bin].push_back(&m);
        }
      }

      // Per-susceptible bin: accumulate λ contributions over (mode, inf_bin).
      for (int susc_bin = 0; susc_bin < num_bins_needed; ++susc_bin) {
        if (susc_by_bin[susc_bin].empty()) continue;

        for (int mode = 0; mode < num_modes; ++mode) {
          const ContactMatrix* mode_matrix =
              contact_matrices_.getMatrix(venue_type_id, mode);
          double mode_susc_mult =
              (mode < static_cast<int>(trans_params.modes.size()))
                  ? trans_params.modes[mode].susceptibility_multiplier
                  : 1.0;

          for (int inf_bin = 0; inf_bin < num_bins_needed; ++inf_bin) {
            double total_inf = sub_bins[inf_bin].total_inf_by_mode[mode];
            if (!(total_inf > 0.0)) continue;

            double contacts = contact_matrices_.default_contacts;
            if (mode_matrix &&
                susc_bin < static_cast<int>(mode_matrix->contacts.size()) &&
                inf_bin <
                    static_cast<int>(mode_matrix->contacts[susc_bin].size())) {
              contacts = mode_matrix->contacts[susc_bin][inf_bin];
            } else if (susc_bin < static_cast<int>(matrix->contacts.size()) &&
                       inf_bin < static_cast<int>(
                                     matrix->contacts[susc_bin].size())) {
              contacts = matrix->getContacts(susc_bin, inf_bin);
            }
            if (!(contacts > 0.0)) continue;

            int bin_size = sub_bins[inf_bin].total_size;
            if (susc_bin == inf_bin) bin_size = std::max(1, bin_size - 1);
            double omega = contacts / bin_size;
            double contrib = omega * total_inf * mode_susc_mult;
            if (!(contrib > 0.0)) continue;

            for (const CarriageMember* sm : susc_by_bin[susc_bin]) {
              susc_lambda[sm->pid] += contrib;
              // Pick an infector for this contribution by weight-sampling
              // proportional to per-person infectiousness in this sub-interval.
              // We record one AccumSource per (susc, mode, inf_bin, sub) with
              // the FULL bin contribution; per-person sampling happens at
              // the single Bernoulli site below.
              const auto& ids = sub_bins[inf_bin].infectious_ids;
              const auto& per = sub_bins[inf_bin].inf_per_person_by_mode[mode];
              for (size_t pi = 0; pi < ids.size(); ++pi) {
                if (pi >= per.size() || !(per[pi] > 0.0)) continue;
                double w = omega * per[pi] * mode_susc_mult;
                if (!(w > 0.0)) continue;
                susc_sources[sm->pid].push_back(AccumSource{mode, ids[pi], w});
              }
            }
          }
        }
      }
    }
  }

  return result;
}

int InteractionManager::processPartialPresenceVenue(
    const std::vector<InteractionMember>& members, Venue* venue,
    VenueId actual_venue_id, double current_time, double delta_hours,
    std::unordered_set<PersonId>* active_infections,
    const std::unordered_set<PersonId>* /*visitor_ids*/,
    std::vector<PendingInfection>* pending_infections,
    const std::unordered_map<PersonId, VisitorInfo>* visitor_data,
    uint8_t encounter_type_id,
    const CompartmentalModelManager* /*comp_model*/) {
  PartialPresenceLambdaResult acc = computePartialPresenceLambda(
      members, venue, actual_venue_id, current_time, delta_hours, visitor_data,
      encounter_type_id);
  auto& susc_lambda = acc.susc_lambda;
  auto& susc_sources = acc.susc_sources;

  const uint8_t venue_type_id = venue->type_id;
  using AccumSource = PartialPresenceAccumSource;

  // ---------------------------------------------------------------------------
  // Step 3: per-susceptible Bernoulli draw + infector sampling.
  // ---------------------------------------------------------------------------
  // Iterate susceptibles in person_id order for deterministic per-call work.
  std::vector<PersonId> ordered_susc;
  ordered_susc.reserve(susc_lambda.size());
  for (auto& kv : susc_lambda) ordered_susc.push_back(kv.first);
  std::sort(ordered_susc.begin(), ordered_susc.end());

  int new_infections = 0;
  const uint64_t time_bits = static_cast<uint64_t>(current_time * 1000);
  const uint64_t venue_key = static_cast<uint64_t>(actual_venue_id);

  for (PersonId susc_id : ordered_susc) {
    double lambda = susc_lambda[susc_id];
    if (!(lambda > 0.0)) continue;

    Person* susc_person = world_.getPerson(susc_id);
    const VisitorInfo* visitor = nullptr;
    if (!susc_person && visitor_data) {
      auto it = visitor_data->find(susc_id);
      if (it != visitor_data->end()) visitor = &it->second;
    }

    double susceptibility = 0.0;
    if (susc_person) {
      susceptibility =
          susc_person->getSusceptibility(current_time, disease_->getName());
    } else if (visitor) {
      susceptibility = 1.0 - visitor->immunity_level;
    } else {
      continue;
    }
    if (!(susceptibility > 0.0)) continue;

    double total_risk = lambda;
    if (simulation_config_.regional_risk.enabled && venue) {
      total_risk *= venue->transmission_factor;
    }
    double prob = 1.0 - std::exp(-total_risk * susceptibility);
    if (!(prob > 1e-12)) continue;

    SplitMix64 susc_rng(mix_seed(base_seed_, susc_id, venue_key, time_bits));
    double rng_roll = uniform_dist_(susc_rng);
    if (!(rng_roll < prob)) continue;

    // Source attribution: weight-sample from accumulated AccumSource entries.
    auto src_it = susc_sources.find(susc_id);
    int sampled_mode = 0;
    PersonId infector_id = -1;
    if (src_it != susc_sources.end() && !src_it->second.empty()) {
      // Sort for determinism, then cumulative-sample.
      auto& srcs = src_it->second;
      std::sort(srcs.begin(), srcs.end(),
                [](const AccumSource& a, const AccumSource& b) {
                  if (a.mode != b.mode) return a.mode < b.mode;
                  return a.infector < b.infector;
                });
      std::vector<double> cum;
      cum.reserve(srcs.size());
      double acc = 0.0;
      for (const auto& s : srcs) {
        acc += s.weighted;
        cum.push_back(acc);
      }
      int sampled = (acc > 0.0) ? sampleFromCumulative(cum, susc_rng) : 0;
      if (sampled < 0) sampled = 0;
      if (sampled < static_cast<int>(srcs.size())) {
        sampled_mode = srcs[sampled].mode;
        infector_id = srcs[sampled].infector;
      }
    }

    uint16_t infector_symptom_id = 0;
    if (infector_id >= 0) {
      Person* infp = world_.getPerson(infector_id);
      if (infp && infp->infection) {
        infector_symptom_id =
            infp->infection->getTrajectory().getCurrentSymptomId(current_time);
      } else if (!infp && visitor_data) {
        auto vit = visitor_data->find(infector_id);
        if (vit != visitor_data->end())
          infector_symptom_id = vit->second.symptom_id;
      }
    }

    const uint8_t transmission_mode_index = static_cast<uint8_t>(sampled_mode);
    const bool is_visitor_susc = (visitor != nullptr);

    if (is_visitor_susc && pending_infections != nullptr) {
      PendingInfection pending;
      pending.person_id = susc_id;
      pending.infector_id = infector_id;
      pending.infection_time = current_time;
      pending.venue_type_id = venue_type_id;
      pending.encounter_type_id = 255;
      pending.venue_id = actual_venue_id;
      pending.infector_symptom_id = infector_symptom_id;
      pending.transmission_mode_index = transmission_mode_index;
      if (visitor) pending.home_array_index = visitor->home_array_index;
      pending_infections->push_back(pending);
    } else if (susc_person && !susc_person->infection && disease_ != nullptr) {
      float severity_factor = 1.0f;
      auto* gu = world_.getGeoUnit(susc_person->geo_unit_id);
      if (gu) severity_factor = gu->severity_factor;

      std::string venue_type_name;
      if (venue_type_id < world_.venue_type_names.size())
        venue_type_name = world_.venue_type_names[venue_type_id];

      uint64_t infection_seed =
          mix_seed(base_seed_, susc_id,
                   static_cast<uint64_t>(current_time * 1000), venue_key);
      susc_person->infection = std::make_unique<Infection>(
          disease_, current_time, susc_person,
          static_cast<unsigned int>(infection_seed), &world_, venue_type_name,
          actual_venue_id, severity_factor, infector_symptom_id, "", "",
          transmission_mode_index);

      if (event_logger_ != nullptr) {
        event_logger_->logInfection(
            susc_id, infector_id, actual_venue_id, current_time,
            /*encounter_type_id*/ 255, infector_symptom_id,
            transmission_mode_index, InfectionSource::Person);
      }

      if (active_infections != nullptr) active_infections->insert(susc_id);
    }
    new_infections++;
  }

  return new_infections;
}

}  // namespace june
