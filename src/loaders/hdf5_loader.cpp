// Low-level HDF5 plumbing for HDF5Loader: dataset/group existence and
// listing, raw string and property dataset reads, and the dataset-handle
// cache. WorldState-building methods (load, loadRegistries, loadGeography,
// loadGeographyOnly, loadDomainChunked) live in domain_loader.cpp; the
// per-chunk and per-section helpers they orchestrate live in
// domain_loader_internals.{h,cpp}.

#include "loaders/hdf5_loader.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace {

// MAY writes string lists as JSON. yaml-cpp handles this syntax, and its scalar
// tags let numeric network arrays pass through unchanged.
std::optional<std::vector<std::string>> parseJsonStringArray(
    const std::string& text) {
  const size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos || text[first] != '[') return std::nullopt;

  // MAY writes list columns as JSON arrays. Check the first item so numeric
  // network arrays can be rejected before calling yaml-cpp. Empty arrays are
  // treated as string lists; quoted arrays still use yaml-cpp for escaping and
  // validation.
  const size_t first_item = text.find_first_not_of(" \t\r\n", first + 1);
  if (first_item == std::string::npos || text[first_item] == ']')
    return std::vector<std::string>{};
  if (text[first_item] != '"' && text[first_item] != '\'') return std::nullopt;

  try {
    YAML::Node node = YAML::Load(text);
    if (!node.IsSequence()) return std::nullopt;

    std::vector<std::string> values;
    values.reserve(node.size());
    for (const auto& item : node) {
      if (!item.IsScalar() || item.Tag() != "!") return std::nullopt;
      values.push_back(item.as<std::string>());
    }
    return values;
  } catch (const YAML::Exception&) {
    return std::nullopt;
  }
}

template <typename ReadFn>
std::vector<std::string> readStringValues(H5::DataSet& dataset,
                                          H5::DataSpace& reclaim_space,
                                          size_t count, ReadFn&& read) {
  std::vector<std::string> result(count);
  if (count == 0) return result;

  H5::StrType str_type = dataset.getStrType();
  if (str_type.isVariableStr()) {
    std::vector<char*> data(count);
    read(data.data(), str_type);
    for (size_t i = 0; i < count; ++i) {
      if (data[i]) result[i] = data[i];
    }
    H5::DataSet::vlenReclaim(data.data(), str_type, reclaim_space);
  } else {
    const size_t string_size = str_type.getSize();
    std::vector<char> data(count * string_size);
    read(data.data(), str_type);
    for (size_t i = 0; i < count; ++i) {
      result[i] = std::string(&data[i * string_size], string_size);
      const size_t null_pos = result[i].find('\0');
      if (null_pos != std::string::npos) result[i].resize(null_pos);
    }
  }
  return result;
}

bool hdf5ObjectExists(H5::H5File& file, const std::string& path,
                      H5O_type_t wanted_type) {
  if (H5Lexists(file.getId(), path.c_str(), H5P_DEFAULT) <= 0) return false;

#if H5_VERSION_GE(1, 12, 0)
  H5O_info2_t info;
  if (H5Oget_info_by_name3(file.getId(), path.c_str(), &info, H5O_INFO_BASIC,
                           H5P_DEFAULT) < 0)
#else
  H5O_info_t info;
  if (H5Oget_info_by_name(file.getId(), path.c_str(), &info, H5P_DEFAULT) < 0)
#endif
    return false;
  return info.type == wanted_type;
}

std::vector<std::string> listChildNames(H5::H5File& file,
                                        const std::string& group_path,
                                        H5G_obj_t wanted_type) {
  std::vector<std::string> names;
  try {
    H5::Group group = file.openGroup(group_path);
    hsize_t num_objects = group.getNumObjs();
    for (hsize_t i = 0; i < num_objects; ++i) {
      if (group.getObjTypeByIdx(i) == wanted_type) {
        names.push_back(group.getObjnameByIdx(i));
      }
    }
  } catch (...) {
  }
  return names;
}

}  // namespace

namespace june {

HDF5Loader::HDF5Loader(const std::string& filename)
    : file_(filename, H5F_ACC_RDONLY) {}

bool HDF5Loader::datasetExists(const std::string& path) {
  if (dataset_cache_.count(path)) return true;
  return hdf5ObjectExists(file_, path, H5O_TYPE_DATASET);
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
  return hdf5ObjectExists(file_, path, H5O_TYPE_GROUP);
}

std::vector<std::string> HDF5Loader::getDatasetNames(
    const std::string& groupPath) {
  return listChildNames(file_, groupPath, H5G_DATASET);
}

std::vector<std::string> HDF5Loader::getGroupNames(
    const std::string& groupPath) {
  return listChildNames(file_, groupPath, H5G_GROUP);
}

std::vector<std::string> HDF5Loader::readStringDataset(
    const std::string& path) {
  H5::DataSet& dataset = getDataSet(path);
  H5::DataSpace dataspace = dataset.getSpace();

  hsize_t dims[1];
  dataspace.getSimpleExtentDims(dims);
  size_t count = dims[0];
  return readStringValues(dataset, dataspace, count,
                          [&dataset](void* data, const H5::DataType& type) {
                            dataset.read(data, type);
                          });
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

  return readStringValues(
      dataset, memspace, count,
      [&dataset, &memspace, &dataspace](void* data, const H5::DataType& type) {
        dataset.read(data, type, memspace, dataspace);
      });
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
        }

        if (registry && !registry->empty()) {
          auto [min_it, max_it] = std::minmax_element(ints.begin(), ints.end());
          if (*max_it >= (int32_t)registry->size() || *min_it < -1) {
            std::cerr << "WARNING: Property '" << prop_name << "' in " << path
                      << " has out-of-range codes (max=" << *max_it
                      << ", reg_size=" << registry->size() << ")" << std::endl;

            // Replace invalid registry codes with -1, the null sentinel.
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
        const bool is_population_property =
            path.starts_with("/population/properties/");
        for (size_t i = 0; i < count; ++i) {
          if (is_population_property) {
            auto list = parseJsonStringArray(strings[i]);
            if (list.has_value()) {
              result[i] = std::move(*list);
              continue;
            }
          }
          result[i] = std::move(strings[i]);
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
