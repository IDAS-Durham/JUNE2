#pragma once

#include <H5Cpp.h>

#include "event_types.h"

namespace june::event_lookup_schema {

inline H5::CompType person() {
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

inline H5::CompType personActivity() {
  H5::StrType activity_name_type(H5::PredType::C_S1, 64);
  H5::CompType type(sizeof(detail::PersonActivityRecord));
  type.insertMember("person_id",
                    HOFFSET(detail::PersonActivityRecord, person_id),
                    H5::PredType::NATIVE_INT);
  type.insertMember("activity_name",
                    HOFFSET(detail::PersonActivityRecord, activity_name),
                    activity_name_type);
  type.insertMember("venue_id", HOFFSET(detail::PersonActivityRecord, venue_id),
                    H5::PredType::NATIVE_INT);
  type.insertMember("subset_index",
                    HOFFSET(detail::PersonActivityRecord, subset_index),
                    H5::PredType::NATIVE_INT);
  type.insertMember("activity_index",
                    HOFFSET(detail::PersonActivityRecord, activity_index),
                    H5::PredType::NATIVE_INT);
  return type;
}

inline H5::CompType populationSummary() {
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

inline H5::CompType venue() {
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
  return type;
}

}  // namespace june::event_lookup_schema
