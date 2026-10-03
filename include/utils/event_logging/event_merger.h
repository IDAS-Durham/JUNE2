#pragma once

#include <H5Cpp.h>

#include <algorithm>
#include <string>
#include <vector>

#include "event_types.h"

namespace june {

namespace event_merger_detail {

// Visit every input dataset in bounded chunks, preserving file and row order.
// `values_per_record` is one for typed records and the HDF5 element size when
// the callback needs a byte buffer for a runtime-selected type.
template <typename T, typename Callback>
hsize_t forEachDatasetChunk(const std::vector<std::string>& input_files,
                            const std::string& name, const H5::DataType& type,
                            Callback callback, size_t values_per_record = 1) {
  constexpr hsize_t kChunkSize = 100000;
  hsize_t total_count = 0;

  for (const auto& f : input_files) {
    try {
      H5::H5File file(f, H5F_ACC_RDONLY);
      if (!H5Lexists(file.getId(), name.c_str(), H5P_DEFAULT)) continue;

      H5::DataSet dataset = file.openDataSet(name);
      H5::DataSpace input_space = dataset.getSpace();
      hsize_t dims[1];
      input_space.getSimpleExtentDims(dims);

      for (hsize_t offset = 0; offset < dims[0]; offset += kChunkSize) {
        hsize_t count = std::min(kChunkSize, dims[0] - offset);
        std::vector<T> buffer(count * values_per_record);
        hsize_t count_h[1] = {count};
        hsize_t offset_h[1] = {offset};
        input_space.selectHyperslab(H5S_SELECT_SET, count_h, offset_h);
        H5::DataSpace memory_space(1, count_h);
        dataset.read(buffer.data(), type, memory_space, input_space);
        callback(buffer, count, total_count);
        total_count += count;
      }
    } catch (...) {
    }
  }
  return total_count;
}

// Sum the record counts from dataset metadata without reading record data.
inline hsize_t countDatasetRecords(
    const std::vector<std::string>& input_files, const std::string& name) {
  hsize_t total_count = 0;

  for (const auto& f : input_files) {
    try {
      H5::H5File file(f, H5F_ACC_RDONLY);
      if (!H5Lexists(file.getId(), name.c_str(), H5P_DEFAULT)) continue;

      H5::DataSet dataset = file.openDataSet(name);
      H5::DataSpace input_space = dataset.getSpace();
      hsize_t dims[1];
      input_space.getSimpleExtentDims(dims);
      total_count += dims[0];
    } catch (...) {
    }
  }
  return total_count;
}

// Shared writer for typed records and runtime-selected HDF5 fields.
template <typename T, typename OutputContainer>
void writeMergedDataset(OutputContainer& out_container,
                        const std::string& output_name,
                        const std::vector<std::string>& input_files,
                        const std::string& input_name,
                        const H5::DataType& type,
                        size_t values_per_record = 1,
                        bool always_chunked = false) {
  hsize_t total_count = countDatasetRecords(input_files, input_name);
  if (total_count == 0) return;

  hsize_t dims[1] = {total_count};
  H5::DataSpace out_space(1, dims);
  H5::DSetCreatPropList plist;
  if (always_chunked || total_count > 1000) {
    hsize_t chunk_dims[1] = {std::min(total_count, hsize_t(100000))};
    plist.setChunk(1, chunk_dims);
    plist.setDeflate(6);
  }
  H5::DataSet out_ds =
      out_container.createDataSet(output_name, type, out_space, plist);

  hsize_t current_out_offset = 0;
  forEachDatasetChunk<T>(
      input_files, input_name, type,
      [&](const std::vector<T>& buffer, hsize_t count, hsize_t) {
        hsize_t count_h[1] = {count};
        hsize_t out_offset_h[1] = {current_out_offset};
        out_space.selectHyperslab(H5S_SELECT_SET, count_h, out_offset_h);
        H5::DataSpace mem_space(1, count_h);
        out_ds.write(buffer.data(), type, mem_space, out_space);
        current_out_offset += count;
      },
      values_per_record);
}

// Template helper: merge records from multiple files in bounded chunks.
template <typename T>
void mergeDatasetTemplate(H5::H5File& out_file, const std::string& name,
                          const std::vector<std::string>& input_files,
                          const H5::CompType& type) {
  writeMergedDataset<T>(out_file, name, input_files, name, type);
}

void mergePeopleLookup(H5::H5File& out_file,
                       const std::vector<std::string>& input_files);
void mergeVenueLookup(H5::H5File& out_file,
                      const std::vector<std::string>& input_files);
void mergePersonActivityLookup(H5::H5File& out_file,
                               const std::vector<std::string>& input_files);
void mergePopulationSummary(H5::H5File& out_file,
                            const std::vector<std::string>& input_files);
void mergeProfileAssignments(H5::H5File& out_file,
                             const std::vector<std::string>& input_files);
void mergePopulationNetworks(H5::H5File& out_file,
                             const std::vector<std::string>& input_files);

}  // namespace event_merger_detail

void mergeEventFiles(const std::vector<std::string>& input_files,
                     const std::string& output_file);

}  // namespace june
