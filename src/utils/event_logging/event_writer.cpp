#include "utils/event_logging/event_writer.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>

#ifdef USE_MPI
#include <mpi.h>
#endif

#include "utils/event_logger.h"
#include "utils/event_logging/event_writer_detail.h"

namespace {
// Returns true when the caller is the rank that should produce
// human-readable progress output. With MPI: rank 0 only. Without MPI or
// before MPI_Init: always true. Same pattern as MemoryUtils::logMemory.
bool isLogRank() {
#ifdef USE_MPI
  int initialized = 0;
  MPI_Initialized(&initialized);
  if (initialized) {
    int rank = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    return rank == 0;
  }
#endif
  return true;
}

// Write a string-registry vector as a chunked variable-length string dataset.
// No-op when the registry is empty or the dataset already exists.
void writeStringRegistry(H5::Group& registries_group,
                         const std::string& dataset_name,
                         const std::vector<std::string>& names) {
  if (names.empty()) return;
  if (H5Lexists(registries_group.getId(), dataset_name.c_str(), H5P_DEFAULT))
    return;

  hsize_t dims[1] = {names.size()};
  H5::DataSpace space(1, dims);
  H5::StrType type(H5::PredType::C_S1, H5T_VARIABLE);
  H5::DSetCreatPropList plist;
  hsize_t chunk_dims[1] = {std::min(dims[0], hsize_t(100000))};
  if (chunk_dims[0] == 0) chunk_dims[0] = 1;
  plist.setChunk(1, chunk_dims);

  H5::DataSet ds = registries_group.createDataSet(dataset_name, type, space,
                                                  plist);
  std::vector<const char*> c_strs;
  c_strs.reserve(names.size());
  for (const auto& s : names) c_strs.push_back(s.c_str());
  ds.write(c_strs.data(), type);
}
}  // namespace

namespace june {

void EventWriter::saveToHDF5WithLookups(
    const EventLogger& logger, const std::string& filename,
    const WorldState& world, const Config& config,
    const std::unordered_set<PersonId>* person_ids_filter) {
  if (isLogRank()) {
    std::cout << "\n=== Saving Events + Lookup Tables to HDF5: " << filename
              << " ===" << std::endl;
  }

  try {
    bool exists = std::filesystem::exists(filename);
    H5::H5File file;
    if (exists) {
      file = H5::H5File(filename, H5F_ACC_RDWR);
    } else {
      file = H5::H5File(filename, H5F_ACC_TRUNC);
      file.createGroup("/events");
    }

    writeInfectionEvents(file, logger, config.simulation.compression_level);
    writeSymptomChangeEvents(file, logger, config.simulation.compression_level);
    writeDeathEvents(file, logger, config.simulation.compression_level);
    writeHospitalAdmissionEvents(file, logger,
                                 config.simulation.compression_level);
    writeICUAdmissionEvents(file, logger, config.simulation.compression_level);
    writeHospitalDischargeEvents(file, logger,
                                 config.simulation.compression_level);
    writeVaccinationEvents(file, logger, config.simulation.compression_level);
    writeRelationshipEvents(file, logger, config.simulation.compression_level);
    writeCoordinatedEncounterEvents(file, logger,
                                    config.simulation.compression_level);

    // Always write lookups and metadata at the end, even if file exists
    H5::Group lookups_group =
        event_writer_detail::openOrCreateGroup(file, "/lookups");

    if (config.simulation.save_full_person_details != "none") {
      writePersonLookupTable(file, world, config, logger, exists,
                             person_ids_filter);
    }
    if (config.simulation.save_population_summary && !exists) {
      // Only write population summary once
      writePopulationSummary(file, world, config);
      writePopulationNetworks(file, world, config);
    }
    if (!exists) {
      writeVenueLookupTable(file, world, config);
    }
    if (config.simulation.save_person_activities != "none") {
      writePersonActivitiesTable(file, world, config, logger, exists,
                                 person_ids_filter);
    }

    H5::Group metadata_group =
        event_writer_detail::openOrCreateGroup(file, "/metadata");
    H5::Group registries_group =
        event_writer_detail::openOrCreateGroup(metadata_group, "registries");

    writeStringRegistry(registries_group, "encounter_types",
                        world.encounter_type_names);
    writeStringRegistry(registries_group, "activities", world.activity_names);
    writeStringRegistry(registries_group, "symptoms", world.symptom_names);

    if (isLogRank()) {
      std::cout << "Events and lookup tables saved successfully!" << std::endl;
    }
  } catch (const H5::Exception& e) {
    std::cerr << "HDF5 error while saving: " << e.getDetailMsg() << std::endl;
    throw std::runtime_error("Failed to save events and lookups to HDF5 file");
  }
}

void EventWriter::writeInfectionEvents(H5::H5File& file,
                                       const EventLogger& logger,
                                       int compression_level) {
  H5::CompType type(sizeof(InfectionEvent));
  type.insertMember("person_id", HOFFSET(InfectionEvent, person_id),
                    H5::PredType::NATIVE_INT);
  type.insertMember("infector_id", HOFFSET(InfectionEvent, infector_id),
                    H5::PredType::NATIVE_INT);
  type.insertMember("venue_id", HOFFSET(InfectionEvent, venue_id),
                    H5::PredType::NATIVE_INT);
  type.insertMember("time", HOFFSET(InfectionEvent, time),
                    H5::PredType::NATIVE_DOUBLE);
  type.insertMember("encounter_type_id",
                    HOFFSET(InfectionEvent, encounter_type_id),
                    H5::PredType::NATIVE_UINT8);
  type.insertMember("transmission_mode_index",
                    HOFFSET(InfectionEvent, transmission_mode_index),
                    H5::PredType::NATIVE_UINT8);
  type.insertMember("infector_symptom_id",
                    HOFFSET(InfectionEvent, infector_symptom_id),
                    H5::PredType::NATIVE_UINT16);
  type.insertMember("source", HOFFSET(InfectionEvent, source),
                    H5::PredType::NATIVE_UINT8);
  writeDatasetTemplate(file, "/events/infections", logger.infections_, type,
                       compression_level);
}

void EventWriter::writeSymptomChangeEvents(H5::H5File& file,
                                           const EventLogger& logger,
                                           int compression_level) {
  if (logger.symptom_changes_.empty()) return;
  std::vector<detail::SymptomChangeRecord> records(
      logger.symptom_changes_.size());
  for (size_t i = 0; i < logger.symptom_changes_.size(); ++i) {
    const auto& event = logger.symptom_changes_[i];
    records[i].person_id = event.person_id;
    records[i].venue_id = event.venue_id;
    records[i].time = event.time;
    records[i].old_symptom_id = event.old_symptom_id;
    records[i].new_symptom_id = event.new_symptom_id;
  }
  H5::CompType type(sizeof(detail::SymptomChangeRecord));
  type.insertMember("person_id",
                    HOFFSET(detail::SymptomChangeRecord, person_id),
                    H5::PredType::NATIVE_INT);
  type.insertMember("venue_id", HOFFSET(detail::SymptomChangeRecord, venue_id),
                    H5::PredType::NATIVE_INT);
  type.insertMember("time", HOFFSET(detail::SymptomChangeRecord, time),
                    H5::PredType::NATIVE_DOUBLE);
  type.insertMember("old_symptom_id",
                    HOFFSET(detail::SymptomChangeRecord, old_symptom_id),
                    H5::PredType::NATIVE_UINT16);
  type.insertMember("new_symptom_id",
                    HOFFSET(detail::SymptomChangeRecord, new_symptom_id),
                    H5::PredType::NATIVE_UINT16);
  writeDatasetTemplate(file, "/events/symptom_changes", records, type,
                       compression_level);
}

void EventWriter::writeDeathEvents(H5::H5File& file, const EventLogger& logger,
                                   int compression_level) {
  H5::CompType type(sizeof(DeathEvent));
  type.insertMember("person_id", HOFFSET(DeathEvent, person_id),
                    H5::PredType::NATIVE_INT);
  type.insertMember("venue_id", HOFFSET(DeathEvent, venue_id),
                    H5::PredType::NATIVE_INT);
  type.insertMember("time", HOFFSET(DeathEvent, time),
                    H5::PredType::NATIVE_DOUBLE);
  writeDatasetTemplate(file, "/events/deaths", logger.deaths_, type,
                       compression_level);
}

void EventWriter::writeHospitalAdmissionEvents(H5::H5File& file,
                                               const EventLogger& logger,
                                               int compression_level) {
  if (logger.hospital_admissions_.empty()) return;
  std::vector<detail::HospitalAdmissionRecord> records(
      logger.hospital_admissions_.size());
  for (size_t i = 0; i < logger.hospital_admissions_.size(); ++i) {
    const auto& event = logger.hospital_admissions_[i];
    records[i].person_id = event.person_id;
    records[i].hospital_id = event.hospital_id;
    records[i].time = event.time;
    strncpy(records[i].reason, event.reason.c_str(), 63);
    records[i].reason[63] = '\0';
  }
  H5::StrType str_type(H5::PredType::C_S1, 64);
  H5::CompType type(sizeof(detail::HospitalAdmissionRecord));
  type.insertMember("person_id",
                    HOFFSET(detail::HospitalAdmissionRecord, person_id),
                    H5::PredType::NATIVE_INT);
  type.insertMember("hospital_id",
                    HOFFSET(detail::HospitalAdmissionRecord, hospital_id),
                    H5::PredType::NATIVE_INT);
  type.insertMember("time", HOFFSET(detail::HospitalAdmissionRecord, time),
                    H5::PredType::NATIVE_DOUBLE);
  type.insertMember("reason", HOFFSET(detail::HospitalAdmissionRecord, reason),
                    str_type);
  writeDatasetTemplate(file, "/events/hospital_admissions", records, type,
                       compression_level);
}

void EventWriter::writeICUAdmissionEvents(H5::H5File& file,
                                          const EventLogger& logger,
                                          int compression_level) {
  H5::CompType type(sizeof(ICUAdmissionEvent));
  type.insertMember("person_id", HOFFSET(ICUAdmissionEvent, person_id),
                    H5::PredType::NATIVE_INT);
  type.insertMember("hospital_id", HOFFSET(ICUAdmissionEvent, hospital_id),
                    H5::PredType::NATIVE_INT);
  type.insertMember("time", HOFFSET(ICUAdmissionEvent, time),
                    H5::PredType::NATIVE_DOUBLE);
  writeDatasetTemplate(file, "/events/icu_admissions", logger.icu_admissions_,
                       type, compression_level);
}

void EventWriter::writeHospitalDischargeEvents(H5::H5File& file,
                                               const EventLogger& logger,
                                               int compression_level) {
  if (logger.hospital_discharges_.empty()) return;
  std::vector<detail::HospitalDischargeRecord> records(
      logger.hospital_discharges_.size());
  for (size_t i = 0; i < logger.hospital_discharges_.size(); ++i) {
    const auto& event = logger.hospital_discharges_[i];
    records[i].person_id = event.person_id;
    records[i].hospital_id = event.hospital_id;
    records[i].time = event.time;
    strncpy(records[i].outcome, event.outcome.c_str(), 63);
    records[i].outcome[63] = '\0';
  }
  H5::StrType str_type(H5::PredType::C_S1, 64);
  H5::CompType type(sizeof(detail::HospitalDischargeRecord));
  type.insertMember("person_id",
                    HOFFSET(detail::HospitalDischargeRecord, person_id),
                    H5::PredType::NATIVE_INT);
  type.insertMember("hospital_id",
                    HOFFSET(detail::HospitalDischargeRecord, hospital_id),
                    H5::PredType::NATIVE_INT);
  type.insertMember("time", HOFFSET(detail::HospitalDischargeRecord, time),
                    H5::PredType::NATIVE_DOUBLE);
  type.insertMember(
      "outcome", HOFFSET(detail::HospitalDischargeRecord, outcome), str_type);
  writeDatasetTemplate(file, "/events/hospital_discharges", records, type,
                       compression_level);
}

void EventWriter::writeVaccinationEvents(H5::H5File& file,
                                         const EventLogger& logger,
                                         int compression_level) {
  if (logger.vaccinations_.empty()) return;
  std::vector<detail::VaccinationRecord> records(logger.vaccinations_.size());
  for (size_t i = 0; i < logger.vaccinations_.size(); ++i) {
    const auto& event = logger.vaccinations_[i];
    records[i].person_id = event.person_id;
    strncpy(records[i].vaccine_type, event.vaccine_type, 63);
    records[i].vaccine_type[63] = '\0';
    records[i].dose_index = event.dose_index;
    records[i].time = event.time;
  }
  H5::StrType str_type(H5::PredType::C_S1, 64);
  H5::CompType type(sizeof(detail::VaccinationRecord));
  type.insertMember("person_id", HOFFSET(detail::VaccinationRecord, person_id),
                    H5::PredType::NATIVE_INT);
  type.insertMember("vaccine_type",
                    HOFFSET(detail::VaccinationRecord, vaccine_type), str_type);
  type.insertMember("dose_index",
                    HOFFSET(detail::VaccinationRecord, dose_index),
                    H5::PredType::NATIVE_INT);
  type.insertMember("time", HOFFSET(detail::VaccinationRecord, time),
                    H5::PredType::NATIVE_DOUBLE);
  writeDatasetTemplate(file, "/events/vaccinations", records, type,
                       compression_level);
}

H5::DataSet EventWriter::createCompressedDataset(H5::H5File& file,
                                                 const std::string& name,
                                                 const H5::DataType& datatype,
                                                 const H5::DataSpace& dataspace,
                                                 int compression_level) {
  if (compression_level <= 0)
    return file.createDataSet(name, datatype, dataspace);
  hsize_t dims[1];
  dataspace.getSimpleExtentDims(dims);
  H5::DSetCreatPropList plist;
  hsize_t chunk_dims[1] = {std::min(dims[0], hsize_t(100000))};
  plist.setChunk(1, chunk_dims);
  plist.setDeflate(compression_level);
  return file.createDataSet(name, datatype, dataspace, plist);
}

void EventWriter::writeRelationshipEvents(H5::H5File& file,
                                          const EventLogger& logger,
                                          int compression_level) {
  if (logger.relationships_.empty()) return;
  std::vector<detail::RelationshipRecord> records(logger.relationships_.size());
  for (size_t i = 0; i < logger.relationships_.size(); ++i) {
    const auto& event = logger.relationships_[i];
    records[i].person_a = event.person_a;
    records[i].person_b = event.person_b;
    records[i].time = event.time;
    records[i].dissolution_time = event.dissolution_time;
    strncpy(records[i].tie_tag, event.tie_tag, 31);
    records[i].tie_tag[31] = '\0';
  }
  H5::StrType tag_str_type(H5::PredType::C_S1, 32);
  H5::CompType type(sizeof(detail::RelationshipRecord));
  type.insertMember("person_a", HOFFSET(detail::RelationshipRecord, person_a),
                    H5::PredType::NATIVE_INT);
  type.insertMember("person_b", HOFFSET(detail::RelationshipRecord, person_b),
                    H5::PredType::NATIVE_INT);
  type.insertMember("time", HOFFSET(detail::RelationshipRecord, time),
                    H5::PredType::NATIVE_DOUBLE);
  type.insertMember("dissolution_time",
                    HOFFSET(detail::RelationshipRecord, dissolution_time),
                    H5::PredType::NATIVE_DOUBLE);
  type.insertMember("tie_tag", HOFFSET(detail::RelationshipRecord, tie_tag),
                    tag_str_type);
  writeDatasetTemplate(file, "/events/relationships", records, type,
                       compression_level);
}

void EventWriter::writeCoordinatedEncounterEvents(H5::H5File& file,
                                                  const EventLogger& logger,
                                                  int compression_level) {
  if (logger.coordinated_encounters_.empty()) return;
  std::vector<detail::CoordinatedEncounterRecord> records(
      logger.coordinated_encounters_.size());
  for (size_t i = 0; i < logger.coordinated_encounters_.size(); ++i) {
    const auto& event = logger.coordinated_encounters_[i];
    records[i].person_a = event.person_a;
    records[i].person_b = event.person_b;
    records[i].time = event.time;
    records[i].encounter_type_id = event.encounter_type_id;
    records[i].slot = event.slot;
    records[i].group_id = event.group_id;
  }
  H5::CompType type(sizeof(detail::CoordinatedEncounterRecord));
  type.insertMember("person_a",
                    HOFFSET(detail::CoordinatedEncounterRecord, person_a),
                    H5::PredType::NATIVE_INT);
  type.insertMember("person_b",
                    HOFFSET(detail::CoordinatedEncounterRecord, person_b),
                    H5::PredType::NATIVE_INT);
  type.insertMember("time", HOFFSET(detail::CoordinatedEncounterRecord, time),
                    H5::PredType::NATIVE_DOUBLE);
  type.insertMember(
      "encounter_type_id",
      HOFFSET(detail::CoordinatedEncounterRecord, encounter_type_id),
      H5::PredType::NATIVE_UINT8);
  type.insertMember("slot", HOFFSET(detail::CoordinatedEncounterRecord, slot),
                    H5::PredType::NATIVE_INT);
  type.insertMember("group_id",
                    HOFFSET(detail::CoordinatedEncounterRecord, group_id),
                    H5::PredType::NATIVE_UINT64);
  writeDatasetTemplate(file, "/events/coordinated_encounters", records, type,
                       compression_level);
}

}  // namespace june
