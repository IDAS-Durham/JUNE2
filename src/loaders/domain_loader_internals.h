#pragma once

// Internal helpers shared between domain_loader.cpp and
// domain_loader_internals.cpp. Not part of the public HDF5Loader API:
// kept in namespace `june::detail` and intended only for these two TUs
// and their focused tests.

#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/types.h"

namespace june {

class HDF5Loader;
class WorldState;
struct Config;

namespace detail {

using GeoPartitionMap =
    std::unordered_map<GeoUnitId, std::pair<size_t, size_t>>;

struct ChunkSpan {
  size_t start;
  size_t count;
  std::vector<size_t> gu_indices;
};

GeoPartitionMap buildPartitionMap(HDF5Loader& loader,
                                  const std::string& path_prefix);

std::vector<ChunkSpan> detectChunkSpans(
    const GeoPartitionMap& partition_map,
    const std::vector<GeoUnitId>& geo_units_vec, size_t start_idx,
    size_t end_idx);

void registerSystemAndConfigActivities(WorldState& world, const Config& config);

void syncScheduleTypeNames(WorldState& world, const Config& config);

std::unordered_map<std::string, std::vector<std::string>>
discoverVenuePropertyNames(HDF5Loader& loader);

void loadPersonsInSpan(
    HDF5Loader& loader, const ChunkSpan& span,
    const GeoPartitionMap& pop_partition_map,
    const std::vector<GeoUnitId>& geo_units_vec,
    const std::vector<std::string>& population_property_names,
    std::unordered_map<std::string, std::unordered_map<std::string, int32_t>>&
        property_indices_cache,
    std::unordered_map<std::string,
                       std::map<std::vector<std::string>, int32_t>>&
        list_property_indices_cache);

void loadVenuesInSpan(
    HDF5Loader& loader, const ChunkSpan& span,
    const GeoPartitionMap& venue_partition_map,
    const std::vector<GeoUnitId>& geo_units_vec,
    const std::unordered_map<std::string, std::vector<std::string>>&
        venue_type_prop_names,
    std::unordered_map<std::string, std::unordered_map<std::string, int32_t>>&
        venue_property_indices_cache);

void loadActivityMappingsInSpan(
    HDF5Loader& loader, const ChunkSpan& span,
    const std::unordered_map<PersonId, size_t>& local_person_idx_map);

void loadMembershipMetadata(
    HDF5Loader& loader,
    const std::unordered_map<PersonId, size_t>& local_person_idx_map);

constexpr uint32_t kAbsentFlatIndex = std::numeric_limits<uint32_t>::max();

uint32_t matchMembershipRowToFlatIndex(const WorldState& world,
                                       const Person& person, VenueId venue_id,
                                       const SubsetIndex* subset_index);

void loadVenueSubsets(HDF5Loader& loader,
                      const std::unordered_set<GeoUnitId>& owned_geo_units);

void fillGlobalVenueMaps(WorldState& world,
                         const std::vector<int32_t>& venue_ids,
                         const std::vector<uint8_t>& venue_type_ids,
                         const std::vector<GeoUnitId>& venue_geo_unit_ids);

void buildGlobalVenueMaps(HDF5Loader& loader);

}  // namespace detail
}  // namespace june
