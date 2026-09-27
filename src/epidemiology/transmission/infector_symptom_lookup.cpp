#include "epidemiology/transmission/infector_symptom_lookup.h"

#include "core/world_state.h"
#include "epidemiology/disease.h"

namespace june {

uint16_t InfectorSymptomLookup::resolve(
    PersonId infector_id, double current_time,
    const std::unordered_map<PersonId, VisitorInfo>* visitor_data) {
  if (infector_id < 0) return kNoSymptomId;
  const Person* infector = world_.getPerson(infector_id);
  if (infector && infector->infection) {
    return infector->infection->getTrajectory().getCurrentSymptomId(
        current_time);
  }
  if (!infector && visitor_data) {
    auto visitor = visitor_data->find(infector_id);
    if (visitor != visitor_data->end()) return visitor->second.symptom_id;
  }
  ++gap_count_;
  return kNoSymptomId;
}

std::string formatInfectorLookupGapWarning(uint64_t gap_count) {
  if (gap_count == 0) return "";
  return "[WARNING] infector symptom lookup: " + std::to_string(gap_count) +
         " infection(s) had a Person infector with no symptom found (no "
         "Infection, or a visitor missing from the visitor data); they were "
         "judged with the infector symptom absent.\n";
}

}  // namespace june
