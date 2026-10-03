#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>

#include "utils/event_logging/event_logger.h"
#include "utils/event_logging/event_schemas.h"
#include "utils/event_logging/event_writer_detail.h"
#include "utils/mpi_logging.h"

namespace june::event_writer {

namespace {

template <typename Event, typename Record, typename Mapper>
void writeConvertedEvents(H5::H5File& file, const std::string& dataset_name,
                          const std::vector<Event>& events,
                          const H5::CompType& type, int compression_level,
                          Mapper mapper) {
  if (events.empty()) return;

  std::vector<Record> records;
  records.reserve(events.size());
  std::transform(events.begin(), events.end(), std::back_inserter(records),
                 mapper);
  writeDatasetTemplate(file, dataset_name, records, type, compression_level);
}

void writeHospitalAdmissionEvents(
    H5::H5File& file, const std::vector<HospitalAdmissionEvent>& events,
    int compression_level) {
  writeConvertedEvents<HospitalAdmissionEvent, detail::HospitalAdmissionRecord>(
      file, "/events/hospital_admissions", events,
      event_schema::hospitalAdmission(), compression_level,
      [](const HospitalAdmissionEvent& event) {
        detail::HospitalAdmissionRecord record{};
        record.person_id = event.person_id;
        record.hospital_id = event.hospital_id;
        record.time = event.time;
        copyFixedField(record.reason, event.reason);
        return record;
      });
}

void writeHospitalDischargeEvents(
    H5::H5File& file, const std::vector<HospitalDischargeEvent>& events,
    int compression_level) {
  writeConvertedEvents<HospitalDischargeEvent, detail::HospitalDischargeRecord>(
      file, "/events/hospital_discharges", events,
      event_schema::hospitalDischarge(), compression_level,
      [](const HospitalDischargeEvent& event) {
        detail::HospitalDischargeRecord record{};
        record.person_id = event.person_id;
        record.hospital_id = event.hospital_id;
        record.time = event.time;
        copyFixedField(record.outcome, event.outcome);
        return record;
      });
}

}  // namespace

void saveToHDF5WithLookups(
    const EventLogger& logger, const std::string& filename,
    const WorldState& world, const Config& config,
    const std::unordered_set<PersonId>& infected_person_ids,
    const std::unordered_set<PersonId>* person_ids_filter) {
  if (logRank0()) {
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

    writeDatasetTemplate(file, "/events/infections", logger.infections_,
                         event_schema::infection(),
                         config.simulation.compression_level);
    writeDatasetTemplate(file, "/events/symptom_changes",
                         logger.symptom_changes_, event_schema::symptomChange(),
                         config.simulation.compression_level);
    writeDatasetTemplate(file, "/events/deaths", logger.deaths_,
                         event_schema::death(),
                         config.simulation.compression_level);
    writeHospitalAdmissionEvents(file, logger.hospital_admissions_,
                                 config.simulation.compression_level);
    writeDatasetTemplate(file, "/events/icu_admissions", logger.icu_admissions_,
                         event_schema::icuAdmission(),
                         config.simulation.compression_level);
    writeHospitalDischargeEvents(file, logger.hospital_discharges_,
                                 config.simulation.compression_level);
    writeDatasetTemplate(file, "/events/vaccinations", logger.vaccinations_,
                         event_schema::vaccination(),
                         config.simulation.compression_level);
    writeDatasetTemplate(file, "/events/relationships", logger.relationships_,
                         event_schema::relationship(),
                         config.simulation.compression_level);
    writeDatasetTemplate(file, "/events/coordinated_encounters",
                         logger.coordinated_encounters_,
                         event_schema::coordinatedEncounter(),
                         config.simulation.compression_level);
    writeDatasetTemplate(file, "/events/follows", logger.follows_,
                         event_schema::follow(),
                         config.simulation.compression_level);

    // Always write lookups and metadata at the end, even if file exists
    event_writer_detail::openOrCreateGroup(file, "/lookups");

    if (config.simulation.save_full_person_details != "none") {
      writePersonLookupTable(file, world, config, infected_person_ids, exists,
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
      writePersonActivitiesTable(file, world, config, infected_person_ids,
                                 exists, person_ids_filter);
    }

    H5::Group metadata_group =
        event_writer_detail::openOrCreateGroup(file, "/metadata");
    H5::Group registries_group =
        event_writer_detail::openOrCreateGroup(metadata_group, "registries");

    event_writer_detail::writeStringDataset(registries_group, "encounter_types",
                                            world.encounter_type_names, false);
    event_writer_detail::writeStringDataset(registries_group, "activities",
                                            world.activity_names, false);
    event_writer_detail::writeStringDataset(registries_group, "symptoms",
                                            world.symptom_names, false);

    // rule_id in /events/follows indexes this, in follows-list order.
    std::vector<std::string> follow_rule_names;
    follow_rule_names.reserve(config.coordinated_encounters.follows.size());
    for (const auto& f : config.coordinated_encounters.follows)
      follow_rule_names.push_back(f.name);
    event_writer_detail::writeStringDataset(registries_group, "follow_rules",
                                            follow_rule_names, false);

    if (logRank0()) {
      std::cout << "Events and lookup tables saved successfully!" << std::endl;
    }
  } catch (const H5::Exception& e) {
    std::cerr << "HDF5 error while saving: " << e.getDetailMsg() << std::endl;
    throw std::runtime_error("Failed to save events and lookups to HDF5 file");
  }
}

}  // namespace june::event_writer
