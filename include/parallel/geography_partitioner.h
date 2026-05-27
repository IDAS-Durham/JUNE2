#pragma once

#ifdef USE_MPI

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/config.h"
#include "core/types.h"
#include "core/world_state.h"

namespace june {

/**
 * GeographyPartitioner - Handles loading of geographic data and partitioning
 * via METIS
 */
class GeographyPartitioner {
 public:
  struct GeoUnitData {
    GeoUnitId id;
    std::string name;
    float centroid_x;
    float centroid_y;
    int population;
    std::vector<std::string> neighbors;
  };

  GeographyPartitioner(WorldState& world, const Config& config);

  // Load centroids and adjacency data from files
  void loadGeographyMetadata();

  // Perform METIS partitioning
  void partition(int num_ranks, int rank,
                 std::unordered_set<GeoUnitId>& owned_units);

  // Load actual population counts from HDF5 if on rank 0
  void loadPopulationWeights(const std::string& h5_file);

  // Helper: Find parent geo unit at specific level
  GeoUnitId findParentAtLevel(GeoUnitId child_id,
                              const std::string& target_level) const;

  // Get population data for broadcasting
  const std::unordered_map<std::string, GeoUnitData>& getGeoData() const {
    return geo_data_;
  }
  std::unordered_map<std::string, GeoUnitData>& getGeoData() {
    return geo_data_;
  }

 private:
  void loadCentroids(const std::string& filename);
  void loadAdjacency(const std::string& filename);
  void mapGeoUnitNamesToIds();

  WorldState& world_;
  const Config& config_;
  std::unordered_map<std::string, GeoUnitData> geo_data_;
  int rank_;
};

}  // namespace june

#endif  // USE_MPI
