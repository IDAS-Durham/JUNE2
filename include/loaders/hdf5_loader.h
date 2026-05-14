#pragma once

#include <H5Cpp.h>

#include <string>
#include <vector>
#ifdef USE_MPI
#include <mpi.h>
#endif
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <variant>

#include "../core/config.h"
#include "../core/world_state.h"
#include "../utils/memory_utils.h"
#include "../utils/time_utils.h"

namespace june {

class HDF5Loader {
 public:
  // Lightweight metadata for partitioning
  struct PersonMetadata {
    PersonId person_id;
    GeoUnitId geo_unit_id;
  };

  // Load world state from HDF5 file
  static WorldState load(const std::string& filename, const Config& config);

  // Load person metadata in chunks
  // Callback is called for each chunk: void callback
  template <typename Callback>
  static void loadPersonMetadataChunked(const std::string& filename,
                                        size_t chunk_size, Callback callback);

  // Load only geography (lightweight, for non-zero ranks during initialization)
  static WorldState loadGeographyOnly(const std::string& filename);

  // Load world state for a specific domain with chunked loading
  // chunk_size: number of geo_units to process at once
  static WorldState loadDomainChunked(
      const std::string& filename,
      const std::unordered_set<GeoUnitId>& owned_geo_units, size_t chunk_size,
      const Config& config);

  explicit HDF5Loader(const std::string& filename, const Config& config);

  void loadGeography();

  // Helper: read string dataset
  std::vector<std::string> readStringDataset(const std::string& path);

  // Helper: read numeric dataset
  template <typename T>
  std::vector<T> readNumericDataset(const std::string& path);

  // Helper: read numeric dataset range
  template <typename T>
  std::vector<T> readNumericDatasetRange(const std::string& path, size_t start,
                                         size_t count);

  // Helper: read string dataset range
  std::vector<std::string> readStringDatasetRange(const std::string& path,
                                                  size_t start, size_t count);

  // Helper: read 2D numeric dataset range
  template <typename T>
  std::vector<std::vector<T>> read2DNumericDatasetRange(const std::string& path,
                                                        size_t start_row,
                                                        size_t row_count);

  // Helper: check if dataset exists
  bool datasetExists(const std::string& path);

  // Helper: check if group exists
  bool groupExists(const std::string& path);

  // Helper: get all dataset names in a group
  std::vector<std::string> getDatasetNames(const std::string& groupPath);

  // Helper: get all group names in a group
  std::vector<std::string> getGroupNames(const std::string& groupPath);

  // Helper: load all string registries from /metadata/registries
  void loadRegistries();

  // Helper to get a cached dataset handle
  H5::DataSet& getDataSet(const std::string& path);

  WorldState world_;

 private:
  H5::H5File file_;
  const Config& config_;

  // Helper: read property dataset range (detects type and returns PropertyValue
  // vector)
  std::vector<PropertyValue> readPropertyDatasetRange(
      const std::string& path, size_t start, size_t count,
      const std::string& prop_name = "");

  std::unordered_map<std::string, H5::DataSet> dataset_cache_;
  std::unordered_map<std::string, H5T_class_t> type_cache_;
};

// =============================================================================
// Implementation
// =============================================================================

inline WorldState HDF5Loader::load(const std::string& filename,
                                   const Config& config) {
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

inline HDF5Loader::HDF5Loader(const std::string& filename, const Config& config)
    : file_(filename, H5F_ACC_RDONLY), config_(config) {}

inline bool HDF5Loader::datasetExists(const std::string& path) {
  if (dataset_cache_.count(path)) return true;
  H5E_auto2_t old_func;
  void* old_data;
  H5Eget_auto(H5E_DEFAULT, &old_func, &old_data);
  H5Eset_auto(H5E_DEFAULT, NULL, NULL);
  try {
    getDataSet(path);
    H5Eset_auto(H5E_DEFAULT, old_func, old_data);
    return true;
  } catch (...) {
    H5Eset_auto(H5E_DEFAULT, old_func, old_data);
    return false;
  }
}

inline H5::DataSet& HDF5Loader::getDataSet(const std::string& path) {
  auto it = dataset_cache_.find(path);
  if (it == dataset_cache_.end()) {
    auto [inserted_it, success] =
        dataset_cache_.emplace(path, file_.openDataSet(path));
    return inserted_it->second;
  }
  return it->second;
}

inline bool HDF5Loader::groupExists(const std::string& path) {
  H5E_auto2_t old_func;
  void* old_data;
  H5Eget_auto(H5E_DEFAULT, &old_func, &old_data);
  H5Eset_auto(H5E_DEFAULT, NULL, NULL);
  try {
    file_.openGroup(path);
    H5Eset_auto(H5E_DEFAULT, old_func, old_data);
    return true;
  } catch (...) {
    H5Eset_auto(H5E_DEFAULT, old_func, old_data);
    return false;
  }
}

inline std::vector<std::string> HDF5Loader::getDatasetNames(
    const std::string& groupPath) {
  std::vector<std::string> names;
  try {
    H5::Group group = file_.openGroup(groupPath);
    hsize_t numObjs = group.getNumObjs();
    for (hsize_t i = 0; i < numObjs; ++i) {
      std::string objName = group.getObjnameByIdx(i);
      H5G_obj_t objType = group.getObjTypeByIdx(i);
      if (objType == H5G_DATASET) {
        names.push_back(objName);
      }
    }
  } catch (...) {
    // Group doesn't exist or error - return empty
  }
  return names;
}

inline std::vector<std::string> HDF5Loader::getGroupNames(
    const std::string& groupPath) {
  std::vector<std::string> names;
  try {
    H5::Group group = file_.openGroup(groupPath);
    hsize_t numObjs = group.getNumObjs();
    for (hsize_t i = 0; i < numObjs; ++i) {
      std::string objName = group.getObjnameByIdx(i);
      H5G_obj_t objType = group.getObjTypeByIdx(i);
      if (objType == H5G_GROUP) {
        names.push_back(objName);
      }
    }
  } catch (...) {
    // Group doesn't exist or error - return empty
  }
  return names;
}

inline std::vector<std::string> HDF5Loader::readStringDataset(
    const std::string& path) {
  H5::DataSet& dataset = getDataSet(path);
  H5::DataSpace dataspace = dataset.getSpace();

  hsize_t dims[1];
  dataspace.getSimpleExtentDims(dims);
  size_t count = dims[0];

  std::vector<std::string> result(count);

  if (count == 0) return result;

  // Handle variable-length strings
  H5::StrType strType = dataset.getStrType();

  if (strType.isVariableStr()) {
    std::vector<char*> rdata(count);
    dataset.read(rdata.data(), strType);

    for (size_t i = 0; i < count; ++i) {
      if (rdata[i]) {
        result[i] = rdata[i];
      }
    }

    // Reclaim memory
    H5::DataSet::vlenReclaim(rdata.data(), strType, dataspace);
  } else {
    // Fixed-length strings
    size_t strSize = strType.getSize();
    std::vector<char> buffer(count * strSize);
    dataset.read(buffer.data(), strType);

    for (size_t i = 0; i < count; ++i) {
      result[i] = std::string(&buffer[i * strSize], strSize);
      // Trim null characters
      size_t pos = result[i].find('\0');
      if (pos != std::string::npos) {
        result[i].resize(pos);
      }
    }
  }

  return result;
}

template <typename T>
inline std::vector<T> HDF5Loader::readNumericDataset(const std::string& path) {
  H5::DataSet& dataset = getDataSet(path);
  H5::DataSpace dataspace = dataset.getSpace();

  hsize_t dims[1];
  dataspace.getSimpleExtentDims(dims);
  size_t count = dims[0];

  std::vector<T> result(count);

  if (count > 0) {
    if constexpr (std::is_same_v<T, int32_t>) {
      dataset.read(result.data(), H5::PredType::NATIVE_INT32);
    } else if constexpr (std::is_same_v<T, float>) {
      dataset.read(result.data(), H5::PredType::NATIVE_FLOAT);
    } else if constexpr (std::is_same_v<T, double>) {
      dataset.read(result.data(), H5::PredType::NATIVE_DOUBLE);
    } else if constexpr (std::is_same_v<T, uint16_t>) {
      dataset.read(result.data(), H5::PredType::NATIVE_UINT16);
    } else if constexpr (std::is_same_v<T, int16_t>) {
      dataset.read(result.data(), H5::PredType::NATIVE_INT16);
    } else if constexpr (std::is_same_v<T, uint8_t>) {
      dataset.read(result.data(), H5::PredType::NATIVE_UINT8);
    } else if constexpr (std::is_same_v<T, int64_t>) {
      dataset.read(result.data(), H5::PredType::NATIVE_INT64);
    } else if constexpr (std::is_same_v<T, uint64_t>) {
      dataset.read(result.data(), H5::PredType::NATIVE_UINT64);
    } else if constexpr (std::is_same_v<T, bool>) {
      // Read as uint8 and convert
      std::vector<uint8_t> temp(count);
      dataset.read(temp.data(), H5::PredType::NATIVE_UINT8);
      for (size_t i = 0; i < count; ++i) {
        result[i] = temp[i] != 0;
      }
    }
  }

  return result;
}

// Read a range of a numeric dataset (hyperslab selection)
template <typename T>
inline std::vector<T> HDF5Loader::readNumericDatasetRange(
    const std::string& path, size_t start, size_t count) {
  H5::DataSet& dataset = getDataSet(path);
  H5::DataSpace dataspace = dataset.getSpace();

  // Define hyperslab (range to read)
  hsize_t offset[1] = {start};
  hsize_t read_count[1] = {count};
  dataspace.selectHyperslab(H5S_SELECT_SET, read_count, offset);

  // Define memory space
  H5::DataSpace memspace(1, read_count);

  std::vector<T> result(count);

  if (count > 0) {
    if constexpr (std::is_same_v<T, int32_t>) {
      dataset.read(result.data(), H5::PredType::NATIVE_INT32, memspace,
                   dataspace);
    } else if constexpr (std::is_same_v<T, float>) {
      dataset.read(result.data(), H5::PredType::NATIVE_FLOAT, memspace,
                   dataspace);
    } else if constexpr (std::is_same_v<T, double>) {
      dataset.read(result.data(), H5::PredType::NATIVE_DOUBLE, memspace,
                   dataspace);
    } else if constexpr (std::is_same_v<T, uint16_t>) {
      dataset.read(result.data(), H5::PredType::NATIVE_UINT16, memspace,
                   dataspace);
    } else if constexpr (std::is_same_v<T, int16_t>) {
      dataset.read(result.data(), H5::PredType::NATIVE_INT16, memspace,
                   dataspace);
    } else if constexpr (std::is_same_v<T, uint8_t>) {
      dataset.read(result.data(), H5::PredType::NATIVE_UINT8, memspace,
                   dataspace);
    } else if constexpr (std::is_same_v<T, int64_t>) {
      dataset.read(result.data(), H5::PredType::NATIVE_INT64, memspace,
                   dataspace);
    } else if constexpr (std::is_same_v<T, uint64_t>) {
      dataset.read(result.data(), H5::PredType::NATIVE_UINT64, memspace,
                   dataspace);
    } else if constexpr (std::is_same_v<T, bool>) {
      std::vector<uint8_t> temp(count);
      dataset.read(temp.data(), H5::PredType::NATIVE_UINT8, memspace,
                   dataspace);
      for (size_t i = 0; i < count; ++i) {
        result[i] = temp[i] != 0;
      }
    }
  }

  return result;
}

// Read a range of a string dataset
inline std::vector<std::string> HDF5Loader::readStringDatasetRange(
    const std::string& path, size_t start, size_t count) {
  H5::DataSet& dataset = getDataSet(path);
  H5::DataSpace dataspace = dataset.getSpace();

  // Define hyperslab
  hsize_t offset[1] = {start};
  hsize_t read_count[1] = {count};
  dataspace.selectHyperslab(H5S_SELECT_SET, read_count, offset);

  // Define memory space
  H5::DataSpace memspace(1, read_count);

  std::vector<std::string> result(count);

  if (count == 0) return result;

  H5::StrType strType = dataset.getStrType();

  if (strType.isVariableStr()) {
    std::vector<char*> rdata(count);
    dataset.read(rdata.data(), strType, memspace, dataspace);

    for (size_t i = 0; i < count; ++i) {
      if (rdata[i]) {
        result[i] = rdata[i];
      }
    }

    // Reclaim memory
    H5::DataSet::vlenReclaim(rdata.data(), strType, memspace);
  } else {
    // Fixed-length strings
    size_t strSize = strType.getSize();
    std::vector<char> buffer(count * strSize);
    dataset.read(buffer.data(), strType, memspace, dataspace);

    for (size_t i = 0; i < count; ++i) {
      result[i] = std::string(&buffer[i * strSize], strSize);
      // Trim null characters
      size_t pos = result[i].find('\0');
      if (pos != std::string::npos) {
        result[i].resize(pos);
      }
    }
  }

  return result;
}

// Read a range of rows from a 2D numeric dataset
template <typename T>
inline std::vector<std::vector<T>> HDF5Loader::read2DNumericDatasetRange(
    const std::string& path, size_t start_row, size_t row_count) {
  H5::DataSet& dataset = getDataSet(path);
  H5::DataSpace dataspace = dataset.getSpace();

  hsize_t dims[2];
  dataspace.getSimpleExtentDims(dims);
  size_t total_rows = dims[0];
  size_t cols = dims[1];

  if (start_row >= total_rows || row_count == 0) {
    return std::vector<std::vector<T>>();
  }

  if (start_row + row_count > total_rows) {
    row_count = total_rows - start_row;
  }

  hsize_t offset[2] = {start_row, 0};
  hsize_t count[2] = {row_count, cols};
  dataspace.selectHyperslab(H5S_SELECT_SET, count, offset);
  H5::DataSpace memspace(2, count);

  std::vector<T> flat(row_count * cols);
  H5::DataType h5_type;
  if constexpr (std::is_same_v<T, int32_t>)
    h5_type = H5::PredType::NATIVE_INT32;
  else if constexpr (std::is_same_v<T, float>)
    h5_type = H5::PredType::NATIVE_FLOAT;
  else if constexpr (std::is_same_v<T, double>)
    h5_type = H5::PredType::NATIVE_DOUBLE;
  else
    h5_type = dataset.getDataType();

  dataset.read(flat.data(), h5_type, memspace, dataspace);

  // Fast repack into 2D structure
  std::vector<std::vector<T>> result(row_count, std::vector<T>(cols));
  for (size_t i = 0; i < row_count; ++i) {
    std::copy(flat.begin() + i * cols, flat.begin() + (i + 1) * cols,
              result[i].begin());
  }
  return result;
}

// Read property dataset range with type detection
inline std::vector<PropertyValue> HDF5Loader::readPropertyDatasetRange(
    const std::string& path, size_t start, size_t count,
    const std::string& prop_name) {
  std::vector<PropertyValue> result(count);
  if (count == 0) return result;

  try {
    H5T_class_t type_class;
    auto type_it = type_cache_.find(path);
    if (type_it == type_cache_.end()) {
      H5::DataSet& dataset = getDataSet(path);
      type_class = dataset.getTypeClass();
      type_cache_[path] = type_class;
    } else {
      type_class = type_it->second;
    }

    if (type_class == H5T_INTEGER) {
      // Read as int32 and keep as integer code
      auto ints = readNumericDatasetRange<int32_t>(path, start, count);

      // Ensure integer codes are within registry bounds
      if (!prop_name.empty() && !ints.empty()) {
        const std::vector<std::string>* registry = nullptr;
        if (path.find("/population/") != std::string::npos) {
          if (world_.person_property_value_registries.count(prop_name))
            registry = &world_.person_property_value_registries.at(prop_name);
        } else if (path.find("/venues/") != std::string::npos) {
          if (world_.venue_property_value_registries.count(prop_name))
            registry = &world_.venue_property_value_registries.at(prop_name);
        } else if (path.find("/geography/") != std::string::npos) {
          if (world_.geo_unit_property_value_registries.count(prop_name))
            registry = &world_.geo_unit_property_value_registries.at(prop_name);
        }

        if (registry) {
          auto [min_it, max_it] = std::minmax_element(ints.begin(), ints.end());
          if (*max_it >= (int32_t)registry->size() || *min_it < -1) {
            std::cerr << "WARNING: Property '" << prop_name << "' in " << path
                      << " has out-of-range codes (max=" << *max_it
                      << ", reg_size=" << registry->size() << ")" << std::endl;

            // Reliability fallback: mark invalid codes as -1 (null)
            for (auto& val : ints) {
              if (val >= (int32_t)registry->size() || val < -1) val = -1;
            }
          }
        }
      }

      for (size_t i = 0; i < count; ++i) {
        result[i] = ints[i];
      }
    } else if (type_class == H5T_FLOAT) {
      // Read as float
      auto floats = readNumericDatasetRange<float>(path, start, count);
      for (size_t i = 0; i < count; ++i) {
        result[i] = floats[i];
      }
    } else {
      // Default to string
      try {
        auto strings = readStringDatasetRange(path, start, count);
        for (size_t i = 0; i < count; ++i) {
          result[i] = strings[i];
        }
      } catch (const std::exception& e) {
        std::cerr << "ERROR: Failed to read string property " << path
                  << " (start=" << start << ", count=" << count
                  << "): " << e.what() << std::endl;
        throw;
      } catch (...) {
        std::cerr << "ERROR: Failed to read string property " << path
                  << " (start=" << start << ", count=" << count << ")"
                  << std::endl;
        throw;
      }
    }
  } catch (...) {
    std::cerr << "Error reading property " << path << std::endl;
  }

  return result;
}

inline void HDF5Loader::loadRegistries() {
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

inline void HDF5Loader::loadGeography() {
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

// =============================================================================
// Load Geography Only (lightweight, for non-zero ranks)
// =============================================================================

inline WorldState HDF5Loader::loadGeographyOnly(const std::string& filename) {
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

// =============================================================================
// Load Person Metadata in Chunks
// =============================================================================

template <typename Callback>
inline void HDF5Loader::loadPersonMetadataChunked(const std::string& filename,
                                                  size_t chunk_size,
                                                  Callback callback) {
  H5::H5File file(filename, H5F_ACC_RDONLY);

  // Open datasets
  H5::DataSet ids_dataset = file.openDataSet("/population/ids");
  H5::DataSet geo_dataset = file.openDataSet("/population/geo_unit_ids");

  // Get total count
  H5::DataSpace ids_space = ids_dataset.getSpace();
  hsize_t dims[1];
  ids_space.getSimpleExtentDims(dims);
  size_t total_count = dims[0];


  // Process in chunks
  for (size_t offset = 0; offset < total_count; offset += chunk_size) {
    size_t current_chunk_size = std::min(chunk_size, total_count - offset);

    // Read chunk using hyperslab selection
    hsize_t h_offset[1] = {offset};
    hsize_t h_count[1] = {current_chunk_size};

    // Read person IDs
    H5::DataSpace ids_file_space = ids_dataset.getSpace();
    ids_file_space.selectHyperslab(H5S_SELECT_SET, h_count, h_offset);
    H5::DataSpace ids_mem_space(1, h_count);

    std::vector<PersonId> ids(current_chunk_size);
    ids_dataset.read(ids.data(), H5::PredType::NATIVE_INT32, ids_mem_space,
                     ids_file_space);

    // Read geo_unit IDs
    H5::DataSpace geo_file_space = geo_dataset.getSpace();
    geo_file_space.selectHyperslab(H5S_SELECT_SET, h_count, h_offset);
    H5::DataSpace geo_mem_space(1, h_count);

    std::vector<GeoUnitId> geo_ids(current_chunk_size);
    geo_dataset.read(geo_ids.data(), H5::PredType::NATIVE_INT32, geo_mem_space,
                     geo_file_space);

    // Pack into PersonMetadata
    std::vector<PersonMetadata> chunk_metadata(current_chunk_size);
    for (size_t i = 0; i < current_chunk_size; ++i) {
      chunk_metadata[i].person_id = ids[i];
      chunk_metadata[i].geo_unit_id = geo_ids[i];
    }

    // Call callback with this chunk
    callback(chunk_metadata);

  }

}
// =============================================================================
// Load Domain Data with Chunked Loading
// =============================================================================

inline WorldState HDF5Loader::loadDomainChunked(
    const std::string& filename,
    const std::unordered_set<GeoUnitId>& owned_geo_units, size_t chunk_size,
    const Config& config) {
  HDF5Loader loader(filename, config);

  int mpi_rank = 0;
#ifdef USE_MPI
  MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);
#endif

  // Load string registries first
  loader.loadRegistries();

  // Load geography (needed for hierarchy traversal and level name resolution)
  loader.loadGeography();

  // Convert owned_geo_units to vector for chunking
  std::vector<GeoUnitId> geo_units_vec(owned_geo_units.begin(),
                                       owned_geo_units.end());
  std::sort(geo_units_vec.begin(), geo_units_vec.end());
  size_t num_geo_units = geo_units_vec.size();
  size_t num_chunks = (num_geo_units + chunk_size - 1) / chunk_size;

  // Cache for interning property values
  std::unordered_map<std::string, std::unordered_map<std::string, int32_t>>
      property_indices_cache;
  std::unordered_map<std::string, std::unordered_map<std::string, int32_t>>
      venue_property_indices_cache;

  // Pre-reserve major vectors to avoid reallocations
  if (loader.datasetExists("/metadata/registries/population_counts")) {
    auto counts = loader.readNumericDataset<int32_t>(
        "/metadata/registries/population_counts");
    size_t total_p = 0;
    for (int c : counts) total_p += c;
    if (total_p > 0) loader.world_.people.reserve(total_p);
  }


  // Convert partition indexes to maps
  auto read_partition_map = [&](const std::string& path_prefix) {
    auto gu_ids =
        loader.readNumericDataset<int32_t>(path_prefix + "/geo_unit_ids");
    auto starts =
        loader.readNumericDataset<int32_t>(path_prefix + "/start_indices");
    auto counts = loader.readNumericDataset<int32_t>(path_prefix + "/counts");

    std::unordered_map<GeoUnitId, std::pair<size_t, size_t>> map;
    for (size_t i = 0; i < gu_ids.size(); ++i) {
      map[gu_ids[i]] = {(size_t)starts[i], (size_t)counts[i]};
    }
    return map;
  };

  auto pop_partition_map = read_partition_map("/population/partition_index");
  auto venue_partition_map = read_partition_map("/venues/partition_index");
  auto rel_partition_map =
      read_partition_map("/activity_mappings/activity_map/partition_index");

  // Load activity names (needed for activity mappings)
  loader.world_.activity_names = loader.readStringDataset(
      "/activity_mappings/activity_map/activity_names");

  // Add activities from configuration that might be missing in HDF5 (e.g.
  // "sex")
  auto register_activity = [&](const std::string& name) {
    if (!name.empty() && std::find(loader.world_.activity_names.begin(),
                                   loader.world_.activity_names.end(), name) ==
                             loader.world_.activity_names.end()) {
      loader.world_.activity_names.push_back(name);
    }
  };

  // Add internal system activities
  register_activity("dead");
  register_activity("none");
  register_activity("visiting");
  register_activity("no_venue");

  for (const auto& sched_type : config.schedule.schedule_types) {
    for (const auto& [dt_name, dt_slots] : sched_type.slots_by_day_type) {
      for (const auto& slot : dt_slots) {
        for (const auto& act : slot.allowed_activities) register_activity(act);
        for (const auto& act : slot.coordinated_only_activities)
          register_activity(act);
      }
    }
  }

  for (const auto& st : config.schedule.schedule_types) {
    if (std::find(loader.world_.schedule_type_names.begin(),
                  loader.world_.schedule_type_names.end(),
                  st.name) == loader.world_.schedule_type_names.end()) {
      loader.world_.schedule_type_names.push_back(st.name);
    }
  }

  // Discover all population property datasets dynamically
  std::vector<std::string> population_property_names =
      loader.getDatasetNames("/population/properties");


  // Register property names in the static registry
  loader.world_.person_property_names = population_property_names;

  // Discover property names only. We will load them lazily per geo-unit.
  std::string venue_prop_base = "/venues/properties";
  std::unordered_map<std::string, std::vector<std::string>>
      venue_type_prop_names;
  if (loader.groupExists(venue_prop_base)) {
    auto prop_types = loader.getGroupNames(venue_prop_base);
    for (const auto& v_type : prop_types) {
      venue_type_prop_names[v_type] =
          loader.getDatasetNames(venue_prop_base + "/" + v_type);
    }
  }

  // Map to cache person indices incrementally across chunks
  std::unordered_map<PersonId, size_t> local_person_idx_map;
  if (loader.world_.people.capacity() > 0)
    local_person_idx_map.reserve(loader.world_.people.capacity());

  // Process each chunk
  for (size_t chunk_idx = 0; chunk_idx < num_chunks; ++chunk_idx) {
    size_t start_idx = chunk_idx * chunk_size;
    size_t end_idx = std::min(start_idx + chunk_size, num_geo_units);
    size_t people_before_chunk = loader.world_.people.size();

    MemoryUtils::logMemory("Before GeoUnit Chunk " +
                           std::to_string(chunk_idx + 1));

    // Detect contiguous spans for population
    struct PopSpan {
      size_t start;
      size_t count;
      std::vector<size_t> gu_indices;
    };
    std::vector<PopSpan> pop_spans;
    for (size_t i = start_idx; i < end_idx; ++i) {
      GeoUnitId gu = geo_units_vec[i];
      if (pop_partition_map.count(gu)) {
        auto [start, count] = pop_partition_map.at(gu);
        if (count > 0) {
          if (!pop_spans.empty() &&
              start == pop_spans.back().start + pop_spans.back().count) {
            pop_spans.back().count += count;
            pop_spans.back().gu_indices.push_back(i);
          } else {
            pop_spans.push_back({start, count, {i}});
          }
        }
      }
    }
    // Initialize network names registry
    loader.world_.network_names = population_property_names;

    for (const auto& span : pop_spans) {
      auto chunk_ids = loader.readNumericDatasetRange<int32_t>(
          "/population/ids", span.start, span.count);
      auto chunk_ages = loader.readNumericDatasetRange<float>(
          "/population/ages", span.start, span.count);
      auto chunk_sexes = loader.readNumericDatasetRange<uint8_t>(
          "/population/sexes", span.start, span.count);

      std::vector<std::vector<PropertyValue>> chunk_property_columns;
      for (const auto& prop_name : population_property_names) {
        chunk_property_columns.push_back(loader.readPropertyDatasetRange(
            "/population/properties/" + prop_name, span.start, span.count,
            prop_name));
      }

      size_t local_read_offset = 0;
      for (size_t gu_idx : span.gu_indices) {
        GeoUnitId geo_unit = geo_units_vec[gu_idx];
        size_t gu_count = pop_partition_map.at(geo_unit).second;

        for (size_t j = 0; j < gu_count; ++j) {
          Person p;
          size_t j_off = local_read_offset + j;
          p.id = chunk_ids[j_off];
          p.age = chunk_ages[j_off];
          p.sex = static_cast<Sex>(chunk_sexes[j_off]);
          p.geo_unit_id = geo_unit;

          // Dynamic properties (Phase 3: Flat Storage & Interning)
          p.properties_start =
              static_cast<uint32_t>(loader.world_.person_properties.size());
          p.properties_count =
              static_cast<uint8_t>(chunk_property_columns.size());
          for (size_t k = 0; k < chunk_property_columns.size(); ++k) {
            const auto& prop_name = population_property_names[k];
            const auto& prop_val = chunk_property_columns[k][j_off];

            int32_t interned_val = -1;
            if (std::holds_alternative<int32_t>(prop_val)) {
              interned_val = std::get<int32_t>(prop_val);
            } else if (std::holds_alternative<double>(prop_val)) {
              interned_val = static_cast<int32_t>(std::get<double>(prop_val));
            } else if (std::holds_alternative<bool>(prop_val)) {
              interned_val = std::get<bool>(prop_val) ? 1 : 0;
            } else if (std::holds_alternative<std::string>(prop_val)) {
              const std::string& s = std::get<std::string>(prop_val);
              if (!s.empty() && (s[0] == '[' || s[0] == '{')) {
                // Handle as network below
                interned_val = -1;
              } else if (!s.empty()) {
                // Categorical interning
                auto& registry =
                    loader.world_.person_property_value_registries[prop_name];
                auto& index_cache = property_indices_cache[prop_name];

                // Initialize cache if needed (e.g. if loaded from previous
                // ranks or chunks)
                if (index_cache.empty() && !registry.empty()) {
                  for (size_t r_idx = 0; r_idx < registry.size(); ++r_idx) {
                    index_cache[registry[r_idx]] = static_cast<int32_t>(r_idx);
                  }
                }

                auto it_idx = index_cache.find(s);
                if (it_idx == index_cache.end()) {
                  interned_val = static_cast<int32_t>(registry.size());
                  registry.push_back(s);
                  index_cache[s] = interned_val;
                } else {
                  interned_val = it_idx->second;
                }
              }
            }
            loader.world_.person_properties.push_back(interned_val);
          }

          // Network parsing
          p.network_meta_start =
              static_cast<uint32_t>(loader.world_.network_meta.size());
          for (size_t k = 0; k < population_property_names.size(); ++k) {
            const auto& prop_val = chunk_property_columns[k][j_off];
            if (std::holds_alternative<std::string>(prop_val)) {
              const std::string& s = std::get<std::string>(prop_val);
              if (!s.empty() && (s[0] == '[' || s[0] == '{')) {
                uint32_t partner_start = static_cast<uint32_t>(
                    loader.world_.network_partners.size());
                int32_t current_id = 0;
                bool has_id = false;
                for (char c : s) {
                  if (c >= '0' && c <= '9') {
                    current_id = current_id * 10 + (c - '0');
                    has_id = true;
                  } else if (has_id) {
                    loader.world_.network_partners.push_back(current_id);
                    current_id = 0;
                    has_id = false;
                  }
                }
                if (has_id)
                  loader.world_.network_partners.push_back(current_id);

                uint32_t partner_count =
                    static_cast<uint32_t>(
                        loader.world_.network_partners.size()) -
                    partner_start;
                if (partner_count > 0) {
                  Person::NetworkMeta meta;
                  meta.network_type_id = static_cast<uint16_t>(
                      k);  // Match population_property_names index
                  meta.partner_start = partner_start;
                  meta.partner_count = partner_count;
                  loader.world_.network_meta.push_back(meta);
                  p.network_meta_count++;
                }
              }
            }
          }

          loader.world_.people.push_back(std::move(p));
        }
        local_read_offset += gu_count;
      }
    }

    // Detect contiguous spans for venues
    struct VenueSpan {
      size_t start;
      size_t count;
      std::vector<size_t> gu_indices;
    };
    std::vector<VenueSpan> venue_spans;
    for (size_t i = start_idx; i < end_idx; ++i) {
      GeoUnitId gu = geo_units_vec[i];
      if (venue_partition_map.count(gu)) {
        auto [start, count] = venue_partition_map.at(gu);
        if (count > 0) {
          if (!venue_spans.empty() &&
              start == venue_spans.back().start + venue_spans.back().count) {
            venue_spans.back().count += count;
            venue_spans.back().gu_indices.push_back(i);
          } else {
            venue_spans.push_back({start, count, {i}});
          }
        }
      }
    }

    for (const auto& span : venue_spans) {
      auto chunk_ids = loader.readNumericDatasetRange<int32_t>(
          "/venues/ids", span.start, span.count);
      auto chunk_type_ids = loader.readNumericDatasetRange<uint8_t>(
          "/venues/types", span.start, span.count);
      auto chunk_ranks_in_type = loader.readNumericDatasetRange<int32_t>(
          "/venues/ranks_in_type", span.start, span.count);
      auto chunk_parent_ids = loader.readNumericDatasetRange<int32_t>(
          "/venues/parent_ids", span.start, span.count);

      std::vector<float> chunk_latitudes, chunk_longitudes;
      if (loader.datasetExists("/venues/latitudes")) {
        chunk_latitudes = loader.readNumericDatasetRange<float>(
            "/venues/latitudes", span.start, span.count);
        chunk_longitudes = loader.readNumericDatasetRange<float>(
            "/venues/longitudes", span.start, span.count);
      }

      std::vector<uint8_t> chunk_is_residence_raw;
      if (loader.datasetExists("/venues/is_residence")) {
        chunk_is_residence_raw = loader.readNumericDatasetRange<uint8_t>(
            "/venues/is_residence", span.start, span.count);
      }

      std::vector<Venue> span_venues;
      span_venues.reserve(span.count);
      std::unordered_map<uint8_t, std::vector<std::pair<size_t, int32_t>>>
          venues_by_type_in_span;

      size_t local_read_offset = 0;
      for (size_t gu_idx : span.gu_indices) {
        GeoUnitId geo_unit = geo_units_vec[gu_idx];
        size_t gu_count = venue_partition_map.at(geo_unit).second;

        for (size_t j = 0; j < gu_count; ++j) {
          size_t v_idx = span_venues.size();
          Venue v;
          size_t j_off = local_read_offset + j;
          v.id = chunk_ids[j_off];
          v.type_id = chunk_type_ids[j_off];
          v.geo_unit_id = geo_unit;
          v.parent_id = chunk_parent_ids[j_off];
          v.latitude = chunk_latitudes.empty() ? 0.0f : chunk_latitudes[j_off];
          v.longitude =
              chunk_longitudes.empty() ? 0.0f : chunk_longitudes[j_off];
          v.is_residence = chunk_is_residence_raw.empty()
                               ? false
                               : (chunk_is_residence_raw[j_off] != 0);

          const std::string& type_name =
              loader.world_.venue_type_names[v.type_id];
          size_t prop_count = venue_type_prop_names.count(type_name)
                                  ? venue_type_prop_names.at(type_name).size()
                                  : 0;

          v.properties_start =
              static_cast<uint32_t>(loader.world_.venue_properties.size());
          v.properties_count = static_cast<uint16_t>(prop_count);

          // Pre-allocate space in flat vector
          loader.world_.venue_properties.resize(v.properties_start + prop_count,
                                                -1);

          int32_t global_rank = chunk_ranks_in_type[j_off];
          venues_by_type_in_span[v.type_id].push_back({v_idx, global_rank});
          span_venues.push_back(std::move(v));
        }
        local_read_offset += gu_count;
      }

      // Batch load properties per type for this span using SUB-SPANS
      for (auto const& [type_id, venue_infos] : venues_by_type_in_span) {
        const std::string& type_name = loader.world_.venue_type_names[type_id];
        if (venue_type_prop_names.count(type_name)) {
          const auto& prop_names = venue_type_prop_names.at(type_name);

          auto sorted_infos = venue_infos;
          std::sort(
              sorted_infos.begin(), sorted_infos.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });

          struct PropSpan {
            int32_t start_r;
            size_t count;
            std::vector<size_t> internal_indices;
          };
          std::vector<PropSpan> prop_spans;
          for (const auto& info : sorted_infos) {
            if (!prop_spans.empty() &&
                info.second == prop_spans.back().start_r +
                                   (int32_t)prop_spans.back().count) {
              prop_spans.back().count++;
              prop_spans.back().internal_indices.push_back(info.first);
            } else {
              prop_spans.push_back({info.second, 1, {info.first}});
            }
          }

          for (size_t p_idx = 0; p_idx < prop_names.size(); ++p_idx) {
            const std::string& p_name = prop_names[p_idx];
            std::string p_path =
                "/venues/properties/" + type_name + "/" + p_name;

            for (const auto& span_p : prop_spans) {
              auto p_vals = loader.readPropertyDatasetRange(
                  p_path, span_p.start_r, span_p.count, p_name);
              for (size_t k = 0; k < span_p.count; ++k) {
                if (k < p_vals.size() &&
                    !std::holds_alternative<std::monostate>(p_vals[k])) {
                  int32_t interned_val = -1;
                  const auto& val = p_vals[k];

                  if (std::holds_alternative<int32_t>(val)) {
                    interned_val = std::get<int32_t>(val);
                  } else if (std::holds_alternative<double>(val)) {
                    interned_val = static_cast<int32_t>(std::get<double>(val));
                  } else if (std::holds_alternative<bool>(val)) {
                    interned_val = std::get<bool>(val) ? 1 : 0;
                  } else if (std::holds_alternative<std::string>(val)) {
                    const std::string& s = std::get<std::string>(val);
                    auto& registry =
                        loader.world_.venue_property_value_registries[p_name];
                    auto& cache = venue_property_indices_cache[p_name];

                    if (cache.empty() && !registry.empty()) {
                      for (size_t r = 0; r < registry.size(); ++r)
                        cache[registry[r]] = r;
                    }

                    auto it = cache.find(s);
                    if (it == cache.end()) {
                      interned_val = registry.size();
                      registry.push_back(s);
                      cache[s] = interned_val;
                    } else {
                      interned_val = it->second;
                    }
                  }

                  size_t global_v_idx = span_p.internal_indices[k];
                  uint32_t flat_idx =
                      span_venues[global_v_idx].properties_start + p_idx;
                  loader.world_.venue_properties[flat_idx] = interned_val;
                }
              }
            }
          }
        }
      }
      for (auto& v : span_venues) loader.world_.venues.push_back(std::move(v));
    }

    // Build a local index for this chunk's people
    // Only index the newly added people from this chunk
    for (size_t idx = people_before_chunk; idx < loader.world_.people.size();
         ++idx) {
      local_person_idx_map[loader.world_.people[idx].id] = idx;
    }

    // Detect contiguous spans for activity mappings
    if (loader.groupExists("/activity_mappings/activity_map")) {
      struct RelSpan {
        size_t start;
        size_t count;
        std::vector<size_t> gu_indices;
      };
      std::vector<RelSpan> rel_spans;
      for (size_t i = start_idx; i < end_idx; ++i) {
        GeoUnitId gu = geo_units_vec[i];
        if (rel_partition_map.count(gu)) {
          auto [start, count] = rel_partition_map.at(gu);
          if (count > 0) {
            if (!rel_spans.empty() &&
                start == rel_spans.back().start + rel_spans.back().count) {
              rel_spans.back().count += count;
              rel_spans.back().gu_indices.push_back(i);
            } else {
              rel_spans.push_back({start, count, {i}});
            }
          }
        }
      }

      for (const auto& span : rel_spans) {
        auto span_activity_data = loader.read2DNumericDatasetRange<int32_t>(
            "/activity_mappings/activity_map/activity_data", span.start,
            span.count);

        Person* last_p = nullptr;
        int16_t last_act_idx = -1;

        for (size_t row_idx = 0; row_idx < span.count; ++row_idx) {
          const auto& row = span_activity_data[row_idx];
          PersonId pid = row[0];
          int16_t act_idx = static_cast<int16_t>(row[1]);

          auto it = local_person_idx_map.find(pid);
          if (it == local_person_idx_map.end()) continue;
          Person* p = &loader.world_.people[it->second];

          if (p != last_p) {
            p->activity_meta_start =
                static_cast<uint32_t>(loader.world_.activity_meta.size());
            p->activity_meta_count = 0;
            last_p = p;
            last_act_idx = -1;
          }

          if (act_idx != last_act_idx) {
            Person::ActivityMeta ameta;
            ameta.activity_index = act_idx;
            ameta.venue_start =
                static_cast<uint32_t>(loader.world_.activity_venues.size());
            ameta.venue_count = 0;
            loader.world_.activity_meta.push_back(ameta);
            p->activity_meta_count++;
            last_act_idx = act_idx;
          }

          loader.world_.activity_venues.push_back({row[2], row[3]});
          loader.world_.activity_meta.back().venue_count++;
        }
      }
    }

  }

  // Build venue index before loading subsets so getVenue() works
  for (size_t i = 0; i < loader.world_.venues.size(); ++i) {
    loader.world_.venue_index[loader.world_.venues[i].id] = i;
  }

  // Load subsets (after all venues are loaded)
  if (loader.groupExists("/venues/subsets") &&
      loader.groupExists("/venues/subsets/partition_index") &&
      loader.groupExists("/venues/subsets/members_partition_index")) {

    // Load both partition indexes
    auto subset_meta_geo_units = loader.readNumericDataset<int32_t>(
        "/venues/subsets/partition_index/geo_unit_ids");
    auto subset_meta_starts = loader.readNumericDataset<int32_t>(
        "/venues/subsets/partition_index/start_indices");
    auto subset_meta_counts = loader.readNumericDataset<int32_t>(
        "/venues/subsets/partition_index/counts");

    auto member_geo_units = loader.readNumericDataset<int32_t>(
        "/venues/subsets/members_partition_index/geo_unit_ids");
    auto member_starts = loader.readNumericDataset<int64_t>(
        "/venues/subsets/members_partition_index/start_indices");
    auto member_counts = loader.readNumericDataset<int32_t>(
        "/venues/subsets/members_partition_index/counts");

    // Local storage to gather subsets before sorting and flattening
    std::vector<Subset> temp_subsets;
    std::unordered_map<std::string, uint16_t> subset_type_cache;
    if (!loader.world_.subset_type_names.empty()) {
      for (size_t i = 0; i < loader.world_.subset_type_names.size(); ++i)
        subset_type_cache[loader.world_.subset_type_names[i]] = i;
    }

    int loaded_subsets = 0;
    int loaded_members = 0;

    // For each owned geo_unit, load its subsets
    for (GeoUnitId geo_unit : owned_geo_units) {
      auto meta_it = std::find(subset_meta_geo_units.begin(),
                               subset_meta_geo_units.end(), geo_unit);
      if (meta_it == subset_meta_geo_units.end()) continue;

      size_t meta_idx = std::distance(subset_meta_geo_units.begin(), meta_it);
      size_t subset_start_hdf5 = subset_meta_starts[meta_idx];
      size_t subset_count_hdf5 = subset_meta_counts[meta_idx];
      if (subset_count_hdf5 == 0) continue;

      auto v_ids = loader.readNumericDatasetRange<int32_t>(
          "/venues/subsets/venue_ids", subset_start_hdf5, subset_count_hdf5);
      auto s_indices = loader.readNumericDatasetRange<int32_t>(
          "/venues/subsets/subset_indices", subset_start_hdf5,
          subset_count_hdf5);
      auto m_counts = loader.readNumericDatasetRange<int32_t>(
          "/venues/subsets/member_counts", subset_start_hdf5,
          subset_count_hdf5);
      auto m_offsets = loader.readNumericDatasetRange<int64_t>(
          "/venues/subsets/members_offsets", subset_start_hdf5,
          subset_count_hdf5);

      std::vector<std::string> s_names;
      if (loader.datasetExists("/metadata/names/subsets")) {
        s_names = loader.readStringDatasetRange(
            "/metadata/names/subsets", subset_start_hdf5, subset_count_hdf5);
      } else {
        s_names =
            loader.readStringDatasetRange("/venues/subsets/subset_names",
                                          subset_start_hdf5, subset_count_hdf5);
      }

      // Find member data range for this geo_unit
      auto member_it =
          std::find(member_geo_units.begin(), member_geo_units.end(), geo_unit);
      if (member_it == member_geo_units.end()) continue;

      size_t m_idx = std::distance(member_geo_units.begin(), member_it);
      int64_t m_base_offset = member_starts[m_idx];
      size_t m_total_count = member_counts[m_idx];

      auto all_members = loader.readNumericDatasetRange<int32_t>(
          "/venues/subsets/members_flat", m_base_offset, m_total_count);

      for (size_t i = 0; i < subset_count_hdf5; ++i) {
        Subset s;
        s.venue_id = v_ids[i];
        s.subset_index = s_indices[i];

        // Intern subset type
        const std::string& name = s_names[i];
        auto it_type = subset_type_cache.find(name);
        if (it_type == subset_type_cache.end()) {
          s.subset_type_id = loader.world_.subset_type_names.size();
          loader.world_.subset_type_names.push_back(name);
          subset_type_cache[name] = s.subset_type_id;
        } else {
          s.subset_type_id = it_type->second;
        }

        s.member_count = m_counts[i];
        if (s.member_count > 0) {
          s.member_start = loader.world_.subset_members.size();
          int64_t local_off = m_offsets[i] - m_base_offset;
          loader.world_.subset_members.insert(
              loader.world_.subset_members.end(),
              all_members.begin() + local_off,
              all_members.begin() + local_off + s.member_count);
          loaded_members += s.member_count;
        }

        temp_subsets.push_back(s);
        loaded_subsets++;
      }
    }

    // Sort subsets by venue_id to ensure they can be grouped contiguously
    std::sort(temp_subsets.begin(), temp_subsets.end(),
              [](const Subset& a, const Subset& b) {
                if (a.venue_id != b.venue_id) return a.venue_id < b.venue_id;
                return a.subset_index < b.subset_index;
              });

    // Write to world_.subsets and update world_.venues
    size_t s_i = 0;
    while (s_i < temp_subsets.size()) {
      VenueId vid = temp_subsets[s_i].venue_id;
      size_t start_s = s_i;

      Venue* venue = loader.world_.getVenue(vid);
      if (venue) {
        venue->subset_start = loader.world_.subsets.size();
        while (s_i < temp_subsets.size() && temp_subsets[s_i].venue_id == vid) {
          loader.world_.subsets.push_back(temp_subsets[s_i]);
          s_i++;
        }
        venue->subset_count =
            loader.world_.subsets.size() - venue->subset_start;
      } else {
        // If venue not owned but subsets exist (unlikely given partitioning)
        while (s_i < temp_subsets.size() && temp_subsets[s_i].venue_id == vid)
          s_i++;
      }
    }

  }

  // Build global venue type map: read ALL venue IDs and type_ids from HDF5.
  // This is needed for cross-domain venue lookups in selectVenue() when
  // running in MPI mode — venues owned by other ranks are not in world.venues,
  // but activity mappings may reference them. ~300K entries × 5 bytes ≈ 1.5MB.
  {
    auto all_venue_ids = loader.readNumericDataset<int32_t>("/venues/ids");
    auto all_venue_types = loader.readNumericDataset<uint8_t>("/venues/types");
    size_t n = std::min(all_venue_ids.size(), all_venue_types.size());
    loader.world_.global_venue_type_map.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      loader.world_.global_venue_type_map[all_venue_ids[i]] =
          all_venue_types[i];
    }
  }

  MemoryUtils::logMemory("After loading all chunks, before indexing");
  loader.world_.buildIndices();
  MemoryUtils::logMemory("After indexing final state");

  if (mpi_rank == 0) {
    std::cout << "  Domain loaded: " << loader.world_.people.size() << " people, "
              << loader.world_.venues.size() << " venues" << std::endl;
  }

  return std::move(loader.world_);
}

}  // namespace june
