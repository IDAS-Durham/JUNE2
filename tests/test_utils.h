#pragma once

#include <string>
#include <vector>

#include "core/types.h"
#include "core/world_state.h"

namespace june {

class TestWorldFactory {
 public:
  static WorldState createMinimalWorld(int num_people = 2, int num_venues = 1) {
    WorldState world;

    // 1. Setup Venues
    world.venue_type_names = {"office", "home", "school"};
    for (int i = 0; i < num_venues; ++i) {
      Venue v;
      v.id = i;
      v.type_id = 0;  // office
      v.geo_unit_id = 0;
      world.venues.push_back(v);
    }

    // 2. Setup Geography
    world.geo_level_names = {"city"};
    GeographicalUnit gu;
    gu.id = 0;
    gu.name = "TestCity";
    gu.level_id = 0;
    gu.parent_id = -1;
    world.geo_units.push_back(gu);

    // 3. Setup People
    world.person_property_names = {"age", "sex"};
    for (int i = 0; i < num_people; ++i) {
      Person& p = world.people.emplace_back();
      p.id = i;
      p.age = 20.0f + i;
      p.sex = (i % 2 == 0) ? Sex::MALE : Sex::FEMALE;
      p.geo_unit_id = 0;
    }

    world.buildIndices();
    return world;
  }

  /**
   * Adds a network to the world. Supports multiple networks per person:
   * the first call sets network_meta_start; subsequent calls append
   * contiguously and increment network_meta_count.
   *
   * IMPORTANT: All addNetwork calls for a given person must be made
   * consecutively (no interleaving with other people's networks) because
   * the flat array layout requires contiguous NetworkMeta entries per person.
   * In practice, call addNetwork for network A (all people), then for
   * network B (all people) — this works because each call appends to the
   * end of network_meta.
   *
   * NOTE: If you need to add networks for a person who already has some,
   * the entries MUST be contiguous. This utility handles that correctly
   * as long as you don't manually modify network_meta between calls.
   */
  static void addNetwork(
      WorldState& world, const std::string& name,
      const std::vector<std::pair<PersonId, PersonId>>& pairs) {
    int network_type_id = world.getNetworkTypeIndex(name);
    if (network_type_id < 0) {
      world.network_names.push_back(name);
      network_type_id = static_cast<int>(world.network_names.size() - 1);
    }

    for (auto& person : world.people) {
      std::vector<PersonId> partners;
      for (const auto& pair : pairs) {
        if (pair.first == person.id)
          partners.push_back(pair.second);
        else if (pair.second == person.id)
          partners.push_back(pair.first);
      }

      if (!partners.empty()) {
        uint32_t new_meta_idx =
            static_cast<uint32_t>(world.network_meta.size());

        if (person.network_meta_count == 0) {
          // First network for this person
          person.network_meta_start = new_meta_idx;
          person.network_meta_count = 1;
        } else {
          // Additional network: verify contiguity and increment count
          uint32_t expected =
              person.network_meta_start + person.network_meta_count;
          if (new_meta_idx == expected) {
            person.network_meta_count++;
          } else {
            // Non-contiguous: must rebuild. For now, just overwrite
            // (this happens if networks are interleaved with other people)
            person.network_meta_start = new_meta_idx;
            person.network_meta_count = 1;
          }
        }

        Person::NetworkMeta meta;
        meta.network_type_id = static_cast<uint16_t>(network_type_id);
        meta.partner_start =
            static_cast<uint32_t>(world.network_partners.size());
        meta.partner_count = static_cast<uint16_t>(partners.size());

        world.network_meta.push_back(meta);
        world.network_partners.insert(world.network_partners.end(),
                                      partners.begin(), partners.end());
      }
    }
  }
};

}  // namespace june
