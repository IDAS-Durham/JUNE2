#include "epidemiology/seeding/seed_identity.h"

#include <stdexcept>
#include <unordered_set>

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

void appendField(std::string& identity, const std::string& field) {
  identity += std::to_string(field.size());
  identity += ':';
  identity += field;
}

// The optional fields a clash message names, those that are set.
std::string describeOptionalFields(const InfectionSeedEvent& seed) {
  std::string description;
  auto describe = [&](const char* label, const std::string& value) {
    if (!value.empty()) description += std::string(", ") + label + " '" + value + "'";
  };
  describe("trajectory_key", seed.trajectory_key);
  describe("start_symptom", seed.start_symptom);
  describe("infector_symptom", seed.infector_symptom);
  describe("transmission_mode", seed.transmission_mode);
  return description;
}
}  // namespace

std::string seedIdentity(const InfectionSeedEvent& seed) {
  std::string identity;
  appendField(identity, seed.name);
  appendField(identity, seed.date_time);
  appendField(identity, seedTypeName(seed.type));
  appendField(identity, seed.trajectory_key);
  appendField(identity, seed.start_symptom);
  appendField(identity, seed.infector_symptom);
  appendField(identity, seed.transmission_mode);
  return identity;
}

void requireUniqueSeedIdentities(const std::vector<InfectionSeedEvent>& seeds) {
  std::unordered_set<std::string> seen;
  for (const auto& seed : seeds) {
    if (!seen.insert(seedIdentity(seed)).second) {
      throw std::runtime_error(
          "Infection seeds share name '" + seed.name + "', date '" +
          seed.date_time + "', type '" + seedTypeName(seed.type) + "'" +
          describeOptionalFields(seed) +
          ": give each seed event its own name");
    }
  }
}

}  // namespace june
