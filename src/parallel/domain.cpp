#ifdef USE_MPI

#include "parallel/domain.h"

#include <iostream>

#include "utils/deterministic_rng.h"
#include "utils/random.h"

namespace june {

Domain::Domain(int rank, int num_ranks, WorldState* world, uint64_t base_seed)
    : rank(rank),
      num_ranks(num_ranks),
      world(world),
      local_population(0),
      local_venues(0) {
  // Deterministic per-rank seed derived from global seed (MPI reproducible)
  rng.seed(static_cast<unsigned int>(mix_seed(base_seed, rank, 0xD0A1AULL)));
}

void Domain::assignPeopleAndVenues() {
  // Clear existing assignments
  resident_ids.clear();
  resident_set.clear();
  local_venue_ids.clear();
  local_venue_set.clear();

  // Assign people: A person belongs to the domain that owns their residence
  // geo_unit
  for (const auto& person : world->people) {
    if (ownsGeoUnit(person.geo_unit_id)) {
      resident_ids.push_back(person.id);
      resident_set.insert(person.id);
    }
  }

  // Assign venues: A venue belongs to the domain that owns its geo_unit
  for (const auto& venue : world->venues) {
    if (ownsGeoUnit(venue.geo_unit_id)) {
      local_venue_ids.push_back(venue.id);
      local_venue_set.insert(venue.id);
    }
  }

  local_population = resident_ids.size();
  local_venues = local_venue_ids.size();
}

void Domain::printStatistics() const {
  std::cout << "  Rank " << rank << ": " << geo_unit_ids.size()
            << " geo units, " << local_population << " people, " << local_venues
            << " venues" << std::endl;
}

}  // namespace june

#endif  // USE_MPI
