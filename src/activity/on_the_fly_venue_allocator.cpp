#include "activity/on_the_fly_venue_allocator.h"

#include <stdexcept>

#include <yaml-cpp/yaml.h>

#include "core/world_state.h"

namespace june {

const std::vector<VenueId> OnTheFlyVenueAllocator::empty_pool_{};

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

OnTheFlyVenueAllocator::OnTheFlyVenueAllocator(const std::string& config_path)
    : OnTheFlyVenueAllocator(YAML::LoadFile(config_path)) {}

OnTheFlyVenueAllocator OnTheFlyVenueAllocator::fromString(std::string_view yaml) {
  return OnTheFlyVenueAllocator(YAML::Load(std::string(yaml)));
}

OnTheFlyVenueAllocator::OnTheFlyVenueAllocator(const YAML::Node& root) {
  const auto& rules_node = root["rules"];
  for (const auto& entry : rules_node) {
    const std::string rule_name = entry.first.as<std::string>();
    const YAML::Node& cfg = entry.second;

    RuleConfig rule;

    const std::string strategy_str = cfg["strategy"].as<std::string>();
    if (strategy_str == "hosting_geo_unit") {
      rule.strategy = Strategy::hosting_geo_unit;
    } else if (strategy_str == "resident_geo_unit") {
      rule.strategy = Strategy::resident_geo_unit;
    } else {
      throw std::runtime_error("Unknown OTF strategy: " + strategy_str);
    }

    rule.venue_type = cfg["venue_type"].as<std::string>();

    if (cfg["venue_stability"]) {
      const std::string stability = cfg["venue_stability"].as<std::string>();
      if (stability == "fixed") {
        rule.venue_stability = VenueStability::fixed;
      } else if (stability == "daily") {
        rule.venue_stability = VenueStability::daily;
      } else {
        throw std::runtime_error("Unknown venue_stability: " + stability);
      }
    }

    if (cfg["geo_unit_level"]) {
      rule.geo_unit_level = cfg["geo_unit_level"].as<std::string>();
    }

    rules_[rule_name] = std::move(rule);
  }

  const auto& activity_rules_node = root["activity_rules"];
  for (const auto& entry : activity_rules_node) {
    activity_to_rule_[entry.first.as<std::string>()] =
        entry.second.as<std::string>();
  }
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

bool OnTheFlyVenueAllocator::hasRule(std::string_view activity_name) const {
  return activity_to_rule_.count(std::string(activity_name)) > 0;
}

bool OnTheFlyVenueAllocator::isFixed(std::string_view activity_name) const {
  auto rule_it = activity_to_rule_.find(std::string(activity_name));
  if (rule_it == activity_to_rule_.end()) return false;
  auto cfg_it = rules_.find(rule_it->second);
  if (cfg_it == rules_.end()) return false;
  return cfg_it->second.venue_stability == VenueStability::fixed;
}

const std::vector<VenueId>& OnTheFlyVenueAllocator::resolve(
    std::string_view activity_name, const VenueResolveContext& context,
    const WorldState& world) {
  auto rule_it = activity_to_rule_.find(std::string(activity_name));
  if (rule_it == activity_to_rule_.end()) return empty_pool_;

  const std::string& rule_name = rule_it->second;
  const RuleConfig& rule = rules_.at(rule_name);

  GeoUnitId geo_unit_id;
  if (rule.strategy == Strategy::hosting_geo_unit) {
    if (!context.hosting_geo_unit_id) return empty_pool_;
    geo_unit_id = *context.hosting_geo_unit_id;
  } else {
    geo_unit_id = context.resident_geo_unit_id;
  }

  CacheKey key{rule_name, geo_unit_id};
  auto cache_it = cache_.find(key);
  if (cache_it != cache_.end()) return cache_it->second;

  auto pool = world.getVenuesInGeoUnit(geo_unit_id, rule.venue_type);
  auto [inserted_it, _] = cache_.emplace(std::move(key), std::move(pool));
  return inserted_it->second;
}

}  // namespace june
