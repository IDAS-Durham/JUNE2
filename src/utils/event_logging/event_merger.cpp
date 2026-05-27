#include "utils/event_logging/event_merger.h"

#include <iostream>
#include <stdexcept>

namespace june {

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

}  // namespace june
