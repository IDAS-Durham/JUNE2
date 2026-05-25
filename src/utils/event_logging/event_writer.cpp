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

// Open `name` under `parent` if it exists, otherwise create it.
H5::Group openOrCreateGroup(H5::Group& parent, const std::string& name) {
  if (H5Lexists(parent.getId(), name.c_str(), H5P_DEFAULT))
    return parent.openGroup(name);
  return parent.createGroup(name);
}

// Resolve which people should appear in a /lookups/* table for this write
// call. `mode` is the config switch ("none"/"all"/"infected_only"). Empty
// result means "skip the whole table". The mode=="all" + append branch
// returns empty intentionally (already written on first call).
std::unordered_set<june::PersonId> selectPeopleToSave(
    const june::WorldState& world, const std::string& mode,
    const june::EventLogger& logger, bool append,
    const std::unordered_set<june::PersonId>* person_ids_filter) {
  std::unordered_set<june::PersonId> people_to_save;
  if (person_ids_filter) {
    people_to_save = *person_ids_filter;
  } else if (mode == "all") {
    if (append) return people_to_save;
    for (const auto& person : world.people) people_to_save.insert(person.id);
  } else if (mode == "infected_only") {
    people_to_save = logger.getInfectedPersonIds();
  }
  return people_to_save;
}

// Project the selected people into the flat PersonRecord shape written to
// /lookups/people. Field order matches makePersonCompType().
std::vector<june::detail::PersonRecord> buildPersonRecords(
    const june::WorldState& world,
    const std::unordered_set<june::PersonId>& people_to_save) {
  std::vector<june::detail::PersonRecord> records;
  records.reserve(people_to_save.size());
  for (const auto& person : world.people) {
    if (!people_to_save.count(person.id)) continue;
    june::detail::PersonRecord record;
    record.person_id = person.id;
    record.age = person.age;
    std::string sex_str = (person.sex == june::Sex::MALE)     ? "male"
                          : (person.sex == june::Sex::FEMALE) ? "female"
                                                              : "unknown";
    strncpy(record.sex, sex_str.c_str(), 15);
    record.sex[15] = '\0';
    record.geo_unit_id = person.geo_unit_id;
    record.is_dead = person.is_dead ? 1 : 0;
    record.death_time = person.death_time;
    std::string sched_type =
        person.schedule_type_id < world.schedule_type_names.size()
            ? world.schedule_type_names[person.schedule_type_id]
            : "unknown";
    strncpy(record.schedule_type, sched_type.c_str(), 63);
    record.schedule_type[63] = '\0';
    record.num_activities =
        static_cast<int>(world.getActivityMetas(person).size());
    record.num_residence_venues =
        static_cast<int>(world.getActivityVenues(person, "residence").size());
    record.num_primary_activities = static_cast<int>(
        world.getActivityVenues(person, "primary_activity").size());
    record.num_leisure_venues =
        static_cast<int>(world.getActivityVenues(person, "leisure").size());
    record.num_medical_facilities = static_cast<int>(
        world.getActivityVenues(person, "medical_facility").size());
    records.push_back(record);
  }
  return records;
}

// Stringify one person-property column across the selected people, in
// world.people order. Falls back to network-partner serialisation for keys
// whose values live in Person::NetworkMeta rather than the flat property
// table; falls back to "" when neither source has a value.
std::vector<std::string> collectPropertyValues(
    const june::WorldState& world,
    const std::unordered_set<june::PersonId>& people_to_save,
    const std::string& key) {
  const int network_type_id = world.getNetworkTypeIndex(key);
  std::vector<std::string> values;
  values.reserve(people_to_save.size());
  for (const auto& person : world.people) {
    if (!people_to_save.count(person.id)) continue;
    auto prop = world.getPersonProperty(person, key);
    if (prop.has_value()) {
      const auto& val = *prop;
      if (std::holds_alternative<std::string>(val))
        values.push_back(std::get<std::string>(val));
      else if (std::holds_alternative<int32_t>(val)) {
        int32_t iv = std::get<int32_t>(val);
        auto rit = world.person_property_value_registries.find(key);
        if (rit != world.person_property_value_registries.end() && iv >= 0 &&
            (size_t)iv < rit->second.size())
          values.push_back(rit->second[iv]);
        else
          values.push_back(std::to_string(iv));
      } else if (std::holds_alternative<bool>(val))
        values.push_back(std::get<bool>(val) ? "true" : "false");
      else if (std::holds_alternative<double>(val))
        values.push_back(std::to_string(std::get<double>(val)));
      else
        values.push_back("unknown");
    } else if (network_type_id >= 0) {
      auto partners = world.getNetworkPartners(person, network_type_id);
      if (partners.empty()) {
        values.push_back("");
      } else {
        std::string s = "[";
        for (size_t i = 0; i < partners.size(); ++i) {
          if (i > 0) s.push_back(' ');
          s += std::to_string(partners[i]);
        }
        s.push_back(']');
        values.push_back(std::move(s));
      }
    } else {
      values.push_back("");
    }
  }
  return values;
}

// Write `values` to `name` under `prop_group` as a chunked var-length string
// dataset. Creates the dataset if absent, extends + appends if present.
void writeOrAppendStringDataset(H5::Group& prop_group, const std::string& name,
                                const std::vector<std::string>& values) {
  hsize_t out_dims[1] = {values.size()};
  hsize_t out_maxdims[1] = {H5S_UNLIMITED};
  H5::DataSpace out_space(1, out_dims, out_maxdims);
  H5::StrType out_type(H5::PredType::C_S1, H5T_VARIABLE);

  H5::DSetCreatPropList plist;
  hsize_t chunk_dims[1] = {std::min(out_dims[0], hsize_t(100000))};
  if (chunk_dims[0] == 0) chunk_dims[0] = 1;
  plist.setChunk(1, chunk_dims);

  std::vector<const char*> pc_strs;
  pc_strs.reserve(values.size());
  for (const auto& s : values) pc_strs.push_back(s.c_str());

  H5::DataSet pds;
  if (H5Lexists(prop_group.getId(), name.c_str(), H5P_DEFAULT)) {
    pds = prop_group.openDataSet(name);
    hsize_t current_dims[1];
    pds.getSpace().getSimpleExtentDims(current_dims);
    hsize_t new_dims[1] = {current_dims[0] + values.size()};
    pds.extend(new_dims);

    H5::DataSpace file_space = pds.getSpace();
    file_space.selectHyperslab(H5S_SELECT_SET, out_dims, current_dims);
    pds.write(pc_strs.data(), out_type, out_space, file_space);
  } else {
    pds = prop_group.createDataSet(name, out_type, out_space, plist);
    pds.write(pc_strs.data(), out_type);
  }
}

std::vector<june::detail::PersonActivityRecord> buildPersonActivityRecords(
    const june::WorldState& world,
    const std::unordered_set<june::PersonId>& people_to_save) {
  size_t total_entries = 0;
  for (const auto& person : world.people) {
    if (!people_to_save.count(person.id)) continue;
    for (const auto& meta : world.getActivityMetas(person))
      total_entries += world.getActivityVenues(meta).size();
  }
  std::vector<june::detail::PersonActivityRecord> records;
  records.reserve(total_entries);
  for (const auto& person : world.people) {
    if (!people_to_save.count(person.id)) continue;
    for (const auto& meta : world.getActivityMetas(person)) {
      if (meta.activity_index < 0 ||
          meta.activity_index >= (int16_t)world.activity_names.size())
        continue;
      const std::string& aname = world.activity_names[meta.activity_index];
      auto venues = world.getActivityVenues(meta);
      for (size_t idx = 0; idx < venues.size(); ++idx) {
        june::detail::PersonActivityRecord record;
        record.person_id = person.id;
        strncpy(record.activity_name, aname.c_str(), 63);
        record.activity_name[63] = '\0';
        record.venue_id = venues[idx].first;
        record.subset_index = venues[idx].second;
        record.activity_index = static_cast<int>(idx);
        records.push_back(record);
      }
    }
  }
  return records;
}

H5::CompType makePersonActivityCompType() {
  H5::StrType aname_type(H5::PredType::C_S1, 64);
  H5::CompType atype(sizeof(june::detail::PersonActivityRecord));
  atype.insertMember("person_id",
                     HOFFSET(june::detail::PersonActivityRecord, person_id),
                     H5::PredType::NATIVE_INT);
  atype.insertMember("activity_name",
                     HOFFSET(june::detail::PersonActivityRecord, activity_name),
                     aname_type);
  atype.insertMember("venue_id",
                     HOFFSET(june::detail::PersonActivityRecord, venue_id),
                     H5::PredType::NATIVE_INT);
  atype.insertMember("subset_index",
                     HOFFSET(june::detail::PersonActivityRecord, subset_index),
                     H5::PredType::NATIVE_INT);
  atype.insertMember("activity_index",
                     HOFFSET(june::detail::PersonActivityRecord, activity_index),
                     H5::PredType::NATIVE_INT);
  return atype;
}

// Metadata for one /lookups/population_summary "extra" property slot.
// At most 4 are kept (matches PopulationSummaryRecord::extra_codes width).
struct SummaryPropInfo {
  std::string name;
  int index;
  const std::vector<std::string>* registry = nullptr;
};

std::vector<SummaryPropInfo> collectSummaryPropertyMetadata(
    const june::WorldState& world,
    const std::vector<std::string>& summary_props) {
  std::vector<SummaryPropInfo> props_metadata;
  for (const auto& name : summary_props) {
    int idx = world.getPersonPropertyIndex(name);
    if (idx < 0) continue;
    SummaryPropInfo pi;
    pi.name = name;
    pi.index = idx;
    auto it = world.person_property_value_registries.find(name);
    if (it != world.person_property_value_registries.end())
      pi.registry = &it->second;
    props_metadata.push_back(pi);
  }
  return props_metadata;
}

std::vector<june::PopulationSummaryRecord> buildPopulationSummaryRecords(
    const june::WorldState& world,
    const std::vector<SummaryPropInfo>& props_metadata) {
  const size_t n = world.people.size();
  std::vector<june::PopulationSummaryRecord> records(n);
  for (size_t i = 0; i < n; ++i) {
    const june::Person& person = world.people[i];
    records[i].person_id = person.id;
    records[i].age_group =
        std::min(static_cast<uint8_t>(person.age / 5), uint8_t(17));
    records[i].sex_code = static_cast<uint8_t>(person.sex);
    records[i].schedule_type_code =
        static_cast<uint8_t>(person.schedule_type_id % 256);
    records[i].reserved = 0;
    records[i].geo_unit_id = person.geo_unit_id;
    for (size_t k = 0; k < 4; ++k) {
      records[i].extra_codes[k] = 0;
      if (k < props_metadata.size()) {
        auto p_opt = world.getPersonProperty(person, props_metadata[k].name);
        if (p_opt) {
          if (std::holds_alternative<int32_t>(*p_opt))
            records[i].extra_codes[k] =
                (uint8_t)(std::get<int32_t>(*p_opt) % 256);
          else if (std::holds_alternative<bool>(*p_opt))
            records[i].extra_codes[k] = std::get<bool>(*p_opt) ? 1 : 0;
        }
      }
    }
  }
  return records;
}

H5::CompType makePopulationSummaryCompType() {
  H5::CompType ptype(sizeof(june::PopulationSummaryRecord));
  ptype.insertMember("person_id",
                     HOFFSET(june::PopulationSummaryRecord, person_id),
                     H5::PredType::NATIVE_INT);
  ptype.insertMember("age_group",
                     HOFFSET(june::PopulationSummaryRecord, age_group),
                     H5::PredType::NATIVE_UINT8);
  ptype.insertMember("sex_code",
                     HOFFSET(june::PopulationSummaryRecord, sex_code),
                     H5::PredType::NATIVE_UINT8);
  ptype.insertMember("schedule_type_code",
                     HOFFSET(june::PopulationSummaryRecord, schedule_type_code),
                     H5::PredType::NATIVE_UINT8);
  ptype.insertMember("reserved",
                     HOFFSET(june::PopulationSummaryRecord, reserved),
                     H5::PredType::NATIVE_UINT8);
  ptype.insertMember("geo_unit_id",
                     HOFFSET(june::PopulationSummaryRecord, geo_unit_id),
                     H5::PredType::NATIVE_INT);
  hsize_t adims[1] = {4};
  H5::ArrayType atype(H5::PredType::NATIVE_UINT8, 1, adims);
  ptype.insertMember("extra_codes",
                     HOFFSET(june::PopulationSummaryRecord, extra_codes),
                     atype);
  return ptype;
}

H5::CompType makePersonCompType() {
  H5::StrType sex_type(H5::PredType::C_S1, 16);
  H5::StrType schedule_type(H5::PredType::C_S1, 64);
  H5::CompType person_type(sizeof(june::detail::PersonRecord));
  person_type.insertMember("person_id",
                           HOFFSET(june::detail::PersonRecord, person_id),
                           H5::PredType::NATIVE_INT);
  person_type.insertMember("age", HOFFSET(june::detail::PersonRecord, age),
                           H5::PredType::NATIVE_DOUBLE);
  person_type.insertMember("sex", HOFFSET(june::detail::PersonRecord, sex),
                           sex_type);
  person_type.insertMember("geo_unit_id",
                           HOFFSET(june::detail::PersonRecord, geo_unit_id),
                           H5::PredType::NATIVE_INT);
  person_type.insertMember("is_dead",
                           HOFFSET(june::detail::PersonRecord, is_dead),
                           H5::PredType::NATIVE_INT);
  person_type.insertMember("death_time",
                           HOFFSET(june::detail::PersonRecord, death_time),
                           H5::PredType::NATIVE_DOUBLE);
  person_type.insertMember("schedule_type",
                           HOFFSET(june::detail::PersonRecord, schedule_type),
                           schedule_type);
  person_type.insertMember("num_activities",
                           HOFFSET(june::detail::PersonRecord, num_activities),
                           H5::PredType::NATIVE_INT);
  person_type.insertMember(
      "num_residence_venues",
      HOFFSET(june::detail::PersonRecord, num_residence_venues),
      H5::PredType::NATIVE_INT);
  person_type.insertMember(
      "num_primary_activities",
      HOFFSET(june::detail::PersonRecord, num_primary_activities),
      H5::PredType::NATIVE_INT);
  person_type.insertMember(
      "num_leisure_venues",
      HOFFSET(june::detail::PersonRecord, num_leisure_venues),
      H5::PredType::NATIVE_INT);
  person_type.insertMember(
      "num_medical_facilities",
      HOFFSET(june::detail::PersonRecord, num_medical_facilities),
      H5::PredType::NATIVE_INT);
  return person_type;
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
    H5::Group lookups_group = openOrCreateGroup(file, "/lookups");

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

    H5::Group metadata_group = openOrCreateGroup(file, "/metadata");
    H5::Group registries_group =
        openOrCreateGroup(metadata_group, "registries");

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

void EventWriter::writePersonLookupTable(
    H5::H5File& file, const WorldState& world, const Config& config,
    const EventLogger& logger, bool append,
    const std::unordered_set<PersonId>* person_ids_filter) {
  auto people_to_save = selectPeopleToSave(
      world, config.simulation.save_full_person_details, logger, append,
      person_ids_filter);
  if (people_to_save.empty()) return;

  auto records = buildPersonRecords(world, people_to_save);
  auto person_type = makePersonCompType();
  writeDatasetTemplate(file, "/lookups/people", records, person_type,
                       config.simulation.compression_level);

  if (!world.person_property_names.empty()) {
    H5::Group prop_group =
        openOrCreateGroup(file, "/lookups/people_properties");
    for (const auto& key : world.person_property_names) {
      auto values = collectPropertyValues(world, people_to_save, key);
      writeOrAppendStringDataset(prop_group, key, values);
    }
  }
}

void EventWriter::writeVenueLookupTable(H5::H5File& file,
                                        const WorldState& world,
                                        const Config& config) {
  size_t n = world.venues.size();
  std::vector<detail::VenueRecord> records(n + 1);
  records[0].venue_id = INFECTION_SEED_VENUE_ID;
  strncpy(records[0].name, "infection_seed", 127);
  records[0].name[127] = '\0';
  strncpy(records[0].type, "infection_seed", 63);
  records[0].type[63] = '\0';
  records[0].geo_unit_id = -1;
  records[0].n_subsets = 0;

  for (size_t i = 0; i < n; ++i) {
    const Venue& venue = world.venues[i];
    records[i + 1].venue_id = venue.id;
    std::string vname =
        (venue.id < 0 && venue.id != INFECTION_SEED_VENUE_ID)
            ? "Virtual Coordinated Site (Rank " +
                  std::to_string((-(static_cast<long long>(venue.id) + 1000)) %
                                 1000000) +
                  ")"
            : "Venue_" + std::to_string(venue.id);
    strncpy(records[i + 1].name, vname.c_str(), 127);
    records[i + 1].name[127] = '\0';
    std::string vtype = (venue.id < 0 && venue.id != INFECTION_SEED_VENUE_ID)
                            ? "coordinated_encounter"
                            : (venue.type_id < world.venue_type_names.size()
                                   ? world.venue_type_names[venue.type_id]
                                   : "unknown");
    strncpy(records[i + 1].type, vtype.c_str(), 63);
    records[i + 1].type[63] = '\0';
    records[i + 1].geo_unit_id = venue.geo_unit_id;
    records[i + 1].n_subsets = static_cast<int>(venue.subset_count);
  }

  H5::StrType ntype(H5::PredType::C_S1, 128);
  H5::StrType ttype(H5::PredType::C_S1, 64);
  H5::CompType vtype(sizeof(detail::VenueRecord));
  vtype.insertMember("venue_id", HOFFSET(detail::VenueRecord, venue_id),
                     H5::PredType::NATIVE_INT);
  vtype.insertMember("name", HOFFSET(detail::VenueRecord, name), ntype);
  vtype.insertMember("type", HOFFSET(detail::VenueRecord, type), ttype);
  vtype.insertMember("geo_unit_id", HOFFSET(detail::VenueRecord, geo_unit_id),
                     H5::PredType::NATIVE_INT);
  vtype.insertMember("n_subsets", HOFFSET(detail::VenueRecord, n_subsets),
                     H5::PredType::NATIVE_INT);

  hsize_t vdims[1] = {n + 1};
  H5::DataSpace vspace(1, vdims);
  H5::DataSet vds =
      createCompressedDataset(file, "/lookups/venues", vtype, vspace,
                              config.simulation.compression_level);
  vds.write(records.data(), vtype);
}

void EventWriter::writePersonActivitiesTable(
    H5::H5File& file, const WorldState& world, const Config& config,
    const EventLogger& logger, bool append,
    const std::unordered_set<PersonId>* person_ids_filter) {
  auto people_to_save = selectPeopleToSave(
      world, config.simulation.save_person_activities, logger, append,
      person_ids_filter);
  if (people_to_save.empty()) return;

  auto records = buildPersonActivityRecords(world, people_to_save);
  if (records.empty()) return;
  auto atype = makePersonActivityCompType();
  writeDatasetTemplate(file, "/lookups/person_activities", records, atype,
                       config.simulation.compression_level);
}

void EventWriter::writePopulationSummary(H5::H5File& file,
                                         const WorldState& world,
                                         const Config& config) {
  const size_t n = world.people.size();
  if (n == 0) return;

  std::vector<std::string> summary_props = config.simulation.summary_properties;
  if (summary_props.size() > 4) summary_props.resize(4);

  auto props_metadata = collectSummaryPropertyMetadata(world, summary_props);
  auto records = buildPopulationSummaryRecords(world, props_metadata);
  auto ptype = makePopulationSummaryCompType();

  hsize_t pdims[1] = {n};
  H5::DataSpace pspace(1, pdims);
  H5::DataSet pds =
      createCompressedDataset(file, "/lookups/population_summary", ptype,
                              pspace, config.simulation.compression_level);
  pds.write(records.data(), ptype);

  for (size_t k = 0; k < props_metadata.size(); ++k) {
    H5::StrType stype(H5::PredType::C_S1, H5T_VARIABLE);
    H5::Attribute attr = pds.createAttribute("extra_prop_" + std::to_string(k),
                                             stype, H5::DataSpace(H5S_SCALAR));
    attr.write(stype, props_metadata[k].name);
  }
}

void EventWriter::writePopulationNetworks(H5::H5File& file,
                                          const WorldState& world,
                                          const Config& config) {
  if (world.people.empty() || world.network_names.empty()) return;

  H5::Group lookups_group = openOrCreateGroup(file, "/lookups");
  H5::Group networks_group =
      openOrCreateGroup(lookups_group, "population_networks");

  for (const auto& network_name : world.network_names) {
    const int type_id = world.getNetworkTypeIndex(network_name);
    if (type_id < 0) continue;

    std::vector<int32_t> persons;
    std::vector<int32_t> partners;
    persons.reserve(world.people.size());
    partners.reserve(world.people.size());

    bool has_any = false;
    for (const auto& person : world.people) {
      auto partner_ids = world.getNetworkPartners(person, type_id);
      if (partner_ids.empty()) continue;
      has_any = true;
      for (const auto& pid : partner_ids) {
        persons.push_back(person.id);
        partners.push_back(static_cast<int32_t>(pid));
      }
    }
    if (!has_any) continue;

    H5::Group net_group = openOrCreateGroup(networks_group, network_name);

    hsize_t dims[1] = {persons.size()};
    H5::DataSpace space(1, dims);
    H5::DSetCreatPropList plist;
    hsize_t chunk_dims[1] = {std::min(dims[0], hsize_t(100000))};
    if (chunk_dims[0] == 0) chunk_dims[0] = 1;
    plist.setChunk(1, chunk_dims);
    plist.setDeflate(config.simulation.compression_level);

    if (H5Lexists(net_group.getId(), "person_id", H5P_DEFAULT))
      net_group.unlink("person_id");
    if (H5Lexists(net_group.getId(), "partner_id", H5P_DEFAULT))
      net_group.unlink("partner_id");

    H5::DataSet p_ds = net_group.createDataSet(
        "person_id", H5::PredType::NATIVE_INT32, space, plist);
    p_ds.write(persons.data(), H5::PredType::NATIVE_INT32);

    H5::DataSet q_ds = net_group.createDataSet(
        "partner_id", H5::PredType::NATIVE_INT32, space, plist);
    q_ds.write(partners.data(), H5::PredType::NATIVE_INT32);
  }
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
