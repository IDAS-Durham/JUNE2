#include "utils/event_logging/event_merger.h"

#include <iostream>
#include <stdexcept>
#include <unordered_set>

namespace june {

namespace {

// Open `name` under `parent` if it exists, otherwise create it.
// `parent` may be either H5::H5File or H5::Group.
template <typename Parent>
H5::Group openOrCreateGroup(Parent& parent, const std::string& name) {
  if (H5Lexists(parent.getId(), name.c_str(), H5P_DEFAULT))
    return parent.openGroup(name);
  return parent.createGroup(name);
}

// Extend `out_ds` by buffer.size() rows, write buffer at the tail, then
// clear it. `total_written` is the running output row count and must
// already include the rows about to be flushed.
template <typename T>
void flushBufferToDataset(std::vector<T>& buffer, H5::DataSet& out_ds,
                          const H5::CompType& type, hsize_t total_written) {
  hsize_t new_size[1] = {total_written};
  out_ds.extend(new_size);
  H5::DataSpace out_space = out_ds.getSpace();
  hsize_t count[1] = {buffer.size()};
  hsize_t offset[1] = {total_written - buffer.size()};
  out_space.selectHyperslab(H5S_SELECT_SET, count, offset);
  H5::DataSpace mem_space(1, count);
  out_ds.write(buffer.data(), type, mem_space, out_space);
  buffer.clear();
}

H5::CompType buildPeopleCompType() {
  H5::StrType sex_type(H5::PredType::C_S1, 16);
  H5::StrType schedule_type(H5::PredType::C_S1, 64);
  H5::CompType type(sizeof(detail::PersonRecord));
  type.insertMember("person_id", HOFFSET(detail::PersonRecord, person_id),
                    H5::PredType::NATIVE_INT);
  type.insertMember("age", HOFFSET(detail::PersonRecord, age),
                    H5::PredType::NATIVE_DOUBLE);
  type.insertMember("sex", HOFFSET(detail::PersonRecord, sex), sex_type);
  type.insertMember("geo_unit_id", HOFFSET(detail::PersonRecord, geo_unit_id),
                    H5::PredType::NATIVE_INT);
  type.insertMember("is_dead", HOFFSET(detail::PersonRecord, is_dead),
                    H5::PredType::NATIVE_INT);
  type.insertMember("death_time", HOFFSET(detail::PersonRecord, death_time),
                    H5::PredType::NATIVE_DOUBLE);
  type.insertMember("schedule_type",
                    HOFFSET(detail::PersonRecord, schedule_type),
                    schedule_type);
  type.insertMember("num_activities",
                    HOFFSET(detail::PersonRecord, num_activities),
                    H5::PredType::NATIVE_INT);
  type.insertMember("num_residence_venues",
                    HOFFSET(detail::PersonRecord, num_residence_venues),
                    H5::PredType::NATIVE_INT);
  type.insertMember("num_primary_activities",
                    HOFFSET(detail::PersonRecord, num_primary_activities),
                    H5::PredType::NATIVE_INT);
  type.insertMember("num_leisure_venues",
                    HOFFSET(detail::PersonRecord, num_leisure_venues),
                    H5::PredType::NATIVE_INT);
  type.insertMember("num_medical_facilities",
                    HOFFSET(detail::PersonRecord, num_medical_facilities),
                    H5::PredType::NATIVE_INT);
  return type;
}

// Stream unique PersonRecords from the input files into `out_ds`,
// deduplicated by person_id. Populates `id_to_merged_idx` mapping each
// input person_id to its row index in the merged output. Returns the
// number of unique records written.
hsize_t streamUniquePeople(H5::DataSet& out_ds, const H5::CompType& type,
                           const std::vector<std::string>& input_files,
                           std::vector<int32_t>& id_to_merged_idx) {
  std::vector<uint8_t> seen;
  hsize_t total_unique = 0;
  const size_t CHUNK_SIZE = 100000;
  std::vector<detail::PersonRecord> unique_buffer;

  for (const auto& f : input_files) {
    try {
      H5::H5File file(f, H5F_ACC_RDONLY);
      if (!H5Lexists(file.getId(), "/lookups/people", H5P_DEFAULT)) continue;
      H5::DataSet ds = file.openDataSet("/lookups/people");
      H5::DataSpace in_space = ds.getSpace();
      hsize_t in_dims[1];
      in_space.getSimpleExtentDims(in_dims);

      hsize_t in_count = in_dims[0];
      for (hsize_t offset = 0; offset < in_count; offset += CHUNK_SIZE) {
        hsize_t count = std::min(hsize_t(CHUNK_SIZE), in_count - offset);
        std::vector<detail::PersonRecord> chunk(count);

        hsize_t count_h[1] = {count};
        hsize_t offset_h[1] = {offset};
        in_space.selectHyperslab(H5S_SELECT_SET, count_h, offset_h);
        H5::DataSpace mem_space(1, count_h);
        ds.read(chunk.data(), type, mem_space, in_space);

        for (const auto& r : chunk) {
          int rid = r.person_id;
          if (rid >= (int)seen.size()) seen.resize(rid + 1, 0);
          if (rid >= (int)id_to_merged_idx.size())
            id_to_merged_idx.resize(rid + 1, -1);

          if (!seen[rid]) {
            seen[rid] = 1;
            id_to_merged_idx[rid] = (int32_t)total_unique;
            unique_buffer.push_back(r);
            total_unique++;

            if (unique_buffer.size() >= CHUNK_SIZE) {
              flushBufferToDataset(unique_buffer, out_ds, type, total_unique);
            }
          }
        }
      }
    } catch (...) {
    }
  }

  if (!unique_buffer.empty()) {
    flushBufferToDataset(unique_buffer, out_ds, type, total_unique);
  }
  return total_unique;
}

void collectPeoplePropertyKeys(const std::vector<std::string>& input_files,
                               std::unordered_set<std::string>& keys) {
  for (const auto& f : input_files) {
    try {
      H5::H5File file(f, H5F_ACC_RDONLY);
      if (H5Lexists(file.getId(), "/lookups/people_properties", H5P_DEFAULT)) {
        H5::Group group = file.openGroup("/lookups/people_properties");
        hsize_t n = group.getNumObjs();
        for (hsize_t i = 0; i < n; ++i) {
          keys.insert(group.getObjnameByIdx(i));
        }
      }
    } catch (...) {
    }
  }
}

// Read property `key` values from one input file and scatter them into
// `merged_props` at positions given by id_to_merged_idx.
void readPropertyValuesFromFile(
    const std::string& f, const std::string& key,
    const H5::CompType& id_only_type,
    const std::vector<int32_t>& id_to_merged_idx,
    std::vector<std::string>& merged_props) {
  const size_t CHUNK_SIZE = 100000;
  try {
    H5::H5File file(f, H5F_ACC_RDONLY);
    if (!H5Lexists(file.getId(), "/lookups/people", H5P_DEFAULT)) return;
    if (!H5Lexists(file.getId(),
                   ("/lookups/people_properties/" + key).c_str(),
                   H5P_DEFAULT))
      return;

    H5::DataSet id_ds = file.openDataSet("/lookups/people");
    H5::DataSpace id_space = id_ds.getSpace();
    hsize_t dims[1];
    id_space.getSimpleExtentDims(dims);
    hsize_t in_count = dims[0];

    H5::DataSet prop_ds =
        file.openDataSet("/lookups/people_properties/" + key);
    H5::DataSpace prop_space = prop_ds.getSpace();
    H5::StrType prop_type = prop_ds.getStrType();

    for (hsize_t offset = 0; offset < in_count; offset += CHUNK_SIZE) {
      hsize_t count = std::min(hsize_t(CHUNK_SIZE), in_count - offset);
      std::vector<int> chunk_ids(count);
      hsize_t count_h[1] = {count};
      hsize_t offset_h[1] = {offset};
      id_space.selectHyperslab(H5S_SELECT_SET, count_h, offset_h);
      H5::DataSpace mem_space(1, count_h);
      id_ds.read(chunk_ids.data(), id_only_type, mem_space, id_space);

      std::vector<std::string> chunk_values(count);
      prop_space.selectHyperslab(H5S_SELECT_SET, count_h, offset_h);
      if (prop_type.isVariableStr()) {
        std::vector<char*> rdata(count);
        prop_ds.read(rdata.data(), prop_type, mem_space, prop_space);
        for (size_t i = 0; i < count; ++i)
          if (rdata[i]) chunk_values[i] = rdata[i];
        H5::DataSet::vlenReclaim(rdata.data(), prop_type, mem_space);
      } else {
        size_t s = prop_type.getSize();
        std::vector<char> buf(count * s);
        prop_ds.read(buf.data(), prop_type, mem_space, prop_space);
        for (size_t i = 0; i < count; ++i) {
          chunk_values[i] = std::string(&buf[i * s], s);
          size_t p = chunk_values[i].find('\0');
          if (p != std::string::npos) chunk_values[i].resize(p);
        }
      }

      for (size_t i = 0; i < count; ++i) {
        int rid = chunk_ids[i];
        if (rid >= 0 && rid < (int)id_to_merged_idx.size()) {
          int32_t merge_idx = id_to_merged_idx[rid];
          if (merge_idx != -1) merged_props[merge_idx] = chunk_values[i];
        }
      }
    }
  } catch (...) {
  }
}

H5::CompType buildPopulationSummaryCompType() {
  H5::CompType type(sizeof(PopulationSummaryRecord));
  type.insertMember("person_id", HOFFSET(PopulationSummaryRecord, person_id),
                    H5::PredType::NATIVE_INT);
  type.insertMember("age_group", HOFFSET(PopulationSummaryRecord, age_group),
                    H5::PredType::NATIVE_UINT8);
  type.insertMember("sex_code", HOFFSET(PopulationSummaryRecord, sex_code),
                    H5::PredType::NATIVE_UINT8);
  type.insertMember("schedule_type_code",
                    HOFFSET(PopulationSummaryRecord, schedule_type_code),
                    H5::PredType::NATIVE_UINT8);
  type.insertMember("reserved", HOFFSET(PopulationSummaryRecord, reserved),
                    H5::PredType::NATIVE_UINT8);
  type.insertMember("geo_unit_id",
                    HOFFSET(PopulationSummaryRecord, geo_unit_id),
                    H5::PredType::NATIVE_INT);
  hsize_t extra_dims[1] = {4};
  H5::ArrayType extra_type(H5::PredType::NATIVE_UINT8, 1, extra_dims);
  type.insertMember("extra_codes",
                    HOFFSET(PopulationSummaryRecord, extra_codes), extra_type);
  return type;
}

// Stream unique PopulationSummaryRecords from input files into `out_ds`,
// deduplicated by person_id (first occurrence wins). Returns count of
// unique records written.
hsize_t streamUniquePopulationSummary(
    H5::DataSet& out_ds, const H5::CompType& type,
    const std::vector<std::string>& input_files) {
  std::vector<uint8_t> seen;
  hsize_t total_unique = 0;
  const size_t CHUNK_SIZE = 100000;
  std::vector<PopulationSummaryRecord> unique_buffer;

  for (const auto& f : input_files) {
    try {
      H5::H5File file(f, H5F_ACC_RDONLY);
      if (!H5Lexists(file.getId(), "/lookups/population_summary", H5P_DEFAULT))
        continue;
      H5::DataSet ds = file.openDataSet("/lookups/population_summary");
      H5::DataSpace in_space = ds.getSpace();
      hsize_t in_dims[1];
      in_space.getSimpleExtentDims(in_dims);

      hsize_t in_count = in_dims[0];
      for (hsize_t offset = 0; offset < in_count; offset += CHUNK_SIZE) {
        hsize_t count = std::min(hsize_t(CHUNK_SIZE), in_count - offset);
        std::vector<PopulationSummaryRecord> chunk(count);
        hsize_t count_h[1] = {count};
        hsize_t offset_h[1] = {offset};
        in_space.selectHyperslab(H5S_SELECT_SET, count_h, offset_h);
        H5::DataSpace mem_space(1, count_h);
        ds.read(chunk.data(), type, mem_space, in_space);

        for (const auto& r : chunk) {
          int rid = r.person_id;
          if (rid >= (int)seen.size()) seen.resize(rid + 1, 0);
          if (!seen[rid]) {
            seen[rid] = 1;
            unique_buffer.push_back(r);
            total_unique++;
            if (unique_buffer.size() >= CHUNK_SIZE) {
              flushBufferToDataset(unique_buffer, out_ds, type, total_unique);
            }
          }
        }
      }
    } catch (...) {
    }
  }

  if (!unique_buffer.empty()) {
    flushBufferToDataset(unique_buffer, out_ds, type, total_unique);
  }
  return total_unique;
}

void mergeOnePeopleProperty(const std::string& key,
                            const std::vector<std::string>& input_files,
                            const std::vector<int32_t>& id_to_merged_idx,
                            hsize_t total_unique, H5::Group& prop_group) {
  H5::CompType id_only_type(sizeof(int));
  id_only_type.insertMember("person_id", 0, H5::PredType::NATIVE_INT);

  std::vector<std::string> merged_props(total_unique, "unknown");
  for (const auto& f : input_files) {
    readPropertyValuesFromFile(f, key, id_only_type, id_to_merged_idx,
                               merged_props);
  }

  hsize_t out_dims[1] = {total_unique};
  H5::DataSpace out_space(1, out_dims);
  H5::StrType out_type(H5::PredType::C_S1, H5T_VARIABLE);
  H5::DataSet ds = prop_group.createDataSet(key, out_type, out_space);
  std::vector<const char*> c_strs;
  for (const auto& s : merged_props) c_strs.push_back(s.c_str());
  ds.write(c_strs.data(), out_type);
  std::cout << "    - Merged property: " << key << std::endl;
}

}  // namespace

void EventMerger::mergeEventFiles(const std::vector<std::string>& input_files,
                                  const std::string& output_file) {
  std::cout << "\n=== Merging Event Files ===" << std::endl;
  std::cout << "Input files: " << input_files.size() << std::endl;
  std::cout << "Output file: " << output_file << std::endl;

  try {
    H5::H5File out_file(output_file, H5F_ACC_TRUNC);
    out_file.createGroup("/events");
    out_file.createGroup("/lookups");

    mergeInfectionEvents(out_file, input_files);
    mergeSymptomChangeEvents(out_file, input_files);
    mergeDeathEvents(out_file, input_files);
    mergeHospitalAdmissionEvents(out_file, input_files);
    mergeICUAdmissionEvents(out_file, input_files);
    mergeHospitalDischargeEvents(out_file, input_files);
    mergeVaccinationEvents(out_file, input_files);
    mergeRelationshipEvents(out_file, input_files);
    mergeCoordinatedEncounterEvents(out_file, input_files);

    mergePeopleLookup(out_file, input_files);
    mergeVenueLookup(out_file, input_files);
    mergePersonActivityLookup(out_file, input_files);
    mergePopulationSummary(out_file, input_files);
    mergeProfileAssignments(out_file, input_files);
    mergePopulationNetworks(out_file, input_files);

    // Copy metadata (registries) from first input file
    if (!input_files.empty()) {
      try {
        H5::H5File first_file(input_files[0], H5F_ACC_RDONLY);
        if (H5Lexists(first_file.getId(), "/metadata", H5P_DEFAULT)) {
          H5Ocopy(first_file.getId(), "/metadata", out_file.getId(),
                  "/metadata", H5P_DEFAULT, H5P_DEFAULT);
          std::cout << "  Copied /metadata registries from " << input_files[0]
                    << std::endl;
        }
      } catch (const H5::Exception& e) {
        std::cerr << "Warning: Could not copy metadata: " << e.getDetailMsg()
                  << std::endl;
      }
    }

    std::cout << "\nMerge complete!" << std::endl;
  } catch (const H5::Exception& e) {
    std::cerr << "Error writing merged file: " << e.getDetailMsg() << std::endl;
    throw std::runtime_error("Failed to write merged event file");
  }
}

void EventMerger::mergeInfectionEvents(
    H5::H5File& out_file, const std::vector<std::string>& input_files) {
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
  mergeDatasetTemplate<InfectionEvent>(out_file, "/events/infections",
                                       input_files, type);
}

void EventMerger::mergeSymptomChangeEvents(
    H5::H5File& out_file, const std::vector<std::string>& input_files) {
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
  mergeDatasetTemplate<detail::SymptomChangeRecord>(
      out_file, "/events/symptom_changes", input_files, type);
}

void EventMerger::mergeDeathEvents(
    H5::H5File& out_file, const std::vector<std::string>& input_files) {
  H5::CompType type(sizeof(DeathEvent));
  type.insertMember("person_id", HOFFSET(DeathEvent, person_id),
                    H5::PredType::NATIVE_INT);
  type.insertMember("venue_id", HOFFSET(DeathEvent, venue_id),
                    H5::PredType::NATIVE_INT);
  type.insertMember("time", HOFFSET(DeathEvent, time),
                    H5::PredType::NATIVE_DOUBLE);
  mergeDatasetTemplate<DeathEvent>(out_file, "/events/deaths", input_files,
                                   type);
}

void EventMerger::mergeHospitalAdmissionEvents(
    H5::H5File& out_file, const std::vector<std::string>& input_files) {
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
  mergeDatasetTemplate<detail::HospitalAdmissionRecord>(
      out_file, "/events/hospital_admissions", input_files, type);
}

void EventMerger::mergeICUAdmissionEvents(
    H5::H5File& out_file, const std::vector<std::string>& input_files) {
  H5::CompType type(sizeof(ICUAdmissionEvent));
  type.insertMember("person_id", HOFFSET(ICUAdmissionEvent, person_id),
                    H5::PredType::NATIVE_INT);
  type.insertMember("hospital_id", HOFFSET(ICUAdmissionEvent, hospital_id),
                    H5::PredType::NATIVE_INT);
  type.insertMember("time", HOFFSET(ICUAdmissionEvent, time),
                    H5::PredType::NATIVE_DOUBLE);
  mergeDatasetTemplate<ICUAdmissionEvent>(out_file, "/events/icu_admissions",
                                          input_files, type);
}

void EventMerger::mergeHospitalDischargeEvents(
    H5::H5File& out_file, const std::vector<std::string>& input_files) {
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
  mergeDatasetTemplate<detail::HospitalDischargeRecord>(
      out_file, "/events/hospital_discharges", input_files, type);
}

void EventMerger::mergeVaccinationEvents(
    H5::H5File& out_file, const std::vector<std::string>& input_files) {
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
  mergeDatasetTemplate<detail::VaccinationRecord>(
      out_file, "/events/vaccinations", input_files, type);
}

void EventMerger::mergeRelationshipEvents(
    H5::H5File& out_file, const std::vector<std::string>& input_files) {
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
  mergeDatasetTemplate<detail::RelationshipRecord>(
      out_file, "/events/relationships", input_files, type);
  std::cout << "  Merged /events/relationships" << std::endl;
}

void EventMerger::mergeCoordinatedEncounterEvents(
    H5::H5File& out_file, const std::vector<std::string>& input_files) {
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
  mergeDatasetTemplate<detail::CoordinatedEncounterRecord>(
      out_file, "/events/coordinated_encounters", input_files, type);
  std::cout << "  Merged /events/coordinated_encounters" << std::endl;
}

void EventMerger::mergePeopleLookup(
    H5::H5File& out_file, const std::vector<std::string>& input_files) {
  if (input_files.empty()) return;

  H5::CompType type = buildPeopleCompType();

  hsize_t initial_dims[1] = {0};
  hsize_t max_dims[1] = {H5S_UNLIMITED};
  H5::DataSpace out_space(1, initial_dims, max_dims);
  H5::DSetCreatPropList plist;
  hsize_t chunk_dims[1] = {100000};
  plist.setChunk(1, chunk_dims);
  plist.setDeflate(6);
  H5::DataSet out_ds =
      out_file.createDataSet("/lookups/people", type, out_space, plist);

  std::vector<int32_t> id_to_merged_idx;
  hsize_t total_unique =
      streamUniquePeople(out_ds, type, input_files, id_to_merged_idx);
  std::cout << "  Merged " << total_unique << " unique people (streaming)"
            << std::endl;

  std::unordered_set<std::string> all_prop_keys;
  collectPeoplePropertyKeys(input_files, all_prop_keys);
  if (all_prop_keys.empty()) return;

  H5::Group prop_group = out_file.createGroup("/lookups/people_properties");
  for (const auto& key : all_prop_keys) {
    mergeOnePeopleProperty(key, input_files, id_to_merged_idx, total_unique,
                           prop_group);
  }
}

void EventMerger::mergeVenueLookup(
    H5::H5File& out_file, const std::vector<std::string>& input_files) {
  if (input_files.empty()) return;

  H5::StrType name_type(H5::PredType::C_S1, 128);
  H5::StrType type_type(H5::PredType::C_S1, 64);
  H5::CompType type(sizeof(detail::VenueRecord));
  type.insertMember("venue_id", HOFFSET(detail::VenueRecord, venue_id),
                    H5::PredType::NATIVE_INT);
  type.insertMember("name", HOFFSET(detail::VenueRecord, name), name_type);
  type.insertMember("type", HOFFSET(detail::VenueRecord, type), type_type);
  type.insertMember("geo_unit_id", HOFFSET(detail::VenueRecord, geo_unit_id),
                    H5::PredType::NATIVE_INT);
  type.insertMember("n_subsets", HOFFSET(detail::VenueRecord, n_subsets),
                    H5::PredType::NATIVE_INT);

  std::unordered_set<int> seen_venues;
  hsize_t initial_dims[1] = {0};
  hsize_t max_dims[1] = {H5S_UNLIMITED};
  H5::DataSpace out_space(1, initial_dims, max_dims);
  H5::DSetCreatPropList plist;
  hsize_t chunk_dims[1] = {100000};
  plist.setChunk(1, chunk_dims);
  plist.setDeflate(6);
  H5::DataSet out_ds =
      out_file.createDataSet("/lookups/venues", type, out_space, plist);

  hsize_t total_unique = 0;
  const size_t CHUNK_SIZE = 100000;
  std::vector<detail::VenueRecord> unique_buffer;

  for (const auto& f : input_files) {
    try {
      H5::H5File file(f, H5F_ACC_RDONLY);
      if (!H5Lexists(file.getId(), "/lookups/venues", H5P_DEFAULT)) continue;
      H5::DataSet ds = file.openDataSet("/lookups/venues");
      H5::DataSpace in_space = ds.getSpace();
      hsize_t in_dims[1];
      in_space.getSimpleExtentDims(in_dims);

      hsize_t in_count = in_dims[0];
      for (hsize_t offset = 0; offset < in_count; offset += CHUNK_SIZE) {
        hsize_t count = std::min(hsize_t(CHUNK_SIZE), in_count - offset);
        std::vector<detail::VenueRecord> chunk(count);
        hsize_t count_h[1] = {count};
        hsize_t offset_h[1] = {offset};
        in_space.selectHyperslab(H5S_SELECT_SET, count_h, offset_h);
        H5::DataSpace mem_space(1, count_h);
        ds.read(chunk.data(), type, mem_space, in_space);

        for (const auto& r : chunk) {
          if (seen_venues.insert(r.venue_id).second) {
            unique_buffer.push_back(r);
            total_unique++;
            if (unique_buffer.size() >= CHUNK_SIZE) {
              flushBufferToDataset(unique_buffer, out_ds, type, total_unique);
            }
          }
        }
      }
    } catch (...) {
    }
  }

  if (!unique_buffer.empty()) {
    flushBufferToDataset(unique_buffer, out_ds, type, total_unique);
  }
  std::cout << "  Merged " << total_unique << " unique venues (streaming)"
            << std::endl;
}

void EventMerger::mergePersonActivityLookup(
    H5::H5File& out_file, const std::vector<std::string>& input_files) {
  H5::StrType name_type(H5::PredType::C_S1, 64);
  H5::CompType type(sizeof(detail::PersonActivityRecord));
  type.insertMember("person_id",
                    HOFFSET(detail::PersonActivityRecord, person_id),
                    H5::PredType::NATIVE_INT);
  type.insertMember("activity_name",
                    HOFFSET(detail::PersonActivityRecord, activity_name),
                    name_type);
  type.insertMember("venue_id", HOFFSET(detail::PersonActivityRecord, venue_id),
                    H5::PredType::NATIVE_INT);
  type.insertMember("subset_index",
                    HOFFSET(detail::PersonActivityRecord, subset_index),
                    H5::PredType::NATIVE_INT);
  type.insertMember("activity_index",
                    HOFFSET(detail::PersonActivityRecord, activity_index),
                    H5::PredType::NATIVE_INT);
  mergeDatasetTemplate<detail::PersonActivityRecord>(
      out_file, "/lookups/person_activities", input_files, type);
}

void EventMerger::mergePopulationSummary(
    H5::H5File& out_file, const std::vector<std::string>& input_files) {
  if (input_files.empty()) return;

  H5::CompType type = buildPopulationSummaryCompType();

  hsize_t initial_dims[1] = {0};
  hsize_t max_dims[1] = {H5S_UNLIMITED};
  H5::DataSpace out_space(1, initial_dims, max_dims);
  H5::DSetCreatPropList plist;
  hsize_t chunk_dims[1] = {100000};
  plist.setChunk(1, chunk_dims);
  plist.setDeflate(6);
  H5::DataSet out_ds = out_file.createDataSet("/lookups/population_summary",
                                              type, out_space, plist);

  hsize_t total_unique =
      streamUniquePopulationSummary(out_ds, type, input_files);
  std::cout << "  Merged " << total_unique
            << " population summary records (streaming)" << std::endl;
}

void EventMerger::mergeProfileAssignments(
    H5::H5File& out_file, const std::vector<std::string>& input_files) {
  if (input_files.empty()) return;

  // 1. Discover which facets exist. Take the union across all rank files so we
  //    don't miss a facet that happens to be absent from rank 0. Preserve
  //    insertion order for deterministic output.
  std::vector<std::string> facet_names;
  std::unordered_set<std::string> facet_seen;
  for (const auto& f : input_files) {
    try {
      H5::H5File file(f, H5F_ACC_RDONLY);
      if (!H5Lexists(file.getId(), "/lookups/profile_assignments", H5P_DEFAULT))
        continue;
      H5::Group g = file.openGroup("/lookups/profile_assignments");
      hsize_t n_obj = g.getNumObjs();
      for (hsize_t i = 0; i < n_obj; ++i) {
        std::string name = g.getObjnameByIdx(i);
        if (facet_seen.insert(name).second) {
          facet_names.push_back(name);
        }
      }
    } catch (...) {
    }
  }
  if (facet_names.empty()) return;

  // 2. Ensure output parent group exists.
  H5::Group out_lookups = openOrCreateGroup(out_file, "/lookups");
  H5::Group out_assigns = openOrCreateGroup(out_lookups, "profile_assignments");

  for (const auto& facet : facet_names) {
    // 3. Discover the field set for this facet. Union across files.
    std::vector<std::string> field_names;
    std::unordered_set<std::string> field_seen;
    for (const auto& f : input_files) {
      try {
        H5::H5File file(f, H5F_ACC_RDONLY);
        std::string path = "/lookups/profile_assignments/" + facet;
        if (!H5Lexists(file.getId(), path.c_str(), H5P_DEFAULT)) continue;
        H5::Group g = file.openGroup(path);
        hsize_t n_obj = g.getNumObjs();
        for (hsize_t i = 0; i < n_obj; ++i) {
          std::string name = g.getObjnameByIdx(i);
          if (field_seen.insert(name).second) {
            field_names.push_back(name);
          }
        }
      } catch (...) {
      }
    }
    if (field_names.empty()) continue;

    H5::Group out_facet = openOrCreateGroup(out_assigns, facet);

    // 4. For every field, concatenate arrays from all rank files. MPI
    //    partitions agents across ranks so simple concatenation is correct;
    //    no deduplication is applied here.
    for (const auto& field : field_names) {
      const std::string ds_path =
          "/lookups/profile_assignments/" + facet + "/" + field;

      hsize_t total = 0;
      H5::DataType dtype;
      bool dtype_set = false;
      for (const auto& f : input_files) {
        try {
          H5::H5File file(f, H5F_ACC_RDONLY);
          if (!H5Lexists(file.getId(), ds_path.c_str(), H5P_DEFAULT)) continue;
          H5::DataSet ds = file.openDataSet(ds_path);
          hsize_t d[1];
          ds.getSpace().getSimpleExtentDims(d);
          total += d[0];
          if (!dtype_set) {
            dtype = ds.getDataType();
            dtype_set = true;
          }
        } catch (...) {
        }
      }
      if (!dtype_set || total == 0) continue;
      if (H5Lexists(out_facet.getId(), field.c_str(), H5P_DEFAULT)) continue;

      hsize_t out_dims[1] = {total};
      H5::DataSpace out_space(1, out_dims);
      H5::DSetCreatPropList plist;
      hsize_t chunk[1] = {std::min(total, hsize_t(100000))};
      if (chunk[0] == 0) chunk[0] = 1;
      plist.setChunk(1, chunk);
      plist.setDeflate(6);
      H5::DataSet out_ds =
          out_facet.createDataSet(field, dtype, out_space, plist);

      const size_t elem_size = dtype.getSize();
      std::vector<uint8_t> buffer;
      hsize_t current_out_offset = 0;
      for (const auto& f : input_files) {
        try {
          H5::H5File file(f, H5F_ACC_RDONLY);
          if (!H5Lexists(file.getId(), ds_path.c_str(), H5P_DEFAULT)) continue;
          H5::DataSet in_ds = file.openDataSet(ds_path);
          H5::DataSpace in_space = in_ds.getSpace();
          hsize_t in_dims[1];
          in_space.getSimpleExtentDims(in_dims);
          if (in_dims[0] == 0) continue;

          buffer.resize(in_dims[0] * elem_size);
          in_ds.read(buffer.data(), dtype);

          hsize_t count_h[1] = {in_dims[0]};
          hsize_t out_offset_h[1] = {current_out_offset};
          out_space.selectHyperslab(H5S_SELECT_SET, count_h, out_offset_h);
          H5::DataSpace mem_space(1, count_h);
          out_ds.write(buffer.data(), dtype, mem_space, out_space);
          current_out_offset += in_dims[0];
        } catch (...) {
        }
      }
    }
    std::cout << "  Merged profile_assignments for facet '" << facet << "' ("
              << field_names.size() << " fields)\n";
  }
}

void EventMerger::mergePopulationNetworks(
    H5::H5File& out_file, const std::vector<std::string>& input_files) {
  if (input_files.empty()) return;

  std::vector<std::string> network_names;
  std::unordered_set<std::string> net_seen;
  for (const auto& f : input_files) {
    try {
      H5::H5File file(f, H5F_ACC_RDONLY);
      if (!H5Lexists(file.getId(), "/lookups/population_networks", H5P_DEFAULT))
        continue;
      H5::Group g = file.openGroup("/lookups/population_networks");
      hsize_t n_obj = g.getNumObjs();
      for (hsize_t i = 0; i < n_obj; ++i) {
        std::string name = g.getObjnameByIdx(i);
        if (net_seen.insert(name).second) network_names.push_back(name);
      }
    } catch (...) {
    }
  }
  if (network_names.empty()) return;

  H5::Group out_lookups = openOrCreateGroup(out_file, "/lookups");
  H5::Group out_nets = openOrCreateGroup(out_lookups, "population_networks");

  for (const auto& net_name : network_names) {
    H5::Group out_net = openOrCreateGroup(out_nets, net_name);

    for (const char* field : {"person_id", "partner_id"}) {
      const std::string ds_path =
          "/lookups/population_networks/" + net_name + "/" + field;
      hsize_t total = 0;
      for (const auto& f : input_files) {
        try {
          H5::H5File file(f, H5F_ACC_RDONLY);
          if (!H5Lexists(file.getId(), ds_path.c_str(), H5P_DEFAULT)) continue;
          H5::DataSet ds = file.openDataSet(ds_path);
          hsize_t d[1];
          ds.getSpace().getSimpleExtentDims(d);
          total += d[0];
        } catch (...) {
        }
      }
      if (total == 0) continue;
      if (H5Lexists(out_net.getId(), field, H5P_DEFAULT)) continue;

      hsize_t out_dims[1] = {total};
      H5::DataSpace out_space(1, out_dims);
      H5::DSetCreatPropList plist;
      hsize_t chunk[1] = {std::min(total, hsize_t(100000))};
      if (chunk[0] == 0) chunk[0] = 1;
      plist.setChunk(1, chunk);
      plist.setDeflate(6);
      H5::DataSet out_ds = out_net.createDataSet(
          field, H5::PredType::NATIVE_INT32, out_space, plist);

      std::vector<int32_t> buffer;
      hsize_t current_out_offset = 0;
      for (const auto& f : input_files) {
        try {
          H5::H5File file(f, H5F_ACC_RDONLY);
          if (!H5Lexists(file.getId(), ds_path.c_str(), H5P_DEFAULT)) continue;
          H5::DataSet in_ds = file.openDataSet(ds_path);
          hsize_t in_dims[1];
          in_ds.getSpace().getSimpleExtentDims(in_dims);
          if (in_dims[0] == 0) continue;
          buffer.resize(in_dims[0]);
          in_ds.read(buffer.data(), H5::PredType::NATIVE_INT32);

          hsize_t count_h[1] = {in_dims[0]};
          hsize_t out_offset_h[1] = {current_out_offset};
          out_space.selectHyperslab(H5S_SELECT_SET, count_h, out_offset_h);
          H5::DataSpace mem_space(1, count_h);
          out_ds.write(buffer.data(), H5::PredType::NATIVE_INT32, mem_space,
                       out_space);
          current_out_offset += in_dims[0];
        } catch (...) {
        }
      }
    }
    std::cout << "  Merged population_networks for '" << net_name << "'\n";
  }
}

}  // namespace june
