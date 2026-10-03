#include <iostream>
#include <unordered_set>

#include "utils/event_logging/event_lookup_schemas.h"
#include "utils/event_logging/event_merger.h"
#include "utils/event_logging/event_writer_detail.h"

namespace june {

namespace {

using event_writer_detail::openOrCreateGroup;

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

// Stream unique records from the input files into `out_ds`, deduplicated by
// the key returned by `key_fn`. `on_unique` runs for each first occurrence.
template <typename Record, typename Key, typename KeyFn, typename OnUnique>
hsize_t streamUniqueRecords(H5::DataSet& out_ds, const H5::CompType& type,
                            const std::vector<std::string>& input_files,
                            const std::string& dataset_path, KeyFn key_fn,
                            OnUnique on_unique) {
  std::unordered_set<Key> seen;
  hsize_t total_unique = 0;
  constexpr size_t CHUNK_SIZE = 100000;
  std::vector<Record> unique_buffer;

  event_merger_detail::forEachDatasetChunk<Record>(
      input_files, dataset_path, type,
      [&](const std::vector<Record>& chunk, hsize_t count, hsize_t) {
        for (hsize_t i = 0; i < count; ++i) {
          const auto& record = chunk[i];
          if (!seen.insert(key_fn(record)).second) continue;
          unique_buffer.push_back(record);
          ++total_unique;
          on_unique(record, total_unique - 1);
          if (unique_buffer.size() >= CHUNK_SIZE) {
            flushBufferToDataset(unique_buffer, out_ds, type, total_unique);
          }
        }
      });

  if (!unique_buffer.empty()) {
    flushBufferToDataset(unique_buffer, out_ds, type, total_unique);
  }
  return total_unique;
}

template <typename Record, typename Key, typename KeyFn, typename OnUnique>
hsize_t mergeUniqueLookup(H5::H5File& out_file,
                          const std::vector<std::string>& input_files,
                          const std::string& dataset_path,
                          const H5::CompType& type, KeyFn key_fn,
                          OnUnique on_unique) {
  if (input_files.empty()) return 0;

  hsize_t initial_dims[1] = {0};
  hsize_t max_dims[1] = {H5S_UNLIMITED};
  H5::DataSpace out_space(1, initial_dims, max_dims);
  H5::DSetCreatPropList plist;
  hsize_t chunk_dims[1] = {100000};
  plist.setChunk(1, chunk_dims);
  plist.setDeflate(6);
  H5::DataSet out_ds =
      out_file.createDataSet(dataset_path, type, out_space, plist);

  return streamUniqueRecords<Record, Key>(out_ds, type, input_files,
                                          dataset_path, key_fn, on_unique);
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
void readPropertyValuesFromFile(const std::string& f, const std::string& key,
                                const H5::CompType& id_only_type,
                                const std::vector<int32_t>& id_to_merged_idx,
                                std::vector<std::string>& merged_props) {
  const size_t CHUNK_SIZE = 100000;
  try {
    H5::H5File file(f, H5F_ACC_RDONLY);
    if (!H5Lexists(file.getId(), "/lookups/people", H5P_DEFAULT)) return;
    if (!H5Lexists(file.getId(), ("/lookups/people_properties/" + key).c_str(),
                   H5P_DEFAULT))
      return;

    H5::DataSet id_ds = file.openDataSet("/lookups/people");
    H5::DataSpace id_space = id_ds.getSpace();
    hsize_t dims[1];
    id_space.getSimpleExtentDims(dims);
    hsize_t in_count = dims[0];

    H5::DataSet prop_ds = file.openDataSet("/lookups/people_properties/" + key);
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

// Walk `input_files` and return the union of child-group names under
// `parent_path`, preserving insertion order for deterministic output.
// Returns an empty vector if no input file contains the parent group.
std::vector<std::string> discoverChildGroupNames(
    const std::vector<std::string>& input_files,
    const std::string& parent_path) {
  std::vector<std::string> names;
  std::unordered_set<std::string> seen;
  for (const auto& f : input_files) {
    try {
      H5::H5File file(f, H5F_ACC_RDONLY);
      if (!H5Lexists(file.getId(), parent_path.c_str(), H5P_DEFAULT)) continue;
      H5::Group g = file.openGroup(parent_path);
      hsize_t n_obj = g.getNumObjs();
      for (hsize_t i = 0; i < n_obj; ++i) {
        std::string name = g.getObjnameByIdx(i);
        if (seen.insert(name).second) names.push_back(name);
      }
    } catch (...) {
    }
  }
  return names;
}

// Concatenate the dataset at `ds_path` across all `input_files` into a
// new dataset `field` under `out_facet`. No-op if no input file has the
// dataset, or if `out_facet/field` already exists.
void concatenateOneField(const std::vector<std::string>& input_files,
                         const std::string& ds_path, const std::string& field,
                         H5::Group& out_facet,
                         const H5::DataType* forced_dtype = nullptr) {
  H5::DataType dtype = forced_dtype ? *forced_dtype : H5::DataType();
  bool dtype_set = forced_dtype != nullptr;
  for (const auto& f : input_files) {
    try {
      H5::H5File file(f, H5F_ACC_RDONLY);
      if (!H5Lexists(file.getId(), ds_path.c_str(), H5P_DEFAULT)) continue;
      H5::DataSet ds = file.openDataSet(ds_path);
      if (!dtype_set) {
        dtype = ds.getDataType();
        dtype_set = true;
      }
    } catch (...) {
    }
  }
  if (!dtype_set) return;
  if (H5Lexists(out_facet.getId(), field.c_str(), H5P_DEFAULT)) return;

  event_merger_detail::writeMergedDataset<uint8_t>(
      out_facet, field, input_files, ds_path, dtype, dtype.getSize(), true);
}

void mergeOneNetwork(const std::vector<std::string>& input_files,
                     const std::string& net_name, H5::Group& out_nets) {
  H5::Group out_net = openOrCreateGroup(out_nets, net_name);
  for (const char* field : {"person_id", "partner_id"}) {
    const std::string ds_path =
        "/lookups/population_networks/" + net_name + "/" + field;
    concatenateOneField(input_files, ds_path, field, out_net,
                        &H5::PredType::NATIVE_INT32);
  }
  std::cout << "  Merged population_networks for '" << net_name << "'\n";
}

void mergeOneProfileFacet(const std::vector<std::string>& input_files,
                          const std::string& facet, H5::Group& out_assigns) {
  const std::string facet_path = "/lookups/profile_assignments/" + facet;
  std::vector<std::string> field_names =
      discoverChildGroupNames(input_files, facet_path);
  if (field_names.empty()) return;

  H5::Group out_facet = openOrCreateGroup(out_assigns, facet);

  // For every field, concatenate arrays from all rank files. MPI
  // partitions agents across ranks so simple concatenation is correct;
  // no deduplication is applied here.
  for (const auto& field : field_names) {
    const std::string ds_path = facet_path + "/" + field;
    concatenateOneField(input_files, ds_path, field, out_facet);
  }
  std::cout << "  Merged profile_assignments for facet '" << facet << "' ("
            << field_names.size() << " fields)\n";
}

void mergeOnePeopleProperty(const std::string& key,
                            const std::vector<std::string>& input_files,
                            const std::vector<int32_t>& id_to_merged_idx,
                            hsize_t total_unique, H5::Group& prop_group) {
  H5::CompType id_only_type(sizeof(int));
  id_only_type.insertMember("person_id", 0, H5::PredType::NATIVE_INT);

  std::vector<std::string> merged_props(total_unique, kCouldNotResolve);
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

namespace event_merger_detail {

void mergePeopleLookup(H5::H5File& out_file,
                       const std::vector<std::string>& input_files) {
  if (input_files.empty()) return;

  H5::CompType type = event_lookup_schema::person();

  std::vector<int32_t> id_to_merged_idx;
  hsize_t total_unique = mergeUniqueLookup<detail::PersonRecord, int>(
      out_file, input_files, "/lookups/people", type,
      [](const detail::PersonRecord& record) { return record.person_id; },
      [&id_to_merged_idx](const detail::PersonRecord& record,
                          hsize_t merged_index) {
        if (record.person_id >= (int)id_to_merged_idx.size())
          id_to_merged_idx.resize(record.person_id + 1, -1);
        id_to_merged_idx[record.person_id] = (int32_t)merged_index;
      });
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

void mergeVenueLookup(H5::H5File& out_file,
                      const std::vector<std::string>& input_files) {
  if (input_files.empty()) return;

  H5::CompType type = event_lookup_schema::venue();
  hsize_t total_unique = mergeUniqueLookup<detail::VenueRecord, int>(
      out_file, input_files, "/lookups/venues", type,
      [](const detail::VenueRecord& record) { return record.venue_id; },
      [](const detail::VenueRecord&, hsize_t) {});
  std::cout << "  Merged " << total_unique << " unique venues (streaming)"
            << std::endl;
}

void mergePersonActivityLookup(H5::H5File& out_file,
                               const std::vector<std::string>& input_files) {
  H5::CompType type = event_lookup_schema::personActivity();
  mergeDatasetTemplate<detail::PersonActivityRecord>(
      out_file, "/lookups/person_activities", input_files, type);
}

void mergePopulationSummary(H5::H5File& out_file,
                            const std::vector<std::string>& input_files) {
  if (input_files.empty()) return;

  H5::CompType type = event_lookup_schema::populationSummary();
  hsize_t total_unique = mergeUniqueLookup<PopulationSummaryRecord, int>(
      out_file, input_files, "/lookups/population_summary", type,
      [](const PopulationSummaryRecord& record) { return record.person_id; },
      [](const PopulationSummaryRecord&, hsize_t) {});
  std::cout << "  Merged " << total_unique
            << " population summary records (streaming)" << std::endl;
}

void mergeProfileAssignments(H5::H5File& out_file,
                             const std::vector<std::string>& input_files) {
  if (input_files.empty()) return;

  // Discover which facets exist. Take the union across all rank files so
  // we don't miss a facet that happens to be absent from rank 0. Preserve
  // insertion order for deterministic output.
  std::vector<std::string> facet_names =
      discoverChildGroupNames(input_files, "/lookups/profile_assignments");
  if (facet_names.empty()) return;

  H5::Group out_lookups = openOrCreateGroup(out_file, "/lookups");
  H5::Group out_assigns = openOrCreateGroup(out_lookups, "profile_assignments");

  for (const auto& facet : facet_names) {
    mergeOneProfileFacet(input_files, facet, out_assigns);
  }
}

void mergePopulationNetworks(H5::H5File& out_file,
                             const std::vector<std::string>& input_files) {
  if (input_files.empty()) return;

  std::vector<std::string> network_names =
      discoverChildGroupNames(input_files, "/lookups/population_networks");
  if (network_names.empty()) return;

  H5::Group out_lookups = openOrCreateGroup(out_file, "/lookups");
  H5::Group out_nets = openOrCreateGroup(out_lookups, "population_networks");

  for (const auto& net_name : network_names) {
    mergeOneNetwork(input_files, net_name, out_nets);
  }
}

}  // namespace event_merger_detail
}  // namespace june
