#pragma once

#ifdef USE_MPI

#include <mpi.h>

#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../activity/coordinated_encounter_types.h"
#include "../core/config.h"
#include "../core/world_state.h"
#include "../epidemiology/disease.h"
#include "domain.h"

namespace june {
class DomainManager;

/**
 * DomainCommunicator - Handles adaptive MPI communication for visitors and
 * infections
 */
class DomainCommunicator {
 public:
  DomainCommunicator(WorldState& world, const Config& config, Domain& domain);

  void setDisease(const Disease* disease) { disease_ = disease; }

  // Exchange visitors across ranks
  void exchangeVisitors(const std::vector<PersonLocation>& locations,
                        const DomainManager& dm, double current_time,
                        double delta_hours = 0.0);

  // Exchange pending infections across ranks. Returns the PendingInfection
  // records that this rank applied (i.e. for which a new local infection was
  // created on the home rank), so the caller can log InfectionEvents on the
  // owning rank.
  std::vector<PendingInfection> receivePendingInfections(
      const std::vector<PendingInfection>& pending_infections);

  // Cross-rank coordinated encounter exchange
  void exchangeEncounterProposals(
      const std::vector<EncounterProposal>& local_proposals,
      const DomainManager& dm,
      std::vector<EncounterProposal>& proposals_for_this_rank);
  void exchangeEncounterReplies(
      const std::vector<EncounterReply>& local_replies, const DomainManager& dm,
      std::vector<EncounterReply>& replies_for_this_rank);
  void exchangeFinalizedEncounters(
      const std::vector<CoordinatedEncounter>& local_finalized,
      const DomainManager& dm,
      std::vector<CoordinatedEncounter>& finalized_for_this_rank);

 private:
  void exchangeAllToAll(
      const std::vector<std::vector<Domain::VisitorData>>& outgoing,
      const std::vector<int>& send_counts);
  void exchangePointToPoint(
      const std::vector<std::vector<Domain::VisitorData>>& outgoing,
      const std::vector<int>& send_counts);
  void exchangePointToPointVerySparse(
      const std::vector<std::vector<Domain::VisitorData>>& outgoing,
      const std::vector<int>& send_counts);

  // Shared P2P logic for visitor exchange
  void performP2PVisitorExchange(
      const std::vector<std::vector<Domain::VisitorData>>& outgoing,
      const std::vector<int>& send_counts, const std::vector<int>& recv_counts);

  WorldState& world_;
  const Config& config_;
  Domain& domain_;
  const Disease* disease_;
  int rank_, num_ranks_;
};

}  // namespace june

#endif  // USE_MPI
