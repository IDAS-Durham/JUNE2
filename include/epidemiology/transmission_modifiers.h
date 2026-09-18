#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/config.h"

namespace june {

enum class TransmissionEffectChannel : uint8_t {
  SourceInfectiousness,
  TargetSusceptibility,
  ContactIntensity,
  EnvironmentalRisk,
};

enum class TransmissionEffectScope : uint8_t { Person, Venue };
enum class TransmissionPolicyKind : uint8_t { Symptom, Temporal };

struct PolicyTransmissionEffect {
  TransmissionPolicyKind policy_kind = TransmissionPolicyKind::Temporal;
  uint16_t policy_index = 0;
  TransmissionEffectScope scope = TransmissionEffectScope::Person;
  std::vector<SelectionCriterion> criteria;
  std::string mode_name;
  uint8_t mode_index = 0;  // resolved against Disease::modes before use
  TransmissionEffectChannel channel =
      TransmissionEffectChannel::SourceInfectiousness;
  double multiplier = 1.0;
};

// One shared table stores full mode/channel values; each person or venue keeps
// only a compact ID. Entry zero is always the identity set.
class TransmissionModifierTable {
 public:
  using ModeValues = std::array<double, 4>;
  using Set = std::array<ModeValues, VisitorInfo::MAX_MODES>;

  void reset(size_t num_modes) {
    if (num_modes > VisitorInfo::MAX_MODES)
      throw std::runtime_error(
          "too many transmission modes for modifier table");
    num_modes_ = num_modes;
    sets_.clear();
    ids_.clear();
    Set identity;
    identity.fill(ModeValues{1.0, 1.0, 1.0, 1.0});
    sets_.push_back(identity);
    ids_.emplace(identity, 0);
  }

  uint32_t intern(const Set& values) {
    auto it = ids_.find(values);
    if (it != ids_.end()) return it->second;
    const uint32_t id = static_cast<uint32_t>(sets_.size());
    ids_.emplace(values, id);
    sets_.push_back(values);
    return id;
  }

  const Set& get(uint32_t id) const { return sets_.at(id); }
  double get(uint32_t id, size_t mode,
             TransmissionEffectChannel channel) const {
    return sets_[id][mode][static_cast<size_t>(channel)];
  }
  size_t size() const { return sets_.size(); }
  size_t numModes() const { return num_modes_; }

 private:
  size_t num_modes_ = 0;
  std::vector<Set> sets_;
  std::map<Set, uint32_t> ids_;
};

}  // namespace june
