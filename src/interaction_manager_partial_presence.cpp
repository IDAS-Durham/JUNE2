// Split from interaction_manager.cpp — see REFACTOR_PLAN.md Phase 16.
// Contains the processPartialPresenceVenue family (commute-line FOI path).
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
#include "utils/random.h"

namespace june {

// Routing gate called from processVenueTransmissions (venue.cpp). Returns a
// new-infection count (forwarded from processPartialPresenceVenue) when the
// venue type is declared in SimulationConfig::partial_presence; otherwise
// returns nullopt and the caller falls through to the standard FOI path.
std::optional<int> InteractionManager::dispatchPartialPresenceIfApplicable(
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
  // SimulationConfig::partial_presence pay no cost here.
  if (!runtime_bin_allocator_ || actual_venue_id < 0 || !venue) return {};
  const uint8_t vt = venue->type_id;
  const uint64_t mask =
      simulation_config_.partial_presence.enabled_venue_type_mask;
  if (vt >= 64 || ((mask >> vt) & 1ULL) == 0) return {};
  return processPartialPresenceVenue(
      members, venue, actual_venue_id, current_time, delta_hours,
      active_infections, visitor_ids, pending_infections, visitor_data,
      encounter_type_id, comp_model);
}

void InteractionManager::accumulateOneCarriage(
    const std::vector<CarriageMember>& car, float slot_duration_min,
    double current_time, double delta_hours, int num_modes,
    int num_bins_needed, uint8_t venue_type_id, const ContactMatrix* matrix,
    const TransmissionParams& trans_params,
    std::vector<PartialPresenceSubBin>& sub_bins,
    PartialPresenceLambdaResult& result) const {
  std::vector<float> events =
      collectSubIntervalEventTimes(car, slot_duration_min);
  if (events.size() < 2) return;

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

    classifyMembersInSubInterval(car, t0, t1, scale, current_time, delta_hours,
                                 num_modes, sub_bins, susc_by_bin);

    accumulatePartialLambdaContributions(sub_bins, susc_by_bin, venue_type_id,
                                         matrix, num_bins_needed, num_modes,
                                         trans_params, result);
  }
}

void InteractionManager::accumulatePartialLambdaContributions(
    const std::vector<PartialPresenceSubBin>& sub_bins,
    const std::vector<std::vector<const CarriageMember*>>& susc_by_bin,
    uint8_t venue_type_id, const ContactMatrix* matrix, int num_bins_needed,
    int num_modes, const TransmissionParams& trans_params,
    PartialPresenceLambdaResult& result) const {
  using AccumSource = PartialPresenceAccumSource;
  auto& susc_lambda = result.susc_lambda;
  auto& susc_sources = result.susc_sources;

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

        double contacts =
            lookupContactsForBinPair(mode_matrix, matrix, susc_bin, inf_bin);
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

void InteractionManager::classifyMembersInSubInterval(
    const std::vector<CarriageMember>& car, float t0, float t1, double scale,
    double current_time, double delta_hours, int num_modes,
    std::vector<PartialPresenceSubBin>& sub_bins,
    std::vector<std::vector<const CarriageMember*>>& susc_by_bin) const {
  for (const auto& m : car) {
    // Present iff [eff_board, eff_alight) covers [t0, t1).
    if (!(m.eff_board <= t0 + 1e-5f && m.eff_alight + 1e-5f >= t1)) continue;

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
}

std::vector<float> InteractionManager::collectSubIntervalEventTimes(
    const std::vector<CarriageMember>& car, float slot_duration_min) const {
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
  return events;
}

std::vector<std::vector<CarriageMember>>
InteractionManager::buildPartialPresenceCarriages(
    const std::vector<InteractionMember>& members, Venue* venue,
    VenueId actual_venue_id, const ContactMatrix* matrix, int num_bins_needed,
    uint16_t num_bins,
    const std::unordered_map<PersonId, VisitorInfo>* visitor_data) const {
  std::vector<std::vector<CarriageMember>> carriages(num_bins);

  for (const auto& m : members) {
    const uint16_t carriage =
        runtime_bin_allocator_->getBinIndex(actual_venue_id, m.id);
    if (carriage == RuntimeBinAllocator::kNoBin || carriage >= num_bins)
      continue;

    Person* person = nullptr;
    const VisitorInfo* visitor = nullptr;
    if (!resolvePersonAndVisitor(m.id, m.array_index, visitor_data, person,
                                 visitor)) {
      continue;
    }

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
  return carriages;
}

void InteractionManager::validatePartialPresencePreconditions(
    const Venue* venue, VenueId actual_venue_id,
    uint8_t encounter_type_id) const {
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
}

InteractionManager::PartialPresenceLambdaResult
InteractionManager::computePartialPresenceLambda(
    const std::vector<InteractionMember>& members, Venue* venue,
    VenueId actual_venue_id, double current_time, double delta_hours,
    const std::unordered_map<PersonId, VisitorInfo>* visitor_data,
    uint8_t encounter_type_id) {
  PartialPresenceLambdaResult result;

  validatePartialPresencePreconditions(venue, actual_venue_id,
                                       encounter_type_id);

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

  std::vector<std::vector<CarriageMember>> carriages =
      buildPartialPresenceCarriages(members, venue, actual_venue_id, matrix,
                                    num_bins_needed, num_bins, visitor_data);

  // Per-bin scratch reused across sub-intervals (cleared per sub-interval).
  std::vector<PartialPresenceSubBin> sub_bins(num_bins_needed);

  for (uint16_t c = 0; c < num_bins; ++c) {
    const auto& car = carriages[c];
    if (car.empty()) continue;
    accumulateOneCarriage(car, slot_duration_min, current_time, delta_hours,
                          num_modes, num_bins_needed, venue_type_id, matrix,
                          trans_params, sub_bins, result);
  }

  return result;
}

void InteractionManager::applyPartialPresenceInfection(
    PersonId susc_id, Person* susc_person, const VisitorInfo* visitor,
    PersonId infector_id, uint8_t transmission_mode_index,
    uint16_t infector_symptom_id, double current_time, Venue* venue,
    uint8_t venue_type_id, VenueId actual_venue_id,
    std::unordered_set<PersonId>* active_infections,
    std::vector<PendingInfection>* pending_infections) {
  const bool is_visitor_susc = (visitor != nullptr);
  const uint64_t venue_key = static_cast<uint64_t>(actual_venue_id);

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
    return;
  }
  if (!(susc_person && !susc_person->infection && disease_ != nullptr)) return;

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
  (void)venue;  // venue param kept for parity with processVenueTransmissions
                // call shape; only used to feed transmission_factor in the
                // caller's regional-risk multiply.
}

std::pair<int, PersonId> InteractionManager::sampleInfectorFromAccumSources(
    std::vector<PartialPresenceAccumSource>& srcs, SplitMix64& rng) const {
  if (srcs.empty()) return {0, -1};

  // Sort for determinism, then cumulative-sample.
  std::sort(srcs.begin(), srcs.end(),
            [](const PartialPresenceAccumSource& a,
               const PartialPresenceAccumSource& b) {
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
  int sampled = (acc > 0.0) ? sampleFromCumulative(cum, rng) : 0;
  if (sampled < 0) sampled = 0;
  if (sampled >= static_cast<int>(srcs.size())) return {0, -1};
  return {srcs[sampled].mode, srcs[sampled].infector};
}

std::vector<PersonId> InteractionManager::orderSusceptibles(
    const std::unordered_map<PersonId, double>& susc_lambda) const {
  std::vector<PersonId> ordered;
  ordered.reserve(susc_lambda.size());
  for (const auto& kv : susc_lambda) ordered.push_back(kv.first);
  std::sort(ordered.begin(), ordered.end());
  return ordered;
}

double InteractionManager::computeMemberSusceptibility(
    const Person* person, const VisitorInfo* visitor,
    double current_time) const {
  if (person) {
    return person->getSusceptibility(current_time, disease_->getName());
  }
  if (visitor) return 1.0 - visitor->immunity_level;
  return 0.0;
}

bool InteractionManager::processOnePartialSusceptible(
    PersonId susc_id,
    const std::unordered_map<PersonId, double>& susc_lambda,
    std::unordered_map<PersonId, std::vector<PartialPresenceAccumSource>>&
        susc_sources,
    double current_time, Venue* venue, uint8_t venue_type_id,
    VenueId actual_venue_id,
    const std::unordered_map<PersonId, VisitorInfo>* visitor_data,
    std::unordered_set<PersonId>* active_infections,
    std::vector<PendingInfection>* pending_infections, uint64_t time_bits,
    uint64_t venue_key) {
  auto lambda_it = susc_lambda.find(susc_id);
  if (lambda_it == susc_lambda.end()) return false;
  double lambda = lambda_it->second;
  if (!(lambda > 0.0)) return false;

  Person* susc_person = world_.getPerson(susc_id);
  const VisitorInfo* visitor = nullptr;
  if (!susc_person && visitor_data) {
    auto it = visitor_data->find(susc_id);
    if (it != visitor_data->end()) visitor = &it->second;
  }

  if (!susc_person && !visitor) return false;
  double susceptibility =
      computeMemberSusceptibility(susc_person, visitor, current_time);
  if (!(susceptibility > 0.0)) return false;

  double total_risk = lambda;
  if (simulation_config_.regional_risk.enabled && venue) {
    total_risk *= venue->transmission_factor;
  }
  double prob = 1.0 - std::exp(-total_risk * susceptibility);
  if (!(prob > 1e-12)) return false;

  SplitMix64 susc_rng(mix_seed(base_seed_, susc_id, venue_key, time_bits));
  double rng_roll = uniform_dist_(susc_rng);
  if (!(rng_roll < prob)) return false;

  // Source attribution: weight-sample from accumulated AccumSource entries.
  auto src_it = susc_sources.find(susc_id);
  int sampled_mode = 0;
  PersonId infector_id = -1;
  if (src_it != susc_sources.end()) {
    std::tie(sampled_mode, infector_id) =
        sampleInfectorFromAccumSources(src_it->second, susc_rng);
  }

  uint16_t infector_symptom_id =
      resolveInfectorSymptomId(infector_id, current_time, visitor_data);

  const uint8_t transmission_mode_index = static_cast<uint8_t>(sampled_mode);
  applyPartialPresenceInfection(susc_id, susc_person, visitor, infector_id,
                                transmission_mode_index, infector_symptom_id,
                                current_time, venue, venue_type_id,
                                actual_venue_id, active_infections,
                                pending_infections);
  return true;
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

  // Per-susceptible Bernoulli draws (person_id-ordered for deterministic
  // per-call work).
  std::vector<PersonId> ordered_susc = orderSusceptibles(susc_lambda);

  int new_infections = 0;
  const uint64_t time_bits = static_cast<uint64_t>(current_time * 1000);
  const uint64_t venue_key = static_cast<uint64_t>(actual_venue_id);

  for (PersonId susc_id : ordered_susc) {
    if (processOnePartialSusceptible(susc_id, susc_lambda, susc_sources,
                                     current_time, venue, venue_type_id,
                                     actual_venue_id, visitor_data,
                                     active_infections, pending_infections,
                                     time_bits, venue_key)) {
      new_infections++;
    }
  }

  return new_infections;
}

}  // namespace june
