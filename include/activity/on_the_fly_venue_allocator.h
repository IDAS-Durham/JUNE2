#pragma once

#include <functional>
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

  // Checks that all geo_unit_level names in rules exist in world.geo_level_names.
  // Throws std::runtime_error with a diagnostic if any are unknown.
  void checkConsistency(const WorldState& world) const;

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
  };
  // Non-owning probe key, avoids copying rule_name to test a cache hit.
  struct CacheKeyView {
    std::string_view rule_name;
    GeoUnitId geo_unit_id;
  };
  struct CacheKeyHash {
    using is_transparent = void;
    std::size_t operator()(const CacheKey& k) const {
      return hashOf(k.rule_name, k.geo_unit_id);
    }
    std::size_t operator()(const CacheKeyView& k) const {
      return hashOf(k.rule_name, k.geo_unit_id);
    }

   private:
    static std::size_t hashOf(std::string_view rule_name, GeoUnitId geo_unit_id) {
      return std::hash<std::string_view>{}(rule_name) ^
             (std::hash<GeoUnitId>{}(geo_unit_id) << 32);
    }
  };
  struct CacheKeyEqual {
    using is_transparent = void;
    bool operator()(const CacheKey& a, const CacheKey& b) const {
      return a.rule_name == b.rule_name && a.geo_unit_id == b.geo_unit_id;
    }
    bool operator()(const CacheKey& a, const CacheKeyView& b) const {
      return a.rule_name == b.rule_name && a.geo_unit_id == b.geo_unit_id;
    }
    bool operator()(const CacheKeyView& a, const CacheKey& b) const {
      return a.rule_name == b.rule_name && a.geo_unit_id == b.geo_unit_id;
    }
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

  std::unordered_map<std::string, std::string, TransparentStringHash,
                     std::equal_to<>>
      activity_to_rule_;
  std::unordered_map<std::string, RuleConfig> rules_;
  std::unordered_map<CacheKey, std::vector<VenueId>, CacheKeyHash, CacheKeyEqual>
      cache_;

  static const std::vector<VenueId> empty_pool_;
};

}  // namespace june
