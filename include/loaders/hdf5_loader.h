#pragma once

#include <H5Cpp.h>

#include <string>
#include <vector>
#ifdef USE_MPI
#include <mpi.h>
#endif
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

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

  // Helper: read property dataset range (detects type and returns PropertyValue
  // vector). Peer of the other read* helpers — public so that file-local
  // loaders in hdf5_loader.cpp can use it.
  std::vector<PropertyValue> readPropertyDatasetRange(
      const std::string& path, size_t start, size_t count,
      const std::string& prop_name = "");

  WorldState world_;

 private:
  H5::H5File file_;
  const Config& config_;

  std::unordered_map<std::string, H5::DataSet> dataset_cache_;
  std::unordered_map<std::string, H5T_class_t> type_cache_;
};

// =============================================================================
// Template implementations (must be in header so callers can instantiate)
// =============================================================================

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

}  // namespace june
