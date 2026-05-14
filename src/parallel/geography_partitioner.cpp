#ifdef USE_MPI

#include "../../include/parallel/geography_partitioner.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>

#include "../../include/loaders/hdf5_loader.h"

#ifdef USE_METIS
#include <metis.h>
#endif

namespace june {

GeographyPartitioner::GeographyPartitioner(WorldState& world,
                                           const Config& config)
    : world_(world), config_(config) {}

void GeographyPartitioner::loadGeographyMetadata() {
  loadCentroids(config_.parallel.centroids_file);
  loadAdjacency(config_.parallel.adjacency_file);
  mapGeoUnitNamesToIds();
}

void GeographyPartitioner::loadCentroids(const std::string& filename) {
  std::ifstream file(filename);
  if (!file.is_open()) {
    throw std::runtime_error("Could not open centroids file: " + filename);
  }

  std::string line;
  std::getline(file, line);  // Skip header

  while (std::getline(file, line)) {
    if (line.empty()) continue;

    // Parse CSV field that may be quoted (e.g. "YORKSHIRE, EAST RIDING")
    std::string name;
    size_t pos = 0;
    if (line[0] == '"') {
      size_t end_quote = line.find('"', 1);
      if (end_quote == std::string::npos) continue;
      name = line.substr(1, end_quote - 1);
      pos = end_quote + 2;  // skip closing quote + comma
    } else {
      size_t comma_pos = line.find(',');
      if (comma_pos == std::string::npos) continue;
      name = line.substr(0, comma_pos);
      pos = comma_pos + 1;
    }

    float x, y;
    char comma;
    std::istringstream ss(line.substr(pos));
    ss >> x >> comma >> y;

    GeoUnitData& data = geo_data_[name];
    data.name = name;
    data.id = -1;
    data.centroid_x = x;
    data.centroid_y = y;
    data.population = 0;
  }
}

void GeographyPartitioner::loadAdjacency(const std::string& filename) {
  YAML::Node adjacency = YAML::LoadFile(filename);
  for (auto it = adjacency.begin(); it != adjacency.end(); ++it) {
    std::string name = it->first.as<std::string>();
    std::vector<std::string> neighbors =
        it->second.as<std::vector<std::string>>();

    if (geo_data_.count(name)) {
      geo_data_[name].neighbors = neighbors;
    }
  }
}

void GeographyPartitioner::mapGeoUnitNamesToIds() {
  for (auto& [name, data] : geo_data_) {
    for (const auto& gu : world_.geo_units) {
      if (gu.name == name) {
        data.id = gu.id;
        break;
      }
    }
  }
}

void GeographyPartitioner::partition(
    int num_ranks, int rank, std::unordered_set<GeoUnitId>& owned_units) {
#ifndef USE_METIS
  throw std::runtime_error("METIS not available - cannot partition");
#else
  std::vector<std::string> idx_to_name;
  std::unordered_map<std::string, idx_t> name_to_idx;

  for (const auto& [name, data] : geo_data_) {
    if (data.id != -1) {
      name_to_idx[name] = idx_to_name.size();
      idx_to_name.push_back(name);
    }
  }

  idx_t nvtxs = idx_to_name.size();
  if (nvtxs == 0) {
    throw std::runtime_error(
        "GeographyPartitioner: no geo units to partition. The centroids file "
        "'" +
        config_.parallel.centroids_file +
        "' contains no names that match any geo_unit in the world. Check that "
        "parallel.partitioning.level ('" +
        config_.parallel.partition_level +
        "') and the centroids/adjacency files match the world's geography.");
  }

  std::vector<idx_t> xadj(nvtxs + 1, 0);
  std::vector<idx_t> adjncy;
  std::vector<idx_t> vwgt(nvtxs);

  for (idx_t i = 0; i < nvtxs; ++i) {
    const auto& data = geo_data_[idx_to_name[i]];
    vwgt[i] = std::max(1, data.population);
    for (const auto& neighbor : data.neighbors) {
      if (name_to_idx.count(neighbor)) {
        adjncy.push_back(name_to_idx[neighbor]);
      }
    }
    xadj[i + 1] = adjncy.size();
  }

  idx_t ncon = 1, nparts = num_ranks, objval;
  std::vector<idx_t> part(nvtxs);
  idx_t options[METIS_NOPTIONS];
  METIS_SetDefaultOptions(options);
  options[METIS_OPTION_OBJTYPE] = METIS_OBJTYPE_CUT;
  options[METIS_OPTION_NUMBERING] = 0;
  real_t ubvec = 1.0 + config_.parallel.metis_imbalance_tolerance;

  METIS_PartGraphKway(&nvtxs, &ncon, xadj.data(), adjncy.data(), vwgt.data(),
                      nullptr, nullptr, &nparts, nullptr, &ubvec, options,
                      &objval, part.data());

  // Calculate and log partitioning statistics (rank 0 only)
  std::vector<double> rank_pops(num_ranks, 0);
  double total_pop = 0;
  for (idx_t i = 0; i < nvtxs; ++i) {
    rank_pops[part[i]] += vwgt[i];
    total_pop += vwgt[i];
  }

  if (rank == 0) {
    std::cout << "\n=== Partitioning Statistics ===" << std::endl;
    std::cout << "  Total units partitioned: " << nvtxs << " ("
              << config_.parallel.partition_level << "s)" << std::endl;
    std::cout << "  Total population:        " << std::fixed
              << std::setprecision(0) << total_pop << std::endl;

    double avg_pop = total_pop / num_ranks;
    double max_p = 0;
    double min_p = total_pop;

    for (int r = 0; r < num_ranks; ++r) {
      max_p = std::max(max_p, rank_pops[r]);
      min_p = std::min(min_p, rank_pops[r]);
      if (num_ranks <= 64 || r < 8 || r >= num_ranks - 8) {
        std::cout << "    Rank " << std::setw(3) << r << ": " << std::setw(12)
                  << rank_pops[r] << " people";
        if (avg_pop > 0) {
          double dev = (rank_pops[r] / avg_pop - 1.0) * 100.0;
          std::cout << " (" << (dev >= 0 ? "+" : "") << std::fixed
                    << std::setprecision(1) << dev << "%)";
        }
        std::cout << std::endl;
      } else if (r == 8) {
        std::cout << "    ... (ranks 8 to " << num_ranks - 9 << " omitted) ..."
                  << std::endl;
      }
    }

    double imb = (avg_pop > 0) ? (max_p / avg_pop - 1.0) * 100.0 : 0.0;
    std::cout << "  Ideal population/rank:   " << std::fixed
              << std::setprecision(0) << avg_pop << std::endl;
    std::cout << "  Max population/rank:     " << max_p << std::endl;
    std::cout << "  Pop Imbalance:           " << std::fixed
              << std::setprecision(2) << imb << "%" << std::endl;
    std::cout << "===============================\n" << std::endl;
  }

  for (idx_t i = 0; i < nvtxs; ++i) {
    if (part[i] == rank) {
      owned_units.insert(geo_data_[idx_to_name[i]].id);
    }
  }

  const std::string& level = config_.parallel.partition_level;
  for (const auto& gu : world_.geo_units) {
    GeoUnitId parent_id = findParentAtLevel(gu.id, level);
    if (parent_id != -1 && owned_units.count(parent_id)) {
      owned_units.insert(gu.id);
    }
  }
#endif
}

void GeographyPartitioner::loadPopulationWeights(const std::string& h5_file) {
  HDF5Loader loader(h5_file, config_);
  loader.loadRegistries();

  std::vector<int32_t> counts;
  std::vector<int32_t> gu_ids;

  if (loader.datasetExists("/metadata/registries/population_counts")) {
    counts = loader.readNumericDataset<int32_t>(
        "/metadata/registries/population_counts");
    // For registry-based counts, we assume they correspond 1:1 to
    // loader.world_.geo_units if they are SGUs
  } else if (loader.datasetExists("/population/partition_index/counts")) {
    counts = loader.readNumericDataset<int32_t>(
        "/population/partition_index/counts");
    gu_ids = loader.readNumericDataset<int32_t>(
        "/population/partition_index/geo_unit_ids");
  } else if (loader.datasetExists("/population/geo_unit_ids")) {
    // ULTIMATE FALLBACK: Calculate counts by reading all IDs (can be slow but
    // accurate)
    auto all_ids =
        loader.readNumericDataset<int32_t>("/population/geo_unit_ids");
    std::unordered_map<int32_t, int> counts_map;
    for (int32_t id : all_ids) {
      counts_map[id]++;
    }
    for (const auto& [id, count] : counts_map) {
      gu_ids.push_back(id);
      counts.push_back(count);
    }
  } else {
    std::cerr << "  Warning: No population data found in HDF5. "
                 "Using unweighted partitioning."
              << std::endl;
    return;
  }

  loader.loadGeography();
  loader.world_.buildIndices();  // Build indices for the local world object

  // Propagate registries to the partitioner's world_ so findParentAtLevel()
  // can resolve geographic hierarchy levels. The registries are instance
  // members on WorldState, so they must be explicitly copied.
  if (world_.geo_level_names.empty()) {
    world_.geo_level_names = loader.world_.geo_level_names;
  }

  std::string partition_level = config_.parallel.partition_level;

  // Reset all population counts
  for (auto& [name, data] : geo_data_) {
    data.population = 0;
  }

  if (!gu_ids.empty()) {
    // CASE A: We have explicit GeoUnit IDs for each count (from
    // partition_index)
    int matched_parents = 0;
    int missing_parents = 0;
    int unknown_geo_names = 0;

    for (size_t i = 0; i < gu_ids.size(); ++i) {
      GeoUnitId sgu_id = gu_ids[i];
      int pop = counts[i];

      GeoUnitId parent_id = findParentAtLevel(sgu_id, partition_level);
      if (parent_id != -1) {
        const GeographicalUnit* parent_gu = loader.world_.getGeoUnit(parent_id);
        if (parent_gu) {
          if (geo_data_.count(parent_gu->name)) {
            geo_data_[parent_gu->name].population += pop;
            matched_parents++;
          } else {
            unknown_geo_names++;
            if (unknown_geo_names < 5) {
            }
          }
        }
      } else {
        missing_parents++;
      }
    }
    if (missing_parents > 0 || unknown_geo_names > 0) {
      std::cerr << "  Warning: Partitioner aggregation: " << missing_parents
                << " missing parents, " << unknown_geo_names << " unknown names"
                << std::endl;
    }
  } else {
    // CASE B: Fallback to scanning all geo units (assuming counts match SGUs in
    // order)
    int matched_parents = 0;
    int missing_parents = 0;
    int unknown_geo_names = 0;
    int sgu_count = 0;
    int sgu_idx = 0;

    for (const auto& gu : loader.world_.geo_units) {
      std::string level = (gu.level_id < loader.world_.geo_level_names.size())
                              ? loader.world_.geo_level_names[gu.level_id]
                              : "unknown";

      if (level == "SGU") {
        sgu_count++;
        if (sgu_idx < (int)counts.size()) {
          int pop = counts[sgu_idx++];
          GeoUnitId parent_id = findParentAtLevel(gu.id, partition_level);
          if (parent_id != -1) {
            const GeographicalUnit* parent_gu =
                loader.world_.getGeoUnit(parent_id);
            if (parent_gu) {
              if (geo_data_.count(parent_gu->name)) {
                geo_data_[parent_gu->name].population += pop;
                matched_parents++;
              } else {
                unknown_geo_names++;
              }
            }
          } else {
            missing_parents++;
          }
        }
      }
    }
    if (missing_parents > 0 || unknown_geo_names > 0) {
      std::cerr << "  Warning: Partitioner aggregation: " << missing_parents
                << " missing parents, " << unknown_geo_names << " unknown names"
                << std::endl;
    }
  }
}

GeoUnitId GeographyPartitioner::findParentAtLevel(
    GeoUnitId child_id, const std::string& target_level) const {
  const GeographicalUnit* current = world_.getGeoUnit(child_id);
  while (current) {
    std::string current_level =
        (current->level_id < world_.geo_level_names.size())
            ? world_.geo_level_names[current->level_id]
            : "unknown";
    if (current_level == target_level) return current->id;
    if (current->parent_id == -1) break;
    current = world_.getGeoUnit(current->parent_id);
  }
  return -1;
}

}  // namespace june

#endif  // USE_MPI
