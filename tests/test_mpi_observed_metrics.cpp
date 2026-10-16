#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#ifdef USE_MPI
#include <mpi.h>

#include <cstdint>
#include <memory>

#include "core/world_state.h"
#include "epidemiology/observed_metrics.h"
#include "mpi_test_helpers.h"

using namespace june;

int main(int argc, char** argv) {
  MPI_Init(&argc, &argv);
  doctest::Context context;
  context.applyCommandLine(argc, argv);
  const int result = context.run();
  MPI_Finalize();
  return result;
}

// The same 7 deaths and 10 infected people are split across however many
// ranks run. The reduced totals must not depend on the split.
TEST_CASE("ObservedMetrics - reduced totals do not depend on rank count") {
  int rank = 0;
  int size = 1;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  WorldState world;
  const Disease disease =
      makeStageDisease(std::make_shared<ConstantCurve>(1.0));

  ObservedMetrics m;
  m.declare("deaths", {"death", "world", "", ""});
  m.declare("infected", {"infection", "world", "", ""});
  m.resolve(world, disease);
  m.require("deaths");
  m.require("infected");

  constexpr int kDeaths = 7;
  constexpr int kInfected = 10;
  int64_t local_deaths = 0;
  for (int id = 0; id < kDeaths; ++id) {
    if (id % size == rank) {
      m.onDeath();
      ++local_deaths;
    }
  }
  for (int id = 0; id < kInfected; ++id)
    if (id % size == rank) m.onPresent(1);

  // With more than one rank the local parts differ, so equal totals below
  // come from the reduce and not from every rank seeing the same thing.
  int64_t min_local = 0;
  int64_t max_local = 0;
  MPI_Allreduce(&local_deaths, &min_local, 1, MPI_INT64_T, MPI_MIN,
                MPI_COMM_WORLD);
  MPI_Allreduce(&local_deaths, &max_local, 1, MPI_INT64_T, MPI_MAX,
                MPI_COMM_WORLD);
  if (size > 1) CHECK(min_local != max_local);

  m.reduce();
  CHECK(m.value("deaths") == kDeaths);
  CHECK(m.value("infected") == kInfected);
  CHECK(m.localCount("deaths") == 0);

  // Next day: another death on rank 0, the same infected set re-observed.
  if (rank == 0) m.onDeath();
  for (int id = 0; id < kInfected; ++id)
    if (id % size == rank) m.onPresent(1);
  m.reduce();
  CHECK(m.value("deaths") == kDeaths + 1);  // cumulative adds
  CHECK(m.value("infected") == kInfected);  // current replaces

  // Every rank holds the identical value.
  int64_t d = m.value("deaths");
  int64_t lo = 0;
  int64_t hi = 0;
  MPI_Allreduce(&d, &lo, 1, MPI_INT64_T, MPI_MIN, MPI_COMM_WORLD);
  MPI_Allreduce(&d, &hi, 1, MPI_INT64_T, MPI_MAX, MPI_COMM_WORLD);
  CHECK(lo == hi);
}

#else
TEST_CASE("MPI observed metrics require MPI") {}
#endif
