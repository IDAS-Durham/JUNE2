#include <H5Cpp.h>

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/world_state.h"
#include "doctest.h"
#include "test_utils.h"
#include "utils/event_logging/event_merger.h"
#include "utils/event_logging/event_types.h"
#include "utils/event_logging/event_writer_detail.h"

using namespace june;

namespace {

struct Summary {
  std::vector<int> person_ids;
  std::vector<std::array<uint16_t, 4>> codes;
  std::map<std::string, std::string> attrs;
  uint16_t missing_code = 0;
  std::map<std::string, std::vector<std::string>> tables;
};

Config configFor(std::vector<std::string> properties) {
  Config config;
  config.simulation.summary_properties = std::move(properties);
  config.simulation.summary_properties_explicit = true;
  return config;
}

// Three properties: a scalar with names, a list with names, a plain integer.
// `colour_registry` is given in the caller's order so tests can shuffle it.
WorldState makeWorld(const std::vector<std::string>& colour_registry,
                     const std::vector<int>& person_ids) {
  WorldState world;
  world.person_property_names = {"colour", "tags", "rank"};
  world.person_property_value_registries["colour"] = colour_registry;
  world.person_property_list_value_registries["tags"] = {{"b", "a"}, {"a"}, {}};
  auto colour_index = [&](const std::string& name) {
    for (size_t i = 0; i < colour_registry.size(); ++i)
      if (colour_registry[i] == name) return static_cast<int32_t>(i);
    return int32_t(-1);
  };
  const std::vector<std::string> colour_of = {"red", "green", "blue", "red"};
  for (size_t i = 0; i < person_ids.size(); ++i) {
    Person& person = world.people.emplace_back();
    person.id = person_ids[i];
    person.properties_start =
        static_cast<uint32_t>(world.person_properties.size());
    person.properties_count = 3;
    world.person_properties.push_back(
        colour_index(colour_of[person_ids[i] % 4]));
    world.person_properties.push_back(person_ids[i] % 3);
    // Person 3 has no rank.
    world.person_properties.push_back(person_ids[i] == 3 ? -1
                                                         : person_ids[i] * 10);
  }
  world.buildIndices();
  return world;
}

Summary readSummary(const std::string& path) {
  Summary out;
  H5::H5File file(path, H5F_ACC_RDONLY);
  H5::DataSet ds = file.openDataSet("/lookups/population_summary");
  hsize_t n = 0;
  ds.getSpace().getSimpleExtentDims(&n);
  std::vector<PopulationSummaryRecord> rows(n);
  ds.read(rows.data(), ds.getCompType());
  for (const auto& r : rows) {
    out.person_ids.push_back(r.person_id);
    out.codes.push_back({r.extra_codes[0], r.extra_codes[1], r.extra_codes[2],
                         r.extra_codes[3]});
  }
  H5::StrType stype(H5::PredType::C_S1, H5T_VARIABLE);
  for (int k = 0; k < 4; ++k) {
    const std::string name = "extra_prop_" + std::to_string(k);
    if (!ds.attrExists(name)) continue;
    std::string value;
    ds.openAttribute(name).read(stype, value);
    out.attrs[name] = value;
  }
  ds.openAttribute("missing_code")
      .read(H5::PredType::NATIVE_UINT16, &out.missing_code);
  if (H5Lexists(file.getId(), "/lookups/population_summary_values",
                H5P_DEFAULT)) {
    H5::Group group = file.openGroup("/lookups/population_summary_values");
    for (hsize_t i = 0; i < group.getNumObjs(); ++i) {
      const std::string name = group.getObjnameByIdx(i);
      H5::DataSet table = group.openDataSet(name);
      hsize_t count = 0;
      table.getSpace().getSimpleExtentDims(&count);
      std::vector<char*> raw(count);
      H5::StrType t = table.getStrType();
      table.read(raw.data(), t);
      for (char* s : raw) out.tables[name].push_back(s);
      H5::DataSpace mem(1, &count);
      H5::DataSet::vlenReclaim(raw.data(), t, mem);
    }
  }
  return out;
}

void writeSummary(const std::string& path, const WorldState& world,
                  const Config& config) {
  H5::H5File file(path, H5F_ACC_TRUNC);
  file.createGroup("/lookups");  // the real writer creates it first
  event_writer::writePopulationSummary(file, world, config);
}

// The value a stored code stands for, or "<missing>".
std::string decode(const Summary& s, size_t row, size_t column,
                   const std::string& property) {
  const uint16_t code = s.codes[row][column];
  if (code == s.missing_code) return "<missing>";
  auto it = s.tables.find(property);
  if (it == s.tables.end()) return std::to_string(code);
  return it->second.at(code);
}

}  // namespace

TEST_CASE("summary codes decode back to each person's property value") {
  ScopedTestFiles dir("june_summary_codes");
  const std::string path = dir.path("events.h5").string();
  const WorldState world = makeWorld({"red", "green", "blue"}, {0, 1, 2, 3, 4});
  writeSummary(path, world, configFor({"colour", "tags", "rank"}));
  const Summary s = readSummary(path);

  REQUIRE(s.person_ids.size() == 5);
  CHECK(s.attrs.at("extra_prop_0") == "colour");
  CHECK(s.attrs.at("extra_prop_1") == "tags");
  CHECK(s.attrs.at("extra_prop_2") == "rank");
  CHECK(s.attrs.count("extra_prop_3") == 0);
  const std::vector<std::string> colour_of = {"red", "green", "blue", "red"};
  const std::vector<std::string> tags_of = {R"(["b","a"])", R"(["a"])", "[]"};
  for (size_t row = 0; row < 5; ++row) {
    const int id = s.person_ids[row];
    CHECK(decode(s, row, 0, "colour") == colour_of[id % 4]);
    CHECK(decode(s, row, 1, "tags") == tags_of[id % 3]);
    CHECK(decode(s, row, 2, "rank") ==
          (id == 3 ? std::string("<missing>") : std::to_string(id * 10)));
  }
  // Distinct values must give distinct codes, or the column carries nothing.
  CHECK(s.codes[0][0] != s.codes[1][0]);
  CHECK(s.codes[1][0] != s.codes[2][0]);
  CHECK(s.codes[0][0] == s.codes[3][0]);
  // A plain integer property has no value table.
  CHECK(s.tables.count("rank") == 0);
  CHECK(s.tables.at("colour") ==
        std::vector<std::string>{"blue", "green", "red"});
}

TEST_CASE("summary codes do not depend on the order of the value registry") {
  ScopedTestFiles dir("june_summary_order");
  const std::string a = dir.path("a.h5").string();
  const std::string b = dir.path("b.h5").string();
  const Config config = configFor({"colour"});
  writeSummary(a, makeWorld({"red", "green", "blue"}, {0, 1, 2, 3}), config);
  writeSummary(b, makeWorld({"blue", "red", "green"}, {0, 1, 2, 3}), config);
  const Summary sa = readSummary(a);
  const Summary sb = readSummary(b);
  CHECK(sa.codes == sb.codes);
  CHECK(sa.tables == sb.tables);
}

TEST_CASE("merged summary keeps column names, value tables and every row") {
  ScopedTestFiles dir("june_summary_merge");
  const std::string a = dir.path("rank0.h5").string();
  const std::string b = dir.path("rank1.h5").string();
  const std::string merged = dir.path("merged.h5").string();
  const Config config = configFor({"colour", "tags"});
  // Both ranks carry the same global registry, each holds its own people.
  writeSummary(a, makeWorld({"red", "green", "blue"}, {0, 1}), config);
  writeSummary(b, makeWorld({"red", "green", "blue"}, {2, 3}), config);
  mergeEventFiles({a, b}, merged);

  const Summary s = readSummary(merged);
  REQUIRE(s.person_ids.size() == 4);
  CHECK(s.attrs.at("extra_prop_0") == "colour");
  CHECK(s.attrs.at("extra_prop_1") == "tags");
  CHECK(s.attrs.count("extra_prop_2") == 0);
  const std::vector<std::string> colour_of = {"red", "green", "blue", "red"};
  for (size_t row = 0; row < 4; ++row)
    CHECK(decode(s, row, 0, "colour") == colour_of[s.person_ids[row]]);
  CHECK(s.tables.at("colour") ==
        std::vector<std::string>{"blue", "green", "red"});
}

TEST_CASE("merging summaries with different value tables is an error") {
  ScopedTestFiles dir("june_summary_merge_mismatch");
  const std::string a = dir.path("rank0.h5").string();
  const std::string b = dir.path("rank1.h5").string();
  const Config config = configFor({"colour"});
  writeSummary(a, makeWorld({"red", "green", "blue"}, {0, 1}), config);
  writeSummary(b, makeWorld({"red", "green"}, {3}), config);
  CHECK_THROWS_WITH_AS(mergeEventFiles({a, b}, dir.path("m.h5").string()),
                       doctest::Contains("different property names or value "
                                         "tables"),
                       std::runtime_error);
}

TEST_CASE("a property with too many values for a summary code is an error") {
  ScopedTestFiles dir("june_summary_too_many");
  WorldState world = makeWorld({"red"}, {0});
  auto& registry = world.person_property_value_registries["colour"];
  registry.clear();
  for (int i = 0; i < 65535; ++i) registry.push_back("v" + std::to_string(i));
  CHECK_THROWS_WITH_AS(
      writeSummary(dir.path("e.h5").string(), world, configFor({"colour"})),
      doctest::Contains("'colour' has 65535 distinct values"),
      std::runtime_error);
}

TEST_CASE("an integer property that does not fit a summary code is an error") {
  ScopedTestFiles dir("june_summary_int_range");
  WorldState world = makeWorld({"red", "green", "blue"}, {0, 1});
  world.person_properties[2] = 70000;  // person 0, "rank"
  CHECK_THROWS_WITH_AS(
      writeSummary(dir.path("e.h5").string(), world, configFor({"rank"})),
      doctest::Contains("'rank' value 70000 of person 0"), std::runtime_error);
}
