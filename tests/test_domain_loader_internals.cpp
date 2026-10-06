#include <H5Cpp.h>

#include <iostream>
#include <sstream>
#include <string>

#include "core/world_state.h"
#include "doctest.h"
#include "loaders/domain_loader_internals.h"
#include "loaders/hdf5_loader.h"
#include "test_utils.h"

using namespace june;

// The seam behind buildGlobalVenueMaps(). A rank must be able to name the type
// of every Venue in the world, not just the ones decomposed onto it — otherwise
// kUnknownVenueTypeId means both "no such Venue" and "not mine", and
// venue-gated policy becomes rank-dependent.
TEST_CASE("fillGlobalVenueMaps types venues this rank does not own") {
  WorldState world;
  world.venue_type_names = {"household", "school"};

  GeographicalUnit geo_unit;
  geo_unit.id = 10;
  geo_unit.parent_id = -1;
  geo_unit.level_id = 0;
  world.geo_units.push_back(geo_unit);

  // One local venue; venue 200 belongs to another rank. No activity_venues, so
  // no local person can reach 200 — it is outside any halo.
  Venue local_venue;
  local_venue.id = 100;
  local_venue.type_id = 0;
  local_venue.geo_unit_id = 10;
  world.venues.push_back(local_venue);
  world.buildIndices();

  detail::fillGlobalVenueMaps(world, {100, 200}, {0, 1}, {10, 10});

  SUBCASE("both venues land in the type index") {
    REQUIRE(world.venue_type_by_id.size() == 201);
    CHECK(world.venue_type_by_id[100] == 0);
    CHECK(world.venue_type_by_id[200] == 1);
  }

  SUBCASE("a foreign venue types via getVenueTypeId") {
    CHECK(world.getVenueTypeId(200) == 1);
  }

  SUBCASE("a hole naming no Venue is unresolvable") {
    CHECK(world.getVenueTypeId(150) == kUnknownVenueTypeId);
  }

  SUBCASE("an id past the end is unresolvable") {
    CHECK(world.getVenueTypeId(300) == kUnknownVenueTypeId);
  }

  SUBCASE("a negative id is unresolvable") {
    CHECK(world.getVenueTypeId(makeVirtualVenueId(100)) == kUnknownVenueTypeId);
  }
}

// fillGlobalVenueMaps is public API, callable without an HDF5Loader, and reads
// the type/geo arrays off venue_ids.size(). A short array would read out of
// bounds, so the length agreement is checked, not assumed.
TEST_CASE("fillGlobalVenueMaps rejects mismatched input lengths") {
  WorldState world;
  world.venue_type_names = {"household", "school"};

  CHECK_THROWS(detail::fillGlobalVenueMaps(world, {100, 200}, {0}, {10, 10}));
  CHECK_THROWS(detail::fillGlobalVenueMaps(world, {100, 200}, {0, 1}, {10}));
}

// Sparse /venues/ids costs one byte per hole. That is a warning, never a
// throw: the load must still succeed and every id must still type correctly.
TEST_CASE("fillGlobalVenueMaps warns on sparse ids but still loads") {
  WorldState world;
  world.venue_type_names = {"household", "school"};

  std::ostringstream captured;
  std::streambuf* previous = std::cerr.rdbuf(captured.rdbuf());
  detail::fillGlobalVenueMaps(world, {100, 200}, {0, 1}, {10, 10});
  std::cerr.rdbuf(previous);

  CHECK(captured.str().find("sparse") != std::string::npos);
  CHECK(world.getVenueTypeId(100) == 0);
  CHECK(world.getVenueTypeId(200) == 1);
}

TEST_CASE(
    "HDF5 person properties parse JSON string lists but preserve networks") {
  ScopedTestFiles files{"june_list_property"};
  const auto path = files.write("properties.h5", "");
  {
    H5::H5File file(path.string(), H5F_ACC_TRUNC);
    H5::Group population = file.createGroup("population");
    H5::Group properties = population.createGroup("properties");
    hsize_t dims[1] = {5};
    H5::DataSpace space(1, dims);
    H5::StrType type(H5::PredType::C_S1, H5T_VARIABLE);
    H5::DataSet dataset =
        properties.createDataSet("comorbidities", type, space);
    const char* values[] = {"[\"cancer\", \"crd\"]", "[]", "[1,2,3]", "plain",
                            "[\"quoted\"]"};
    dataset.write(values, type);

    hsize_t mixed_dims[1] = {2};
    H5::DataSpace mixed_space(1, mixed_dims);
    H5::DataSet mixed = properties.createDataSet("mixed", type, mixed_space);
    const char* mixed_values[] = {"[\"list\"]", "scalar"};
    mixed.write(mixed_values, type);
  }

  HDF5Loader loader(path.string());
  auto values = loader.readPropertyDatasetRange(
      "/population/properties/comorbidities", 0, 5, "comorbidities");
  REQUIRE(values.size() == 5);
  REQUIRE(std::holds_alternative<std::vector<std::string>>(values[0]));
  CHECK(std::get<std::vector<std::string>>(values[0]) ==
        std::vector<std::string>{"cancer", "crd"});
  REQUIRE(std::holds_alternative<std::vector<std::string>>(values[1]));
  CHECK(std::get<std::vector<std::string>>(values[1]).empty());
  CHECK(std::holds_alternative<std::string>(values[2]));
  CHECK(std::get<std::string>(values[2]) == "[1,2,3]");
  CHECK(std::holds_alternative<std::string>(values[3]));
  CHECK(std::get<std::string>(values[3]) == "plain");
  REQUIRE(std::holds_alternative<std::vector<std::string>>(values[4]));
  CHECK(std::get<std::vector<std::string>>(values[4]) ==
        std::vector<std::string>{"quoted"});

  const auto mixed = loader.readPropertyDatasetRange(
      "/population/properties/mixed", 0, 2, "mixed");
  REQUIRE(mixed.size() == 2);
  CHECK(std::holds_alternative<std::vector<std::string>>(mixed[0]));
  CHECK(std::holds_alternative<std::string>(mixed[1]));
}

TEST_CASE("domain loader rejects mixed scalar and string-list properties") {
  ScopedTestFiles files{"june_mixed_property"};
  const auto path = files.write("properties.h5", "");
  {
    H5::H5File file(path.string(), H5F_ACC_TRUNC);
    H5::Group population = file.createGroup("population");
    H5::Group properties = population.createGroup("properties");
    hsize_t dims[1] = {2};
    H5::DataSpace space(1, dims);

    H5::DataSet ids =
        population.createDataSet("ids", H5::PredType::NATIVE_INT32, space);
    int32_t id_values[] = {1, 2};
    ids.write(id_values, H5::PredType::NATIVE_INT32);
    H5::DataSet ages =
        population.createDataSet("ages", H5::PredType::NATIVE_FLOAT, space);
    float age_values[] = {20.0F, 21.0F};
    ages.write(age_values, H5::PredType::NATIVE_FLOAT);
    H5::DataSet sexes =
        population.createDataSet("sexes", H5::PredType::NATIVE_UINT8, space);
    uint8_t sex_values[] = {0, 0};
    sexes.write(sex_values, H5::PredType::NATIVE_UINT8);

    H5::StrType type(H5::PredType::C_S1, H5T_VARIABLE);
    H5::DataSet mixed = properties.createDataSet("mixed", type, space);
    const char* mixed_values[] = {"[\"list\"]", "scalar"};
    mixed.write(mixed_values, type);
  }

  HDF5Loader loader(path.string());
  detail::ChunkSpan span{0, 2, {0}};
  detail::GeoPartitionMap partition{{10, {0, 2}}};
  std::unordered_map<std::string, std::unordered_map<std::string, int32_t>>
      scalar_cache;
  std::unordered_map<std::string, std::map<std::vector<std::string>, int32_t>>
      list_cache;

  CHECK_THROWS_WITH(
      detail::loadPersonsInSpan(loader, span, partition, {10}, {"mixed"},
                                scalar_cache, list_cache),
      doctest::Contains(
          "Person property 'mixed' mixes scalar and string-list"));
}
