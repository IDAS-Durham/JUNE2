// Low-level HDF5 plumbing for HDF5Loader: dataset/group existence and
// listing, raw string and property dataset reads, and the dataset-handle
// cache. WorldState-building methods (load, loadRegistries, loadGeography,
// loadGeographyOnly, loadDomainChunked) live in domain_loader.cpp; the
// per-chunk and per-section helpers they orchestrate live in
// domain_loader_internals.{h,cpp}.

#include "loaders/hdf5_loader.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace june {

HDF5Loader::HDF5Loader(const std::string& filename, const Config& config)
    : file_(filename, H5F_ACC_RDONLY), config_(config) {}

bool HDF5Loader::datasetExists(const std::string& path) {
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

H5::DataSet& HDF5Loader::getDataSet(const std::string& path) {
  auto it = dataset_cache_.find(path);
  if (it == dataset_cache_.end()) {
    auto [inserted_it, success] =
        dataset_cache_.emplace(path, file_.openDataSet(path));
    return inserted_it->second;
  }
  return it->second;
}

bool HDF5Loader::groupExists(const std::string& path) {
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

std::vector<std::string> HDF5Loader::getDatasetNames(
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

std::vector<std::string> HDF5Loader::getGroupNames(
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

std::vector<std::string> HDF5Loader::readStringDataset(
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

std::vector<std::string> HDF5Loader::readStringDatasetRange(
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

std::vector<PropertyValue> HDF5Loader::readPropertyDatasetRange(
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

}  // namespace june
