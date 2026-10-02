#pragma once

#ifdef USE_MPI

#include "core/types.h"
#include "epidemiology/disease.h"
#include "epidemiology/emission/emission.h"
#include "parallel/domain.h"

namespace june {

class PolicyManager;

// Builds the record for `person` visiting `location`'s venue, owned by
// `home_rank`, for the slot starting at `slot_start` (days). The emission
// tails are the Person's Emission from `calculator`, so a Visitor emits
// bit-identically to a local; each tail is filled only when its header gate
// sends it (see visitor_wire.h). `disease` supplies susceptibility.
// `policy_manager` (nullptr: no modifiers) scales target_susceptibility by the
// Person's TargetSusceptibility modifier and the emission tails by their
// SourceInfectiousness modifier, as locals' are scaled at the Emission Site;
// deposition_source_multiplier carries the latter per deposition mode.
Domain::VisitorData buildVisitorPayload(const PersonLocation& location,
                                        const Person& person, int home_rank,
                                        double slot_start,
                                        const Disease& disease,
                                        const PolicyManager* policy_manager,
                                        const EmissionCalculator& calculator);

}  // namespace june

#endif  // USE_MPI
