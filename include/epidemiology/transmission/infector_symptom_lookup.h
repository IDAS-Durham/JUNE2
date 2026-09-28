#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "core/types.h"
#include "epidemiology/transmission/transmission_record.h"

namespace june {

class WorldState;

// Looks up the infector's current symptom id for a transmission. A local
// Person's comes from their Infection; a cross-rank visitor's from their
// VisitorInfo. A non-Person source has no infector, so the result is absent
// (kNoSymptomId). A Person source whose symptom can't be found, including one
// with no infector sampled (negative id), is absent too. Looking up counts
// nothing: a lookup can belong to an attempt that is later discarded, so each
// site counts a gap only once its infection is applied (countIfGap).
class InfectorSymptomLookup {
 public:
  explicit InfectorSymptomLookup(const WorldState& world) : world_(world) {}

  uint16_t resolve(
      InfectionSource source, PersonId infector_id, double current_time,
      const std::unordered_map<PersonId, VisitorInfo>* visitor_data) const;

  // Counts one gap if an applied infection had a Person source but no symptom.
  void countIfGap(const TransmissionRecord& transmission);

  // Applied infections on this rank whose Person infector had no symptom.
  uint64_t gapCount() const { return gap_count_; }

 private:
  const WorldState& world_;
  uint64_t gap_count_ = 0;
};

// One end-of-run warning for the rank-summed gap count, empty when there were
// none. Pure, so rank 0 can emit it after the sum.
std::string formatInfectorLookupGapWarning(uint64_t gap_count);

}  // namespace june
