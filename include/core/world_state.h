#pragma once

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "epidemiology/disease.h"
#include "types.h"

namespace june {

// =============================================================================
// WorldState - Main container for the simulation world
// =============================================================================
class WorldState {
 public:
  // Data storage (Structure of Arrays style internally, but exposed as objects)
  std::vector<Person> people;
  std::vector<Venue> venues;
  std::vector<GeographicalUnit> geo_units;

  // Registries
  std::vector<std::string> activity_names;
  std::vector<std::string> venue_type_names;
  std::vector<std::string> subset_type_names;
  std::vector<std::string> network_names;
  std::vector<std::string> schedule_type_names;
  std::vector<std::string> encounter_type_names;
  std::vector<std::string> symptom_names;
  std::vector<std::string> geo_level_names;

  // Property registries
  std::vector<std::string> person_property_names;
  std::unordered_map<std::string, std::vector<std::string>>
      person_property_value_registries;
  // Each list-valued property uses one id per person. The id points into that
  // property's list registry, so flat person storage stays int32_t.
  std::unordered_map<std::string, std::vector<std::vector<std::string>>>
      person_property_list_value_registries;
  // Loading records the type so a column cannot mix scalar and list values.
  std::unordered_map<std::string, bool> person_property_is_list;
  // Empty JSON arrays stay here until the column type is known.
  std::unordered_map<std::string, std::vector<size_t>>
      person_property_pending_empty_list_values;

  std::vector<std::string> venue_property_names;
  std::unordered_map<std::string, std::vector<std::string>>
      venue_property_value_registries;

  // Helper to get indices from registries
  int getActivityIndex(const std::string& name) const {
    return indexOf(activity_names, name);
  }

  int getVenueTypeIndex(const std::string& name) const {
    return indexOf(venue_type_names, name);
  }

  int getNetworkTypeIndex(const std::string& name) const {
    return indexOf(network_names, name);
  }

  int getScheduleTypeIndex(const std::string& name) const {
    return indexOf(schedule_type_names, name);
  }

  int getEncounterTypeIndex(const std::string& name) const {
    return indexOf(encounter_type_names, name);
  }

  int getPersonPropertyIndex(const std::string& name) const {
    return indexOf(person_property_names, name);
  }

  int getVenuePropertyIndex(const std::string& name) const {
    return indexOf(venue_property_names, name);
  }

  // GLOBAL FLAT STORAGE
  // Networks: person.network_meta_start -> network_meta -> network_partners
  std::vector<Person::NetworkMeta> network_meta;
  std::vector<PersonId> network_partners;

  // Activities: person.activity_meta_start -> activity_meta -> activity_venues
  std::vector<Person::ActivityMeta> activity_meta;
  std::vector<std::pair<VenueId, SubsetIndex>> activity_venues;

  // Optional per-(person, venue) membership metadata (Design B side-table,
  // /activity_mappings/membership_metadata in HDF5). Carries per-leg fields
  // such as boarding/alighting times for route activities. Keyed by flat
  // index into activity_venues. Sparse: most assignments carry no metadata.
  std::vector<std::string> membership_field_names;
  std::vector<std::unordered_map<uint32_t, float>> membership_field_values;

  int getMembershipFieldIndex(const std::string& name) const {
    return indexOf(membership_field_names, name);
  }

  // Sentinel for "field absent for this membership". Matches the value MAY
  // writes when a (person, venue) row has no value for a given field.
  static constexpr float kMembershipFieldAbsent = -1.0f;

  float getMembershipField(uint32_t activity_venue_flat_idx,
                           int field_idx) const {
    if (field_idx < 0 ||
        field_idx >= static_cast<int>(membership_field_values.size()))
      return kMembershipFieldAbsent;
    const auto& m = membership_field_values[field_idx];
    auto it = m.find(activity_venue_flat_idx);
    return (it == m.end()) ? kMembershipFieldAbsent : it->second;
  }

  // Dynamic Properties: person.properties_start -> person_properties
  // Stores interned IDs for categorical properties and raw ints for others
  std::vector<int32_t> person_properties;

  // Pre-computed schedules: one vector per day type
  // schedule_starts[person_idx * num_day_types + dt_idx] -> start in
  // precomputed_schedules[dt_idx]
  std::vector<std::vector<ScheduleEntry>> precomputed_schedules;
  std::vector<uint32_t> schedule_starts;
  std::vector<uint16_t> schedule_counts;
  size_t num_day_types = 0;

  // Venues & Subsets
  std::vector<Subset> subsets;
  std::vector<PersonId> subset_members;
  std::vector<int32_t> venue_properties;
  // Lookup maps
  std::unordered_map<PersonId, size_t> person_index;  // id -> index in people
  std::unordered_map<VenueId, size_t> venue_index;    // id -> index in venues
  std::unordered_map<GeoUnitId, size_t>
      geo_unit_index;  // id -> index in geo_units

  // Lookup maps by name/type
  std::unordered_map<std::string, std::vector<uint32_t>>
      venues_by_type;  // type -> indices

  // Global venue maps: cover ALL venues (local + cross-rank). The HDF5 loader
  // pre-populates these before buildIndices() at every rank count, serial
  // included; for a hand-built WorldState buildGlobalVenueMaps() fills the
  // geo-unit and by-type-name ones from world.venues during buildIndices().
  // Single source of truth — getVenuesInGeoUnit() uses only these maps so the
  // candidate pool is identical in serial and parallel runs.
  //
  // venue_type_by_id is indexed by VenueId and covers every Venue in the world,
  // not just this rank's, so that kUnknownVenueTypeId means "no such Venue" and
  // never "not mine". Ids naming no Venue are holes holding
  // kUnknownVenueTypeId. Empty only for a hand-built WorldState, where
  // getVenue() is total.
  std::vector<uint8_t> venue_type_by_id;
  std::unordered_map<VenueId, GeoUnitId> global_venue_geo_unit_map;
  // type_name → sorted list of all VenueIds of that type (globally)
  std::unordered_map<std::string, std::vector<VenueId>>
      global_venues_by_type_name;

  // Helper: get venue type_id, falling back to the all-venue index for
  // cross-rank venues. Ids outside the index — past the end, a hole, or the
  // negative virtual-venue range — are unresolvable.
  uint8_t getVenueTypeId(VenueId id) const {
    const Venue* v = getVenue(id);
    if (v) return v->type_id;
    if (id < 0 || static_cast<size_t>(id) >= venue_type_by_id.size())
      return kUnknownVenueTypeId;
    return venue_type_by_id[static_cast<size_t>(id)];
  }

  // Record the type of `venue_id` in venue_type_by_id, growing it with
  // kUnknownVenueTypeId holes as needed. Negative ids name no Venue and are
  // ignored. For one-off writes; the loader sizes the index once instead.
  void setGlobalVenueType(VenueId venue_id, uint8_t type_id);

  // Geographic index: geo_unit_id -> indices of people in this unit AND all its
  // descendants
  std::unordered_map<GeoUnitId, std::vector<uint32_t>> people_by_geo_unit;

  // The units people are assigned to directly, i.e. the set of values of
  // Person::geo_unit_id. A strict subset of people_by_geo_unit's keys, which
  // also carry every ancestor of those units. Diagnostics that ask "would this
  // exclude anybody?" want this one.
  std::unordered_set<GeoUnitId> directly_inhabited_geo_units;

  // Build lookup indices (call after loading)
  void buildIndices();

  // Populate global_venue_geo_unit_map and global_venues_by_type_name from
  // world.venues. No-op if already filled by the MPI HDF5 loader.
  void buildGlobalVenueMaps();

  // Shared by WorldState::buildGlobalVenueMaps() and the MPI HDF5 loader's
  // detail::buildGlobalVenueMaps(): append venue_id to the type-name bucket,
  // falling back to "unknown" for an out-of-range type_id.
  void addVenueToTypeIndex(VenueId venue_id, uint8_t type_id);

  // Sort each global_venues_by_type_name bucket ascending by VenueId.
  void sortGlobalVenuesByTypeName();

  // Accessors
  Person* getPerson(PersonId id);
  const Person* getPerson(PersonId id) const;

  Venue* getVenue(VenueId id);
  const Venue* getVenue(VenueId id) const;

  GeographicalUnit* getGeoUnit(GeoUnitId id);
  const GeographicalUnit* getGeoUnit(GeoUnitId id) const;

  // Return ids of all venues of `venue_type_name` located in
  // `hosting_geo_unit_id` or any of its descendants. Sorted by venue_id for
  // deterministic assignment.
  std::vector<VenueId> getVenuesInGeoUnit(
      GeoUnitId hosting_geo_unit_id, const std::string& venue_type_name) const;

  // Returns id of the nearest ancestor (or self) of `id` at `level_name`.
  // Returns -1 if not found.
  GeoUnitId ancestorAtLevel(GeoUnitId id, std::string_view level_name) const;

  // Free the two all-venue maps used only by getVenuesInGeoUnit, after the OTF
  // allocator has precomputed every pool it needs. Logs freed sizes.
  // venue_type_by_id is kept for the whole run: cross-rank type lookups
  // happen on every step, and every Venue must stay nameable.
  void dropGlobalVenueMaps();

  // Get all people in a geographic unit (including descendants)
  std::vector<Person*> getPeopleInUnit(GeoUnitId id);

  // Flat networks and activities
  std::span<const Person::NetworkMeta> getNetworkMetas(const Person& p) const {
    return checkedSpan(network_meta, p.network_meta_start,
                       p.network_meta_count);
  }

  std::span<const PersonId> getNetworkPartners(
      const Person::NetworkMeta& meta) const {
    return checkedSpan(network_partners, meta.partner_start,
                       meta.partner_count);
  }

  std::span<const PersonId> getNetworkPartners(
      const Person& p, const std::string& network_name) const {
    return getNetworkPartners(p, getNetworkTypeIndex(network_name));
  }

  std::span<const PersonId> getNetworkPartners(const Person& p,
                                               int network_type_id) const {
    if (network_type_id < 0) return {};
    for (const auto& meta : getNetworkMetas(p)) {
      if (meta.network_type_id == (uint16_t)network_type_id)
        return getNetworkPartners(meta);
    }
    return {};
  }

  std::span<const Person::ActivityMeta> getActivityMetas(
      const Person& p) const {
    return checkedSpan(activity_meta, p.activity_meta_start,
                       p.activity_meta_count);
  }

  std::span<const std::pair<VenueId, SubsetIndex>> getActivityVenues(
      const Person::ActivityMeta& meta) const {
    return checkedSpan(activity_venues, meta.venue_start, meta.venue_count);
  }

  // Venue/Subset accessors
  std::span<const Subset> getSubsets(const Venue& v) const {
    return checkedSpan(subsets, v.subset_start, v.subset_count);
  }

  std::span<const PersonId> getSubsetMembers(const Subset& s) const {
    return checkedSpan(subset_members, s.member_start, s.member_count);
  }

  std::span<const int32_t> getVenueProperties(const Venue& v) const {
    return checkedSpan(venue_properties, v.properties_start,
                       v.properties_count);
  }

  std::span<const std::pair<VenueId, SubsetIndex>> getActivityVenues(
      const Person& p, const std::string& act_name) const {
    int act_idx = getActivityIndex(act_name);
    return getActivityVenues(p, static_cast<int16_t>(act_idx));
  }

  std::span<const std::pair<VenueId, SubsetIndex>> getActivityVenues(
      const Person& p, int16_t act_idx) const {
    if (act_idx < 0) return {};
    for (const auto& meta : getActivityMetas(p)) {
      if (meta.activity_index == act_idx) return getActivityVenues(meta);
    }
    return {};
  }

  // Accessors
  std::span<const int32_t> getPersonProperties(const Person& p) const {
    return checkedSpan(person_properties, p.properties_start,
                       p.properties_count);
  }

  std::optional<PropertyValue> getPersonProperty(
      const Person& p, const std::string& name) const {
    int idx = getPersonPropertyIndex(name);
    if (idx < 0 || idx >= p.properties_count) return std::nullopt;
    size_t abs_idx = p.properties_start + idx;
    if (abs_idx >= person_properties.size()) return std::nullopt;

    int32_t raw_val = person_properties[abs_idx];
    if (raw_val == -1) return std::nullopt;  // monostate/null

    auto list_reg = person_property_list_value_registries.find(name);
    if (list_reg != person_property_list_value_registries.end()) {
      if (raw_val >= 0 &&
          static_cast<size_t>(raw_val) < list_reg->second.size())
        return list_reg->second[raw_val];
      return std::nullopt;
    }

    // Check if this property has a registry (categorical)
    auto it_reg = person_property_value_registries.find(name);
    if (it_reg != person_property_value_registries.end()) {
      if (raw_val >= 0 && (size_t)raw_val < it_reg->second.size()) {
        return it_reg->second[raw_val];
      }
    }
    return raw_val;  // return as int if no registry
  }

  std::span<const ScheduleEntry> getSchedule(const Person& p,
                                             int day_type_idx) const {
    if (num_day_types == 0 || day_type_idx < 0 ||
        day_type_idx >= static_cast<int>(num_day_types))
      return {};
    size_t person_idx = static_cast<size_t>(&p - people.data());
    size_t idx = person_idx * num_day_types + day_type_idx;
    if (idx >= schedule_starts.size()) return {};
    uint32_t start = schedule_starts[idx];
    uint16_t count = schedule_counts[idx];
    if (count == 0) return {};
    const auto& dt_schedules = precomputed_schedules[day_type_idx];
    if (start >= dt_schedules.size() || start + count > dt_schedules.size())
      return {};
    return std::span(dt_schedules.data() + start, count);
  }

  // Statistics
  void printSummary() const;

 private:
  template <typename T, typename Start, typename Count>
  static std::span<const T> checkedSpan(const std::vector<T>& storage,
                                        Start start, Count count) {
    const size_t offset = static_cast<size_t>(start);
    const size_t length = static_cast<size_t>(count);
    if (length == 0 || offset >= storage.size() ||
        length > storage.size() - offset)
      return {};
    return std::span<const T>(storage.data() + offset, length);
  }

  static int indexOf(const std::vector<std::string>& names,
                     const std::string& name) {
    auto it = std::find(names.begin(), names.end(), name);
    return it == names.end()
               ? -1
               : static_cast<int>(std::distance(names.begin(), it));
  }
};

}  // namespace june
