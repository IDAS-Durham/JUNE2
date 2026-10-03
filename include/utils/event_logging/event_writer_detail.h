#pragma once

#include <H5Cpp.h>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/config.h"
#include "core/world_state.h"
#include "event_types.h"

// Helpers shared across event-logging translation units.

namespace june::event_writer_detail {

inline H5::DSetCreatPropList chunkedProperties(hsize_t length,
                                               int compression_level);

}  // namespace june::event_writer_detail

namespace june::event_writer {

void writePersonLookupTable(
    H5::H5File& file, const WorldState& world, const Config& config,
    const std::unordered_set<PersonId>& infected_person_ids,
    bool append = false,
    const std::unordered_set<PersonId>* person_ids_filter = nullptr);
void writeVenueLookupTable(H5::H5File& file, const WorldState& world,
                           const Config& config);
void writePersonActivitiesTable(
    H5::H5File& file, const WorldState& world, const Config& config,
    const std::unordered_set<PersonId>& infected_person_ids,
    bool append = false,
    const std::unordered_set<PersonId>* person_ids_filter = nullptr);
void writePopulationSummary(H5::H5File& file, const WorldState& world,
                            const Config& config);
void writePopulationNetworks(H5::H5File& file, const WorldState& world,
                             const Config& config);

template <typename T>
void appendDatasetTemplate(H5::H5File& file, const std::string& name,
                           const std::vector<T>& data,
                           const H5::CompType& type) {
  if (data.empty()) return;

  H5::DataSet dataset = file.openDataSet(name);
  H5::DataSpace filespace = dataset.getSpace();

  hsize_t current_dims[1];
  filespace.getSimpleExtentDims(current_dims);

  hsize_t new_dims[1] = {current_dims[0] + data.size()};
  dataset.extend(new_dims);

  filespace = dataset.getSpace();
  hsize_t offset[1] = {current_dims[0]};
  hsize_t count[1] = {data.size()};
  filespace.selectHyperslab(H5S_SELECT_SET, count, offset);

  H5::DataSpace memspace(1, count);
  dataset.write(data.data(), type, memspace, filespace);
}

template <typename T>
void writeDatasetTemplate(H5::H5File& file, const std::string& name,
                          const std::vector<T>& data, const H5::CompType& type,
                          int compression_level = 0) {
  if (data.empty()) return;

  if (H5Lexists(file.getId(), name.c_str(), H5P_DEFAULT)) {
    appendDatasetTemplate(file, name, data, type);
    return;
  }

  hsize_t dims[1] = {data.size()};
  hsize_t maxdims[1] = {H5S_UNLIMITED};
  H5::DataSpace dataspace(1, dims, maxdims);

  H5::DSetCreatPropList plist =
      event_writer_detail::chunkedProperties(dims[0], compression_level);
  H5::DataSet dataset = file.createDataSet(name, type, dataspace, plist);
  dataset.write(data.data(), type);
}

}  // namespace june::event_writer

namespace june::event_writer_detail {

inline H5::DSetCreatPropList chunkedProperties(hsize_t length,
                                               int compression_level) {
  H5::DSetCreatPropList plist;
  hsize_t chunk_dims[1] = {std::min(length, hsize_t(100000))};
  if (chunk_dims[0] == 0) chunk_dims[0] = 1;
  plist.setChunk(1, chunk_dims);
  if (compression_level > 0) plist.setDeflate(compression_level);
  return plist;
}

// Open `name` under `parent` if it exists, otherwise create it. Parent can be
// an HDF5 file or group.
template <typename Parent>
inline H5::Group openOrCreateGroup(Parent& parent, const std::string& name) {
  if (H5Lexists(parent.getId(), name.c_str(), H5P_DEFAULT))
    return parent.openGroup(name);
  return parent.createGroup(name);
}

// Write a variable-length string dataset, creating it or appending to an
// existing extendible dataset according to `append`.
inline void writeStringDataset(H5::Group& group, const std::string& name,
                               const std::vector<std::string>& values,
                               bool append) {
  if (values.empty()) return;

  H5::StrType type(H5::PredType::C_S1, H5T_VARIABLE);
  std::vector<const char*> c_strs;
  c_strs.reserve(values.size());
  for (const auto& value : values) c_strs.push_back(value.c_str());

  if (H5Lexists(group.getId(), name.c_str(), H5P_DEFAULT)) {
    if (!append) return;

    H5::DataSet dataset = group.openDataSet(name);
    H5::DataSpace current_space = dataset.getSpace();
    hsize_t current_dims[1];
    current_space.getSimpleExtentDims(current_dims);

    hsize_t count[1] = {values.size()};
    hsize_t new_dims[1] = {current_dims[0] + count[0]};
    dataset.extend(new_dims);

    H5::DataSpace file_space = dataset.getSpace();
    file_space.selectHyperslab(H5S_SELECT_SET, count, current_dims);
    H5::DataSpace memory_space(1, count);
    dataset.write(c_strs.data(), type, memory_space, file_space);
    return;
  }

  hsize_t dims[1] = {values.size()};
  H5::DataSpace space;
  H5::DSetCreatPropList plist = chunkedProperties(dims[0], 0);

  if (append) {
    hsize_t max_dims[1] = {H5S_UNLIMITED};
    space = H5::DataSpace(1, dims, max_dims);
  } else {
    space = H5::DataSpace(1, dims);
  }

  H5::DataSet dataset = group.createDataSet(name, type, space, plist);
  dataset.write(c_strs.data(), type);
}

}  // namespace june::event_writer_detail
