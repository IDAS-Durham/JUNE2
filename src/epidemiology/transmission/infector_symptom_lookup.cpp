#include "epidemiology/transmission/infector_symptom_lookup.h"

#include "core/world_state.h"
#include "epidemiology/disease.h"

namespace june {

uint8_t InfectorSymptomLookup::resolve(
    InfectionSource source, PersonId infector_id, double current_time,
    const std::unordered_map<PersonId, VisitorInfo>* visitor_data) const {
  if (source != InfectionSource::Person) return kNoSymptomId;
  const Person* infector =
      infector_id >= 0 ? world_.getPerson(infector_id) : nullptr;
  // Symptom ids are stored wide but registries stay under kNoSymptomId, so
  // narrowing here, once, is lossless.
  if (infector && infector->infection) {
    return static_cast<uint8_t>(
        infector->infection->getTrajectory().getCurrentSymptomId(
            current_time));
  }
  if (!infector && visitor_data) {
    auto visitor = visitor_data->find(infector_id);
    if (visitor != visitor_data->end())
      return static_cast<uint8_t>(visitor->second.symptom_id);
  }
  return kNoSymptomId;
}

void InfectorSymptomLookup::countIfGap(const TransmissionRecord& transmission) {
  if (transmission.source == InfectionSource::Person &&
      transmission.infector_symptom_id == kNoSymptomId)
    ++gap_count_;
}

std::string formatInfectorLookupGapWarning(uint64_t gap_count) {
  if (gap_count == 0) return "";
  return "[WARNING] infector symptom lookup: " + std::to_string(gap_count) +
         " infection(s) had a Person infector with no symptom found (no "
         "infector sampled, no Infection, or a visitor missing from the "
         "visitor data); they were judged with the infector symptom "
         "absent.\n";
}

}  // namespace june
