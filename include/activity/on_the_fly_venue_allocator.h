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

  static OnTheFlyVenueAllocator fromString(std::string_view yaml);

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

 private:
  enum class VenueStability { daily, fixed };
  enum class Strategy { hosting_geo_unit, resident_geo_unit };

  struct RuleConfig {
    Strategy strategy;
    std::string venue_type;
    VenueStability venue_stability = VenueStability::daily;
    std::string geo_unit_level;  // optional; used by resident_geo_unit
  };

  // Cache key: (rule_name, geo_unit_id)
  struct CacheKey {
    std::string rule_name;
    GeoUnitId geo_unit_id;
    bool operator==(const CacheKey& other) const {
      return rule_name == other.rule_name && geo_unit_id == other.geo_unit_id;
    }
  };
  struct CacheKeyHash {
    std::size_t operator()(const CacheKey& k) const {
      return std::hash<std::string>{}(k.rule_name) ^
             (std::hash<GeoUnitId>{}(k.geo_unit_id) << 32);
    }
  };

  explicit OnTheFlyVenueAllocator(const YAML::Node& root);

  std::unordered_map<std::string, std::string> activity_to_rule_;
  std::unordered_map<std::string, RuleConfig> rules_;
  std::unordered_map<CacheKey, std::vector<VenueId>, CacheKeyHash> cache_;

  static const std::vector<VenueId> empty_pool_;
};

}  // namespace june
