// HDF5Loader methods that build WorldState from an HDF5 world file:
// load(), loadGeographyOnly(), loadRegistries(), loadGeography(), and the
// top-level loadDomainChunked() orchestrator. The per-chunk and per-section
// helpers live in domain_loader_internals.{h,cpp}; the low-level HDF5 read
// primitives live in hdf5_loader.cpp.

#include <algorithm>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <variant>

#ifdef USE_MPI
#include <mpi.h>
#endif

#include "core/config.h"
#include "core/world_state.h"
#include "loaders/domain_loader_internals.h"
#include "loaders/hdf5_loader.h"
#include "utils/memory_utils.h"

namespace june {

WorldState HDF5Loader::load(const std::string& filename, const Config& config) {
  // Load entire world using chunked loading

  // First, load geography to get all geo_unit IDs
  HDF5Loader temp_loader(filename, config);
  temp_loader.loadGeography();

  // Collect all geo_unit IDs into a set
  std::unordered_set<GeoUnitId> all_geo_units;
  for (const auto& gu : temp_loader.world_.geo_units) {
    all_geo_units.insert(gu.id);
  }

  MemoryUtils::logMemory("Start of loadDomainChunked");

  // Performance note: chunk_size for serial loading is fixed
  size_t chunk_size = 1000;
  return loadDomainChunked(filename, all_geo_units, chunk_size, config);
}

WorldState HDF5Loader::loadGeographyOnly(const std::string& filename) {
  // This method does not need config, as it only loads geography.
  // The HDF5Loader constructor used here will default-construct config_
  // which is fine as it's not used by loadGeography().
  HDF5Loader loader(filename, Config());

  // Load registries first (needed for geo level name resolution)
  loader.loadRegistries();
  loader.loadGeography();
  loader.world_.buildIndices();

  return std::move(loader.world_);
}

void HDF5Loader::loadRegistries() {
  if (!groupExists("/metadata/registries")) return;

  // 1. Geography levels
  if (datasetExists("/metadata/registries/geo_levels")) {
    world_.geo_level_names =
        readStringDataset("/metadata/registries/geo_levels");
  }

  // 2. Sex mapping (stored as attribute on group)
  if (groupExists("/metadata/registries/sex")) {
    // Handled via Enum in C++, but we could log the mapping if needed
  }

  // 3. Venue types
  if (datasetExists("/metadata/registries/venue_types")) {
    world_.venue_type_names =
        readStringDataset("/metadata/registries/venue_types");
  }

  // 4. Schedule types
  if (datasetExists("/metadata/registries/schedule_types")) {
    world_.schedule_type_names =
        readStringDataset("/metadata/registries/schedule_types");
  }

  // 5. Categorical properties
  if (groupExists("/metadata/registries/properties")) {
    auto prop_names = getDatasetNames("/metadata/registries/properties");
    world_.person_property_names = prop_names;
    for (const auto& name : prop_names) {
      world_.person_property_value_registries[name] =
          readStringDataset("/metadata/registries/properties/" + name);
    }
  }

  if (groupExists("/metadata/registries/geography_properties")) {
    auto prop_names =
        getDatasetNames("/metadata/registries/geography_properties");
    world_.geo_unit_property_names = prop_names;
    for (const auto& name : prop_names) {
      world_.geo_unit_property_value_registries[name] = readStringDataset(
          "/metadata/registries/geography_properties/" + name);
    }
  }

  // 6. Subset types
  if (datasetExists("/metadata/registries/subset_names")) {
    world_.subset_type_names =
        readStringDataset("/metadata/registries/subset_names");
  }

  // 7. Encounter types
  if (datasetExists("/metadata/registries/encounter_types")) {
    world_.encounter_type_names =
        readStringDataset("/metadata/registries/encounter_types");
  }
}

void HDF5Loader::loadGeography() {
  auto ids = readNumericDataset<int32_t>("/geography/ids");
  std::vector<std::string> names;
  if (datasetExists("/metadata/names/geography")) {
    names = readStringDataset("/metadata/names/geography");
  } else {
    names = readStringDataset("/geography/names");
  }

  auto level_ids = readNumericDataset<uint8_t>("/geography/levels");
  auto parent_ids = readNumericDataset<int32_t>("/geography/parent_ids");

  std::vector<float> latitudes, longitudes;
  if (datasetExists("/geography/latitudes")) {
    latitudes = readNumericDataset<float>("/geography/latitudes");
    longitudes = readNumericDataset<float>("/geography/longitudes");
  }

  size_t count = ids.size();
  world_.geo_units.resize(count);

  for (size_t i = 0; i < count; ++i) {
    world_.geo_units[i].id = ids[i];
    world_.geo_units[i].name = names[i];
    world_.geo_units[i].level_id = level_ids[i];
    world_.geo_units[i].parent_id = parent_ids[i];
    world_.geo_units[i].latitude = latitudes.empty() ? 0.0f : latitudes[i];
    world_.geo_units[i].longitude = longitudes.empty() ? 0.0f : longitudes[i];
  }

  // Load geography properties if they exist
  if (groupExists("/geography/properties")) {
    auto property_names = getDatasetNames("/geography/properties");
    world_.geo_unit_property_names = property_names;

    std::vector<std::vector<PropertyValue>> property_columns;
    for (const auto& prop_name : property_names) {
      property_columns.push_back(readPropertyDatasetRange(
          "/geography/properties/" + prop_name, 0, count, prop_name));
    }

    for (size_t i = 0; i < count; ++i) {
      world_.geo_units[i].properties_start =
          static_cast<uint32_t>(world_.geo_unit_properties.size());
      world_.geo_units[i].properties_count =
          static_cast<uint8_t>(property_columns.size());

      for (size_t k = 0; k < property_columns.size(); ++k) {
        const auto& prop_name = property_names[k];
        const auto& prop_val = property_columns[k][i];

        int32_t interned_val = -1;
        if (std::holds_alternative<int32_t>(prop_val)) {
          interned_val = std::get<int32_t>(prop_val);
        } else if (std::holds_alternative<double>(prop_val)) {
          interned_val = static_cast<int32_t>(std::get<double>(prop_val));
        } else if (std::holds_alternative<bool>(prop_val)) {
          interned_val = std::get<bool>(prop_val) ? 1 : 0;
        } else if (std::holds_alternative<std::string>(prop_val)) {
          const std::string& s = std::get<std::string>(prop_val);
          if (!s.empty()) {
            auto& registry =
                world_.geo_unit_property_value_registries[prop_name];
            auto it = std::find(registry.begin(), registry.end(), s);
            if (it == registry.end()) {
              interned_val = static_cast<int32_t>(registry.size());
              registry.push_back(s);
            } else {
              interned_val =
                  static_cast<int32_t>(std::distance(registry.begin(), it));
            }
          }
        }
        world_.geo_unit_properties.push_back(interned_val);
      }
    }
  }
}

WorldState HDF5Loader::loadDomainChunked(
    const std::string& filename,
    const std::unordered_set<GeoUnitId>& owned_geo_units, size_t chunk_size,
    const Config& config) {
  HDF5Loader loader(filename, config);

  int mpi_rank = 0;
#ifdef USE_MPI
  MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);
#endif

  loader.loadRegistries();
  loader.loadGeography();

  // Convert owned_geo_units to a sorted vector for chunking
  std::vector<GeoUnitId> geo_units_vec(owned_geo_units.begin(),
                                       owned_geo_units.end());
  std::sort(geo_units_vec.begin(), geo_units_vec.end());
  size_t num_geo_units = geo_units_vec.size();
  size_t num_chunks = (num_geo_units + chunk_size - 1) / chunk_size;

  // Caches for interning property values across chunks
  std::unordered_map<std::string, std::unordered_map<std::string, int32_t>>
      property_indices_cache;
  std::unordered_map<std::string, std::unordered_map<std::string, int32_t>>
      venue_property_indices_cache;

  // Pre-reserve people vector to avoid reallocations during chunked load
  if (loader.datasetExists("/metadata/registries/population_counts")) {
    auto counts = loader.readNumericDataset<int32_t>(
        "/metadata/registries/population_counts");
    size_t total_p = 0;
    for (int c : counts) total_p += c;
    if (total_p > 0) loader.world_.people.reserve(total_p);
  }

  // Convert partition indexes into per-geo_unit (start, count) maps
  auto pop_partition_map =
      detail::buildPartitionMap(loader, "/population/partition_index");
  auto venue_partition_map =
      detail::buildPartitionMap(loader, "/venues/partition_index");
  auto rel_partition_map = detail::buildPartitionMap(
      loader, "/activity_mappings/activity_map/partition_index");

  // Activity name registries: HDF5-defined + system + config-referenced
  loader.world_.activity_names = loader.readStringDataset(
      "/activity_mappings/activity_map/activity_names");
  detail::registerSystemAndConfigActivities(loader.world_, config);
  detail::syncScheduleTypeNames(loader.world_, config);

  // Discover all population property datasets dynamically and register them
  std::vector<std::string> population_property_names =
      loader.getDatasetNames("/population/properties");
  loader.world_.person_property_names = population_property_names;

  // Discover venue property names per type; values are loaded lazily later
  auto venue_type_prop_names = detail::discoverVenuePropertyNames(loader);

  // Person index built up incrementally across chunks (used by activity-
  // mapping stitching and membership_metadata side-table lookup)
  std::unordered_map<PersonId, size_t> local_person_idx_map;
  if (loader.world_.people.capacity() > 0)
    local_person_idx_map.reserve(loader.world_.people.capacity());

  // Process each chunk: read its population + venue + activity-mapping spans
  for (size_t chunk_idx = 0; chunk_idx < num_chunks; ++chunk_idx) {
    size_t start_idx = chunk_idx * chunk_size;
    size_t end_idx = std::min(start_idx + chunk_size, num_geo_units);
    size_t people_before_chunk = loader.world_.people.size();

    MemoryUtils::logMemory("Before GeoUnit Chunk " +
                           std::to_string(chunk_idx + 1));

    auto pop_spans = detail::detectChunkSpans(pop_partition_map, geo_units_vec,
                                              start_idx, end_idx);
    loader.world_.network_names = population_property_names;
    for (const auto& span : pop_spans) {
      detail::loadPersonsInSpan(loader, span, pop_partition_map, geo_units_vec,
                                population_property_names,
                                property_indices_cache);
    }

    auto venue_spans = detail::detectChunkSpans(venue_partition_map,
                                                geo_units_vec, start_idx,
                                                end_idx);
    for (const auto& span : venue_spans) {
      detail::loadVenuesInSpan(loader, span, venue_partition_map,
                               geo_units_vec, venue_type_prop_names,
                               venue_property_indices_cache);
    }

    // Extend the local person index with people added by this chunk
    for (size_t idx = people_before_chunk; idx < loader.world_.people.size();
         ++idx) {
      local_person_idx_map[loader.world_.people[idx].id] = idx;
    }

    if (loader.groupExists("/activity_mappings/activity_map")) {
      auto rel_spans = detail::detectChunkSpans(rel_partition_map,
                                                geo_units_vec, start_idx,
                                                end_idx);
      for (const auto& span : rel_spans) {
        detail::loadActivityMappingsInSpan(loader, span, local_person_idx_map);
      }
    }
  }

  detail::loadMembershipMetadata(loader, local_person_idx_map);

  // Build venue index before loading subsets so getVenue() resolves
  for (size_t i = 0; i < loader.world_.venues.size(); ++i) {
    loader.world_.venue_index[loader.world_.venues[i].id] = i;
  }

  detail::loadVenueSubsets(loader, owned_geo_units);
  detail::buildGlobalVenueTypeMap(loader);

  MemoryUtils::logMemory("After loading all chunks, before indexing");
  loader.world_.buildIndices();
  MemoryUtils::logMemory("After indexing final state");

  if (mpi_rank == 0) {
    std::cout << "  Domain loaded: " << loader.world_.people.size()
              << " people, " << loader.world_.venues.size() << " venues"
              << std::endl;
  }

  return std::move(loader.world_);
}

}  // namespace june
