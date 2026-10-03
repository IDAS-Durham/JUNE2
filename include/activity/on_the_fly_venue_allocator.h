#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "activity/venue_resolve_context.h"
#include "core/types.h"

namespace YAML {
class Node;
}

namespace june {

class WorldState;

class OnTheFlyVenueAllocator {
 public:
  explicit OnTheFlyVenueAllocator(const std::string& config_path);

  bool hasRule(std::string_view activity_name) const;

  // True when the rule for this activity uses venue_stability: fixed.
  // Callers should omit sim_day from their seed so the venue is stable
  // across multiple days of a hop.
  bool isFixed(std::string_view activity_name) const;

  // Returns memoised venue pool for the given activity and context.
  // Empty if no rule is defined or no venues match.
  const std::vector<VenueId>& resolve(std::string_view activity_name,
                                      const VenueResolveContext& context,
                                      const WorldState& world);

  // Checks that all geo_unit_level names in rules exist in
  // world.geo_level_names. Throws std::runtime_error with a diagnostic if any
  // are unknown.
  void checkConsistency(const WorldState& world) const;

  // Warm the cache with every pool this allocator can serve, then seal it, so
  // the caller can free the global venue maps. hosting_geo_units are the
  // calendar-event hosting units; resident-strategy rules scan world.people.
  // After sealing, resolve() throws on a cache miss instead of returning empty.
  void precomputeAllPools(const WorldState& world,
                          const std::vector<GeoUnitId>& hosting_geo_units);

 private:
  enum class VenueStability { daily, fixed };
  enum class Strategy { hosting_geo_unit, resident_geo_unit };

  struct RuleConfig {
    Strategy strategy;
    std::string venue_type;
    VenueStability venue_stability = VenueStability::daily;
    std::string geo_unit_level;  // optional; used by resident_geo_unit
  };

  // Transparent hash so callers can probe with string_view without
  // materialising a std::string.
  struct TransparentStringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const {
      return std::hash<std::string_view>{}(s);
    }
  };

  explicit OnTheFlyVenueAllocator(const YAML::Node& root);

  std::unordered_map<std::string, RuleConfig, TransparentStringHash,
                     std::equal_to<>>
      rules_;
  std::unordered_map<std::string,
                     std::unordered_map<GeoUnitId, std::vector<VenueId>>>
      cache_;

  // Set by precomputeAllPools(). Once sealed, a cache miss in resolve() is a
  // bug (the global maps have been freed), so resolve() throws.
  bool sealed_ = false;

  static const std::vector<VenueId> empty_pool_;
};

}  // namespace june
