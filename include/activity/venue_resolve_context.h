#pragma once

#include <optional>

#include "core/types.h"

namespace june {

struct VenueResolveContext {
  GeoUnitId resident_geo_unit_id = -1;
  std::optional<GeoUnitId> hosting_geo_unit_id;
};

}  // namespace june
