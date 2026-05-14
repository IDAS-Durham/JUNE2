#pragma once

#include <H5Cpp.h>

#include <algorithm>
#include <string>
#include <vector>

#include "event_types.h"

namespace june {

class EventMerger {
 public:
  static void mergeEventFiles(const std::vector<std::string>& input_files,
                              const std::string& output_file);

 private:
  static void mergeInfectionEvents(H5::H5File& out_file,
                                   const std::vector<std::string>& input_files);
  static void mergeSymptomChangeEvents(
      H5::H5File& out_file, const std::vector<std::string>& input_files);
  static void mergeDeathEvents(H5::H5File& out_file,
                               const std::vector<std::string>& input_files);
  static void mergeHospitalAdmissionEvents(
      H5::H5File& out_file, const std::vector<std::string>& input_files);
  static void mergeICUAdmissionEvents(
      H5::H5File& out_file, const std::vector<std::string>& input_files);
  static void mergeHospitalDischargeEvents(
      H5::H5File& out_file, const std::vector<std::string>& input_files);
  static void mergeVaccinationEvents(
      H5::H5File& out_file, const std::vector<std::string>& input_files);
  static void mergeRelationshipEvents(
      H5::H5File& out_file, const std::vector<std::string>& input_files);
  static void mergeCoordinatedEncounterEvents(
      H5::H5File& out_file, const std::vector<std::string>& input_files);

  static void mergePeopleLookup(H5::H5File& out_file,
                                const std::vector<std::string>& input_files);
  static void mergeVenueLookup(H5::H5File& out_file,
                               const std::vector<std::string>& input_files);
  static void mergePersonActivityLookup(
      H5::H5File& out_file, const std::vector<std::string>& input_files);
  static void mergePopulationSummary(
      H5::H5File& out_file, const std::vector<std::string>& input_files);
  // Concatenate /lookups/profile_assignments/<facet>/<field> arrays across
  // all rank files. No-op if no rank file contains the group.
  static void mergeProfileAssignments(
      H5::H5File& out_file, const std::vector<std::string>& input_files);
  // Concatenate /lookups/population_networks/<name>/{person_id,partner_id}
  // across all rank files. No-op if no rank file contains the group.
  static void mergePopulationNetworks(
      H5::H5File& out_file, const std::vector<std::string>& input_files);

  // Template helper: merging records from multiple files (Streaming version)
  template <typename T>
  static void mergeDatasetTemplate(H5::H5File& out_file,
                                   const std::string& name,
                                   const std::vector<std::string>& input_files,
                                   const H5::CompType& type) {
    // 1. Calculate total count across all files
    hsize_t total_count = 0;
    for (const auto& f : input_files) {
      try {
        H5::H5File file(f, H5F_ACC_RDONLY);
        if (H5Lexists(file.getId(), name.c_str(), H5P_DEFAULT)) {
          H5::DataSet ds = file.openDataSet(name);
          hsize_t dims[1];
          ds.getSpace().getSimpleExtentDims(dims);
          total_count += dims[0];
        }
      } catch (...) {
      }
    }

    if (total_count == 0) return;

    // 2. Create the output dataset with total size and compression
    hsize_t dims[1] = {total_count};
    H5::DataSpace out_space(1, dims);
    H5::DSetCreatPropList plist;
    if (total_count > 1000) {
      hsize_t chunk_dims[1] = {std::min(total_count, hsize_t(100000))};
      plist.setChunk(1, chunk_dims);
      plist.setDeflate(6);
    }
    H5::DataSet out_ds = out_file.createDataSet(name, type, out_space, plist);

    // 3. Stream data from input files using hyperslabs
    hsize_t current_out_offset = 0;
    const size_t CHUNK_SIZE = 100000;
    std::vector<T> buffer;

    for (const auto& f : input_files) {
      try {
        H5::H5File file(f, H5F_ACC_RDONLY);
        if (H5Lexists(file.getId(), name.c_str(), H5P_DEFAULT)) {
          H5::DataSet in_ds = file.openDataSet(name);
          H5::DataSpace in_space = in_ds.getSpace();
          hsize_t in_dims[1];
          in_space.getSimpleExtentDims(in_dims);

          hsize_t in_count = in_dims[0];
          for (hsize_t offset = 0; offset < in_count; offset += CHUNK_SIZE) {
            hsize_t count = std::min(hsize_t(CHUNK_SIZE), in_count - offset);
            buffer.resize(count);

            // Select and read from input
            hsize_t count_h[1] = {count};
            hsize_t in_offset_h[1] = {offset};
            in_space.selectHyperslab(H5S_SELECT_SET, count_h, in_offset_h);
            H5::DataSpace mem_space(1, count_h);
            in_ds.read(buffer.data(), type, mem_space, in_space);

            // Select and write to output
            hsize_t out_offset_h[1] = {current_out_offset};
            out_space.selectHyperslab(H5S_SELECT_SET, count_h, out_offset_h);
            out_ds.write(buffer.data(), type, mem_space, out_space);

            current_out_offset += count;
          }
        }
      } catch (...) {
      }
    }
  }
};

}  // namespace june
