#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#ifdef USE_MPI
#include <mpi.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/world_state.h"
#include "epidemiology/observed_metrics.h"
#include "mpi_test_helpers.h"
#include "venue_scope_fixtures.h"

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

  const Person nobody{};
  constexpr int kDeaths = 7;
  constexpr int kInfected = 10;
  int64_t local_deaths = 0;
  for (int id = 0; id < kDeaths; ++id) {
    if (id % size == rank) {
      m.onDeath(nobody);
      ++local_deaths;
    }
  }
  for (int id = 0; id < kInfected; ++id)
    if (id % size == rank) m.onPresent(nobody, 1);

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
  if (rank == 0) m.onDeath(nobody);
  for (int id = 0; id < kInfected; ++id)
    if (id % size == rank) m.onPresent(nobody, 1);
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

// The fixture deals a venue and its parent to one rank and the people out by
// id, so most people are registered to venues another rank holds. Counted on
// each person's own rank, the per unit totals must still equal what one process
// would count.
TEST_CASE("ObservedMetrics - per unit totals do not depend on rank count") {
  using namespace venue_scope_fixture;
  int rank = 0;
  int size = 1;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  WorldState world = makeWorld(rank, size);
  const Disease disease =
      makeStageDisease(std::make_shared<ConstantCurve>(1.0));

  ObservedMetrics m;
  m.declare("school_deaths", {"death", "parent_venue", "primary_activity", ""});
  m.declare("class_deaths", {"death", "venue", "primary_activity", ""});
  m.declare("pub_now", {"infection", "venue", "leisure", ""});
  m.resolve(world, disease);
  for (const char* n : {"school_deaths", "class_deaths", "pub_now"})
    m.require(n);
  m.buildVenueParents(world);

  // A venue and its parent are held by the same rank, but a rank holds only
  // some venues. The parent index is still whole on every rank.
  for (const Venue& venue : world.venues)
    if (venue.parent_id >= 0) CHECK(world.getVenue(venue.parent_id) != nullptr);
  if (size > 1) CHECK(world.venues.size() < kVenueCount);
  REQUIRE(world.venue_parent_by_id.size() == kVenueCount);
  CHECK(world.parentUnitOf(kClassA) == kSchool);
  CHECK(world.parentUnitOf(kClassC) == kSchoolTwo);
  CHECK(world.parentUnitOf(kPubTwo) == kPubTwo);

  // Every person dies and is seen infected, counted by the rank that owns them.
  int64_t my_registrations = 0;
  for (PersonId id = 0; id < kPeople; ++id) {
    if (id % size != rank) continue;
    for (const auto& r : registrations())
      if (r.person == id)
        my_registrations += static_cast<int64_t>(r.venues.size());
    m.onDeath(world.people[id]);
    m.onPresent(world.people[id], 1);
  }
  int64_t lo = 0;
  int64_t hi = 0;
  MPI_Allreduce(&my_registrations, &lo, 1, MPI_INT64_T, MPI_MIN,
                MPI_COMM_WORLD);
  MPI_Allreduce(&my_registrations, &hi, 1, MPI_INT64_T, MPI_MAX,
                MPI_COMM_WORLD);
  if (size > 1) CHECK(lo != hi);  // the ranks' own parts differ

  m.reduce();

  // What one process would count, worked out from the registrations directly.
  auto unit_of = [](VenueId v) {
    if (v == kClassA || v == kClassB) return kSchool;
    if (v == kClassC) return kSchoolTwo;
    return v;
  };
  std::vector<int64_t> want_class(kVenueCount, 0);
  std::vector<int64_t> want_school(kVenueCount, 0);
  std::vector<int64_t> want_pub(kVenueCount, 0);
  for (PersonId id = 0; id < kPeople; ++id) {
    std::vector<VenueId> units;
    for (const auto& r : registrations()) {
      if (r.person != id) continue;
      for (VenueId v : r.venues) {
        if (r.activity == kPrimary) {
          ++want_class[v];
          units.push_back(unit_of(v));
        } else {
          ++want_pub[v];
        }
      }
    }
    std::sort(units.begin(), units.end());
    units.erase(std::unique(units.begin(), units.end()), units.end());
    for (VenueId u : units) ++want_school[u];
  }

  std::vector<int64_t> got;
  for (VenueId v = 0; v < kVenueCount; ++v) {
    CHECK(m.unitValue("class_deaths", v) == want_class[v]);
    CHECK(m.unitValue("school_deaths", v) == want_school[v]);
    CHECK(m.unitValue("pub_now", v) == want_pub[v]);
    got.push_back(m.unitValue("class_deaths", v));
    got.push_back(m.unitValue("school_deaths", v));
    got.push_back(m.unitValue("pub_now", v));
  }
  // Every rank holds the identical numbers.
  std::vector<int64_t> min_got(got.size());
  std::vector<int64_t> max_got(got.size());
  MPI_Allreduce(got.data(), min_got.data(), static_cast<int>(got.size()),
                MPI_INT64_T, MPI_MIN, MPI_COMM_WORLD);
  MPI_Allreduce(got.data(), max_got.data(), static_cast<int>(got.size()),
                MPI_INT64_T, MPI_MAX, MPI_COMM_WORLD);
  CHECK(min_got == max_got);
}

#else
TEST_CASE("MPI observed metrics require MPI") {}
#endif
