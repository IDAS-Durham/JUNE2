#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#ifdef USE_MPI
#include <mpi.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/world_state.h"
#include "parallel/domain_manager.h"

using namespace june;

int main(int argc, char** argv) {
  MPI_Init(&argc, &argv);
  doctest::Context context;
  context.applyCommandLine(argc, argv);
  const int result = context.run();
  MPI_Finalize();
  return result;
}

TEST_CASE("DomainManager synchronizes sparse scalar and list registries") {
  int rank = 0;
  int size = 0;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);
  REQUIRE(size == 2);

  WorldState world;
  world.person_property_names = {"scalar_a", "scalar_b", "tags", "maybe_empty"};
  Person& person = world.people.emplace_back();
  person.id = rank;
  person.properties_start = 0;
  person.properties_count = 4;

  if (rank == 0) {
    // Rank 0 inserts these in a different order from rank 1.
    world.person_property_value_registries["scalar_a"] = {"zebra", "alpha"};
    world.person_property_is_list["scalar_a"] = false;
    world.person_properties = {0, -1, 0, -1};

    world.person_property_list_value_registries["tags"] = {{"red"}, {"blue"}};
    world.person_property_is_list["tags"] = true;

    // Keep the empty list pending until another rank identifies the type.
    world.person_property_pending_empty_list_values["maybe_empty"] = {3};
  } else {
    world.person_property_value_registries["scalar_a"] = {"alpha", "zebra"};
    world.person_property_value_registries["scalar_b"] = {"rank-one"};
    world.person_property_is_list["scalar_b"] = false;
    world.person_properties = {1, 0, 0, 0};

    world.person_property_list_value_registries["tags"] = {{"green"}, {"red"}};
    world.person_property_is_list["tags"] = true;
    world.person_property_list_value_registries["maybe_empty"] = {{"value"}};
    world.person_property_is_list["maybe_empty"] = true;
  }
  world.buildIndices();

  Config config;
  DomainManager manager(world, config);
  manager.setMPI(rank, size);
  std::cerr << "registry rank " << rank << " entering synchronization"
            << std::endl;
  manager.synchronizeRegistries();
  std::cerr << "registry rank " << rank << " leaving synchronization"
            << std::endl;

  const std::vector<std::string> expected_scalar_a = {"alpha", "zebra"};
  const std::vector<std::string> expected_scalar_b = {"rank-one"};
  // The list encoding sorts these by item length: red (3), blue (4), green
  // (5).
  const std::vector<std::vector<std::string>> expected_tags = {
      {"red"}, {"blue"}, {"green"}};
  const std::vector<std::vector<std::string>> expected_maybe_empty = {
      {}, {"value"}};
  const bool scalar_keys_match =
      world.person_property_value_registries.size() == 2 &&
      world.person_property_value_registries.count("scalar_a") == 1 &&
      world.person_property_value_registries.count("scalar_b") == 1;
  CHECK(scalar_keys_match);
  CHECK(world.person_property_value_registries.at("scalar_a") ==
        expected_scalar_a);
  CHECK(world.person_property_value_registries.at("scalar_b") ==
        expected_scalar_b);
  CHECK(world.person_property_list_value_registries.at("tags") ==
        expected_tags);
  CHECK(world.person_property_list_value_registries.at("maybe_empty") ==
        expected_maybe_empty);

  CHECK(world.person_property_is_list.at("scalar_a") == false);
  CHECK(world.person_property_is_list.at("scalar_b") == false);
  CHECK(world.person_property_is_list.at("tags") == true);
  CHECK(world.person_property_is_list.at("maybe_empty") == true);

  // Both ranks use the same registry codes: zebra=1, rank-one=0,
  // red=0/green=2, and empty/value=0/1.
  const std::vector<int32_t> expected_codes =
      rank == 0 ? std::vector<int32_t>{1, -1, 0, 0}
                : std::vector<int32_t>{1, 0, 2, 1};
  CHECK(world.person_properties == expected_codes);

  const int local_ok =
      scalar_keys_match &&
      world.person_property_value_registries.at("scalar_a") ==
          expected_scalar_a &&
      world.person_property_list_value_registries.at("tags") == expected_tags &&
      world.person_properties == expected_codes;
  int min_ok = 0;
  int max_ok = 0;
  MPI_Allreduce(&local_ok, &min_ok, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD);
  MPI_Allreduce(&local_ok, &max_ok, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
  CHECK(min_ok == 1);
  CHECK(max_ok == 1);

  if (rank == 0)
    std::cout << "MPI registry invariants: scalar keys/order and list codes "
                 "match on both ranks\n";
}

#else
TEST_CASE("MPI registry synchronization requires MPI") {}
#endif
