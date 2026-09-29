#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "epidemiology/infection_seed.h"

namespace june {

// What makes a seed event the seed it is, independent of where it sits in the
// config: name, date (raw string), type, trajectory key, start symptom,
// infector symptom and transmission mode. An optional field left out is empty,
// whether the YAML key or the bulk CSV column is missing or its cell blank.
// Quantities, filters and target groups are not part of it, so changing a
// seed's count leaves it the same seed.
//
// fields() is the one list of what counts; key, hash and ordering all derive
// from it, so they cannot disagree.
struct SeedIdentity {
  std::string name;
  std::string date_time;
  InfectionSeedType type = InfectionSeedType::UNIFORM;
  std::string trajectory_key;
  std::string start_symptom;
  std::string infector_symptom;
  std::string transmission_mode;

  static SeedIdentity of(const InfectionSeedEvent& seed);
  void writeTo(InfectionSeedEvent& seed) const;

  std::array<std::string, 7> fields() const;

  // Each field length-prefixed, so no value can run into the next.
  std::string key() const;

  // Each field hashed on its own and then mixed in turn, so a value can't
  // slide from one field into the next. Keys the seed's draws.
  uint64_t hash() const;

  bool operator<(const SeedIdentity& other) const;
};

// Two seeds with the same identity are a config error: nothing but position
// would tell them apart. Throws, naming the clashing seed.
void requireUniqueSeedIdentities(const std::vector<InfectionSeedEvent>& seeds);

}  // namespace june
