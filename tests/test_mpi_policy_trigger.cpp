#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#ifdef USE_MPI
#include <mpi.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "core/world_state.h"
#include "epidemiology/policy.h"
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

// The same deaths are split across however many ranks run, each rank counting
// only its share. The trigger must open and close on the same days whatever
// the split, and every rank must agree.
TEST_CASE("Trigger - activation days do not depend on rank count") {
  int rank = 0;
  int size = 1;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  WorldState world;
  const Disease disease =
      makeStageDisease(std::make_shared<ConstantCurve>(1.0));
  PolicyManager pm(world);
  pm.observedMetrics().declare("deaths", {"death", "world", "", ""});
  TemporalPolicy policy;
  policy.name = "close";
  PolicyTrigger trigger;
  trigger.metric = "deaths";
  trigger.cumulative = true;
  trigger.activate_at = 10;
  trigger.calm_days = 4;
  policy.trigger = trigger;
  pm.addTemporalPolicy(policy);
  pm.observedMetrics().resolve(world, disease);
  pm.requireTriggerMetrics();

  // Deaths per day: 2, 3, 0, 5 reach 10 on day 3; four calm days then reopen
  // it on day 7. Deaths are numbered from 0 each day and dealt to ranks by
  // number, so the ranks' local counts differ.
  const int deaths_per_day[] = {2, 3, 0, 5, 0, 0, 0, 0, 0, 0};
  int64_t local_total = 0;
  std::vector<int64_t> in_force_next_day;
  for (int day = 0; day < 10; ++day) {
    for (int id = 0; id < deaths_per_day[day]; ++id) {
      if (id % size == rank) {
        pm.observedMetrics().onDeath();
        ++local_total;
      }
    }
    pm.observedMetrics().reduce();
    pm.advanceTriggers(day);
    in_force_next_day.push_back(
        pm.getTemporalPolicies()[0].window.contains(day + 1.0) ? 1 : 0);
  }

  // Closed from the first slot of day 4 through the last of day 7.
  const std::vector<int64_t> expected = {0, 0, 0, 1, 1, 1, 1, 0, 0, 0};
  CHECK(in_force_next_day == expected);

  // Every rank holds the identical answer, though its own count differs.
  std::vector<int64_t> lo(10), hi(10);
  MPI_Allreduce(in_force_next_day.data(), lo.data(), 10, MPI_INT64_T, MPI_MIN,
                MPI_COMM_WORLD);
  MPI_Allreduce(in_force_next_day.data(), hi.data(), 10, MPI_INT64_T, MPI_MAX,
                MPI_COMM_WORLD);
  CHECK(lo == hi);

  int64_t min_local = 0;
  int64_t max_local = 0;
  MPI_Allreduce(&local_total, &min_local, 1, MPI_INT64_T, MPI_MIN,
                MPI_COMM_WORLD);
  MPI_Allreduce(&local_total, &max_local, 1, MPI_INT64_T, MPI_MAX,
                MPI_COMM_WORLD);
  if (size > 1) CHECK(min_local != max_local);
}

#else
TEST_CASE("MPI policy trigger requires MPI") {}
#endif
