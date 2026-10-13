#include <cstddef>
#include <cstring>
#include <new>
#include <vector>

#include "doctest.h"
#include "utils/event_logging/event_types.h"

using namespace june;

namespace {

// Constructs T over memory full of 0xAB and returns the bytes that no named
// data field covers. Every one of them must be zero, or the record leaks
// whatever the allocator left in it when it is written to HDF5.
template <typename T, typename Make>
size_t nonzeroGapBytes(Make make, const std::vector<std::pair<size_t, size_t>>&
                                      data_fields /* offset, size */) {
  alignas(T) unsigned char buffer[sizeof(T)];
  std::memset(buffer, 0xAB, sizeof buffer);
  make(buffer);
  std::vector<bool> is_data(sizeof(T), false);
  for (const auto& [offset, size] : data_fields)
    for (size_t i = 0; i < size; ++i) is_data[offset + i] = true;
  size_t bad = 0;
  for (size_t i = 0; i < sizeof(T); ++i)
    if (!is_data[i] && buffer[i] != 0) ++bad;
  return bad;
}

}  // namespace

TEST_CASE("event records written raw to HDF5 carry zero padding") {
  CHECK(nonzeroGapBytes<InfectionEvent>(
            [](void* p) {
              new (p) InfectionEvent{.person_id = 1,
                                     .infector_id = 2,
                                     .venue_id = 3,
                                     .time = 4.0,
                                     .encounter_type_id = 5,
                                     .transmission_mode_index = 6,
                                     .infector_symptom_id = 7,
                                     .source = InfectionSource::Person};
            },
            {{offsetof(InfectionEvent, person_id), 4},
             {offsetof(InfectionEvent, infector_id), 4},
             {offsetof(InfectionEvent, venue_id), 4},
             {offsetof(InfectionEvent, time), 8},
             {offsetof(InfectionEvent, encounter_type_id), 1},
             {offsetof(InfectionEvent, transmission_mode_index), 1},
             {offsetof(InfectionEvent, infector_symptom_id), 1},
             {offsetof(InfectionEvent, source), 1},
             {offsetof(InfectionEvent, reserved), 1}}) == 0);

  CHECK(nonzeroGapBytes<SymptomChangeEvent>(
            [](void* p) { new (p) SymptomChangeEvent{1, 2, 3.0, 4, 5}; },
            {{offsetof(SymptomChangeEvent, person_id), 4},
             {offsetof(SymptomChangeEvent, venue_id), 4},
             {offsetof(SymptomChangeEvent, time), 8},
             {offsetof(SymptomChangeEvent, old_symptom_id), 1},
             {offsetof(SymptomChangeEvent, new_symptom_id), 1}}) == 0);

  CHECK(nonzeroGapBytes<CoordinatedEncounterEvent>(
            [](void* p) {
              new (p) CoordinatedEncounterEvent{.person_a = 1,
                                                .person_b = 2,
                                                .time = 3.0,
                                                .encounter_type_id = 4,
                                                .slot = 5,
                                                .group_id = 6};
            },
            {{offsetof(CoordinatedEncounterEvent, person_a), 4},
             {offsetof(CoordinatedEncounterEvent, person_b), 4},
             {offsetof(CoordinatedEncounterEvent, time), 8},
             {offsetof(CoordinatedEncounterEvent, encounter_type_id), 1},
             {offsetof(CoordinatedEncounterEvent, slot), 4},
             {offsetof(CoordinatedEncounterEvent, group_id), 8}}) == 0);

  CHECK(
      nonzeroGapBytes<FollowEvent>(
          [](void* p) {
            new (p) FollowEvent{
                .host = 1, .follower = 2, .time = 3.0, .rule_id = 4, .slot = 5};
          },
          {{offsetof(FollowEvent, host), 4},
           {offsetof(FollowEvent, follower), 4},
           {offsetof(FollowEvent, time), 8},
           {offsetof(FollowEvent, rule_id), 1},
           {offsetof(FollowEvent, slot), 4}}) == 0);
}
