#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "core/types.h"

namespace june {

class WorldState;

// Looks up the infector's current symptom id for a transmission. A local
// Person's comes from their Infection; a cross-rank visitor's from their
// VisitorInfo. A non-Person infector (negative id) has no symptom, so the
// result is absent (kNoSymptomId).
class InfectorSymptomLookup {
 public:
  explicit InfectorSymptomLookup(const WorldState& world) : world_(world) {}

  uint16_t resolve(
      PersonId infector_id, double current_time,
      const std::unordered_map<PersonId, VisitorInfo>* visitor_data);

  // Lookups on this rank that found no symptom for a Person infector.
  uint64_t gapCount() const { return gap_count_; }

 private:
  const WorldState& world_;
  uint64_t gap_count_ = 0;
};

// One end-of-run warning for the rank-summed gap count, empty when there were
// none. Pure, so rank 0 can emit it after the sum.
std::string formatInfectorLookupGapWarning(uint64_t gap_count);

}  // namespace june
