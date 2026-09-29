#include "epidemiology/seeding/seed_identity.h"

#include <stdexcept>
#include <unordered_set>

#include "utils/deterministic_rng.h"

namespace june {

namespace {
std::string seedTypeName(InfectionSeedType type) {
  switch (type) {
    case InfectionSeedType::UNIFORM:
      return "uniform";
    case InfectionSeedType::EXACT:
      return "exact";
    case InfectionSeedType::CLUSTERED:
      return "clustered";
  }
  throw std::runtime_error("Unknown seed type");
}

// The optional fields a clash message names, those that are set.
std::string describeOptionalFields(const SeedIdentity& identity) {
  std::string description;
  auto describe = [&](const char* label, const std::string& value) {
    if (!value.empty()) {
      description += std::string(", ") + label + " '" + value + "'";
    }
  };
  describe("trajectory_key", identity.trajectory_key);
  describe("start_symptom", identity.start_symptom);
  describe("infector_symptom", identity.infector_symptom);
  describe("transmission_mode", identity.transmission_mode);
  return description;
}
}  // namespace

SeedIdentity SeedIdentity::of(const InfectionSeedEvent& seed) {
  return {seed.name,          seed.date_time,        seed.type,
          seed.trajectory_key, seed.start_symptom,   seed.infector_symptom,
          seed.transmission_mode};
}

void SeedIdentity::writeTo(InfectionSeedEvent& seed) const {
  seed.name = name;
  seed.date_time = date_time;
  seed.type = type;
  seed.trajectory_key = trajectory_key;
  seed.start_symptom = start_symptom;
  seed.infector_symptom = infector_symptom;
  seed.transmission_mode = transmission_mode;
}

std::array<std::string, 7> SeedIdentity::fields() const {
  return {name,           date_time,     seedTypeName(type), trajectory_key,
          start_symptom, infector_symptom, transmission_mode};
}

std::string SeedIdentity::key() const {
  std::string identity_key;
  for (const std::string& field : fields()) {
    identity_key += std::to_string(field.size());
    identity_key += ':';
    identity_key += field;
  }
  return identity_key;
}

uint64_t SeedIdentity::hash() const {
  uint64_t identity_hash = 0;
  for (const std::string& field : fields()) {
    identity_hash = mix_seed(identity_hash, hash_name(field));
  }
  return identity_hash;
}

bool SeedIdentity::operator<(const SeedIdentity& other) const {
  return fields() < other.fields();
}

void requireUniqueSeedIdentities(const std::vector<InfectionSeedEvent>& seeds) {
  std::unordered_set<std::string> seen;
  for (const auto& seed : seeds) {
    const SeedIdentity identity = SeedIdentity::of(seed);
    if (!seen.insert(identity.key()).second) {
      throw std::runtime_error(
          "Infection seeds share name '" + identity.name + "', date '" +
          identity.date_time + "', type '" + seedTypeName(identity.type) +
          "'" + describeOptionalFields(identity) +
          ": give each seed event its own name");
    }
  }
}

}  // namespace june
