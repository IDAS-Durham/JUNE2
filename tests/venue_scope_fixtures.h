#pragma once

#include <cstdint>
#include <vector>

#include "core/types.h"
#include "core/world_state.h"

// A small hand-built world for metrics and triggers counted per venue. It has
// the shapes that matter: venues with a parent, venues without one, a person
// registered to two venues of one activity, and a person registered to the
// parent venue itself.
//
//   venue 0  household (no parent)
//   venue 1  pub, venue 2 pub          (no parent)
//   venue 3  school                    (no parent)
//   venue 4  classroom, venue 5 classroom   (parent: school 3)
//   venue 6  school, venue 7 classroom      (parent of 7: school 6)
namespace june::venue_scope_fixture {

constexpr int16_t kResidence = 0;
constexpr int16_t kPrimary = 1;
constexpr int16_t kLeisure = 2;

constexpr VenueId kHome = 0;
constexpr VenueId kPub = 1;
constexpr VenueId kPubTwo = 2;
constexpr VenueId kSchool = 3;
constexpr VenueId kClassA = 4;
constexpr VenueId kClassB = 5;
constexpr VenueId kSchoolTwo = 6;
constexpr VenueId kClassC = 7;
constexpr int kVenueCount = 8;

constexpr uint8_t kHouseholdType = 0;
constexpr uint8_t kPubType = 1;
constexpr uint8_t kSchoolType = 2;
constexpr uint8_t kClassroomType = 3;

constexpr PersonId kPupilA = 0;       // primary: class A. leisure: both pubs
constexpr PersonId kPupilB = 1;       // primary: class B. leisure: one pub
constexpr PersonId kTwoClasses = 2;   // primary: class A and class B
constexpr PersonId kPubWorker = 3;    // primary: a pub
constexpr PersonId kSchoolStaff = 4;  // primary: the school itself
constexpr PersonId kOtherSchool =
    5;  // primary: class C, under the other school
constexpr int kPeople = 6;

struct Registration {
  PersonId person;
  int16_t activity;
  std::vector<VenueId> venues;
};

inline const std::vector<Registration>& registrations() {
  static const std::vector<Registration> all = {
      {kPupilA, kPrimary, {kClassA}},
      {kPupilA, kLeisure, {kPub, kPubTwo}},
      {kPupilB, kPrimary, {kClassB}},
      {kPupilB, kLeisure, {kPub}},
      {kTwoClasses, kPrimary, {kClassA, kClassB}},
      {kPubWorker, kPrimary, {kPub}},
      {kSchoolStaff, kPrimary, {kSchool}},
      {kOtherSchool, kPrimary, {kClassC}},
  };
  return all;
}

// Every person is registered to the household for residence, then to the
// venues above. `rank` and `ranks` let a parallel test hold only the venues
// that rank owns while every rank keeps every person, which is how a person's
// registered venue ends up on another rank. A venue and its parent always go to
// the same rank, as in the real partition.
inline WorldState makeWorld(int rank = 0, int ranks = 1) {
  WorldState world;
  world.activity_names = {"residence", "primary_activity", "leisure"};
  world.venue_type_names = {"household", "pub", "school", "classroom"};

  const uint8_t types[kVenueCount] = {
      kHouseholdType, kPubType,       kPubType,    kSchoolType,
      kClassroomType, kClassroomType, kSchoolType, kClassroomType};
  const VenueId parents[kVenueCount] = {-1,      -1,      -1, -1,
                                        kSchool, kSchool, -1, kSchoolTwo};
  for (VenueId id = 0; id < kVenueCount; ++id) {
    if ((parents[id] >= 0 ? parents[id] : id) % ranks != rank) continue;
    Venue v{};
    v.id = id;
    v.type_id = types[id];
    v.geo_unit_id = 0;
    v.parent_id = parents[id];
    world.venues.push_back(v);
  }

  for (PersonId id = 0; id < kPeople; ++id) {
    Person& p = world.people.emplace_back();
    p.id = id;
    p.age = 30.0f;
    p.geo_unit_id = -1;
  }

  for (Person& p : world.people) {
    p.activity_meta_start = static_cast<uint32_t>(world.activity_meta.size());
    auto add = [&](int16_t activity, const std::vector<VenueId>& venues) {
      world.activity_meta.push_back(
          {activity, static_cast<uint32_t>(world.activity_venues.size()),
           static_cast<uint16_t>(venues.size())});
      for (VenueId v : venues) world.activity_venues.push_back({v, 0});
      ++p.activity_meta_count;
    };
    add(kResidence, {kHome});
    for (const auto& r : registrations())
      if (r.person == p.id) add(r.activity, r.venues);
  }
  world.buildIndices();
  return world;
}

}  // namespace june::venue_scope_fixture
