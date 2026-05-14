#include "utils/config_checks.h"

#include <stdexcept>
#include <string>

#include "core/config.h"
#include "core/world_state.h"

namespace june {

void checkConfigConsistency(const Config& config, const WorldState& world) {
  // --- BUG-S02: bitmask width ---
  // All activity and venue-type bitmask fields are uint32_t, so at most 32
  // distinct names are supported. Abort early rather than silently dropping
  // high-index activities or venue types.
  constexpr size_t kMaskWidth = 32;
  if (world.activity_names.size() > kMaskWidth) {
    throw std::runtime_error(
        "Configuration error: " + std::to_string(world.activity_names.size()) +
        " activity types registered, but bitmask width is only " +
        std::to_string(kMaskWidth) +
        ". Reduce the number of activity types or widen the mask type.");
  }
  if (world.venue_type_names.size() > kMaskWidth) {
    throw std::runtime_error(
        "Configuration error: " +
        std::to_string(world.venue_type_names.size()) +
        " venue types registered, but bitmask width is only " +
        std::to_string(kMaskWidth) +
        ". Reduce the number of venue types or widen the mask type.");
  }

  // --- DESIGN-01: default_schedule_type must name a defined schedule type ---
  // If the name does not match any loaded schedule type, the simulator falls
  // back silently to schedule_types[0], which may be a different type entirely.
  const auto& sched = config.schedule;
  if (!sched.schedule_types.empty()) {
    bool found = false;
    for (const auto& st : sched.schedule_types) {
      if (st.name == sched.default_schedule_type) {
        found = true;
        break;
      }
    }
    if (!found) {
      std::string valid;
      for (const auto& st : sched.schedule_types) {
        if (!valid.empty()) valid += ", ";
        valid += "\"" + st.name + "\"";
      }
      throw std::runtime_error(
          "Configuration error: default_schedule_type \"" +
          sched.default_schedule_type +
          "\" does not match any defined schedule type. Valid names: " + valid +
          ".");
    }
  }

  // --- BUG-S05: all schedule types must share the same slot structure ---
  // The simulator uses schedule_types[0] as the reference for time-slot
  // boundaries. If other schedule types define different slot counts or
  // boundaries, persons assigned to them will be processed with incorrect
  // slot durations.
  if (sched.schedule_types.size() > 1) {
    const auto& ref = sched.schedule_types[0];
    for (size_t i = 1; i < sched.schedule_types.size(); ++i) {
      const auto& st = sched.schedule_types[i];
      // Temporary schedules use flat_slots and have no day-type structure to
      // compare
      if (st.is_temporary) continue;
      const std::string prefix =
          "Configuration error: schedule type \"" + st.name + "\"";

      for (const auto& [dt_name, ref_slots] : ref.slots_by_day_type) {
        auto it = st.slots_by_day_type.find(dt_name);
        if (it == st.slots_by_day_type.end()) {
          throw std::runtime_error(
              prefix + " is missing day type \"" + dt_name +
              "\" which is present in \"" + ref.name +
              "\". All schedule types must share the same slot structure.");
        }
        const auto& st_slots = it->second;
        if (st_slots.size() != ref_slots.size()) {
          throw std::runtime_error(
              prefix + " has " + std::to_string(st_slots.size()) +
              " slot(s) for day type \"" + dt_name + "\" but \"" + ref.name +
              "\" (the reference) has " + std::to_string(ref_slots.size()) +
              ". All schedule types must share the same slot structure.");
        }
        for (size_t s = 0; s < ref_slots.size(); ++s) {
          if (st_slots[s].start != ref_slots[s].start ||
              st_slots[s].end != ref_slots[s].end) {
            throw std::runtime_error(
                prefix + " day type \"" + dt_name + "\" slot " +
                std::to_string(s) + " (start=" + st_slots[s].start +
                ", end=" + st_slots[s].end +
                ") does not match the reference slot from \"" + ref.name +
                "\" (start=" + ref_slots[s].start +
                ", end=" + ref_slots[s].end +
                "). All schedule types must share the same time boundaries.");
          }
        }
      }
    }
  }
}

}  // namespace june
