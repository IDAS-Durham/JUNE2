#pragma once

#include <H5Cpp.h>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/config.h"
#include "core/world_state.h"
#include "event_types.h"

namespace june {

class EventLogger;

class EventWriter {
 public:
  static void saveToHDF5WithLookups(
      const EventLogger& logger, const std::string& filename,
      const WorldState& world, const Config& config,
      const std::unordered_set<PersonId>* person_ids_filter = nullptr);

 private:
  // Helper methods for HDF5 writing
  static void writeInfectionEvents(H5::H5File& file, const EventLogger& logger,
                                   int compression_level);
  static void writeSymptomChangeEvents(H5::H5File& file,
                                       const EventLogger& logger,
                                       int compression_level);
  static void writeDeathEvents(H5::H5File& file, const EventLogger& logger,
                               int compression_level);
  static void writeHospitalAdmissionEvents(H5::H5File& file,
                                           const EventLogger& logger,
                                           int compression_level);
  static void writeICUAdmissionEvents(H5::H5File& file,
                                      const EventLogger& logger,
                                      int compression_level);
  static void writeHospitalDischargeEvents(H5::H5File& file,
                                           const EventLogger& logger,
                                           int compression_level);
  static void writeVaccinationEvents(H5::H5File& file,
                                     const EventLogger& logger,
                                     int compression_level);
  static void writeRelationshipEvents(H5::H5File& file,
                                      const EventLogger& logger,
                                      int compression_level);
  static void writeCoordinatedEncounterEvents(H5::H5File& file,
                                              const EventLogger& logger,
                                              int compression_level);

  // Helper methods for lookup tables
  static void writePersonLookupTable(
      H5::H5File& file, const WorldState& world, const Config& config,
      const EventLogger& logger, bool append = false,
      const std::unordered_set<PersonId>* person_ids_filter = nullptr);
  static void writeVenueLookupTable(H5::H5File& file, const WorldState& world,
                                    const Config& config);
  static void writePersonActivitiesTable(
      H5::H5File& file, const WorldState& world, const Config& config,
      const EventLogger& logger, bool append = false,
      const std::unordered_set<PersonId>* person_ids_filter = nullptr);
  static void writePopulationSummary(H5::H5File& file, const WorldState& world,
                                     const Config& config);
  // Full-population person-network dump (e.g. friendships).
  // One group per network type under /lookups/population_networks/<name>/,
  // containing parallel ``person_id`` and ``partner_id`` 1-D int32 datasets,
  // a row per (person, partner) pair. Not gated by save_full_person_details
  // so it stays available even when /lookups/people is infected_only.
  static void writePopulationNetworks(H5::H5File& file, const WorldState& world,
                                      const Config& config);

  // Helper: Create HDF5 dataset with compression
  static H5::DataSet createCompressedDataset(H5::H5File& file,
                                             const std::string& name,
                                             const H5::DataType& datatype,
                                             const H5::DataSpace& dataspace,
                                             int compression_level);

  // Template helper: writing a simple dataset (Overwrite/New)
  template <typename T>
  static void writeDatasetTemplate(H5::H5File& file, const std::string& name,
                                   const std::vector<T>& data,
                                   const H5::CompType& type,
                                   int compression_level = 0) {
    if (data.empty()) return;

    // Check if dataset exists to determine if we should create or extend
    if (H5Lexists(file.getId(), name.c_str(), H5P_DEFAULT)) {
      appendDatasetTemplate(file, name, data, type);
      return;
    }

    hsize_t dims[1] = {data.size()};
    hsize_t maxdims[1] = {H5S_UNLIMITED};
    H5::DataSpace dataspace(1, dims, maxdims);

    H5::DSetCreatPropList plist;
    hsize_t chunk_dims[1] = {std::min(dims[0], hsize_t(100000))};
    plist.setChunk(1, chunk_dims);
    if (compression_level > 0) {
      plist.setDeflate(compression_level);
    }

    H5::DataSet dataset = file.createDataSet(name, type, dataspace, plist);
    dataset.write(data.data(), type);
  }

  // Template helper: appending to an existing dataset
  template <typename T>
  static void appendDatasetTemplate(H5::H5File& file, const std::string& name,
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
};

}  // namespace june
