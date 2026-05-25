#pragma once

#include <H5Cpp.h>

#include <string>

// Helpers shared across event_writer.cpp and event_writer_lookups.cpp.
// File-private to the event_writer TUs; do not include from elsewhere.

namespace june::event_writer_detail {

// Open `name` under `parent` if it exists, otherwise create it.
inline H5::Group openOrCreateGroup(H5::Group& parent, const std::string& name) {
  if (H5Lexists(parent.getId(), name.c_str(), H5P_DEFAULT))
    return parent.openGroup(name);
  return parent.createGroup(name);
}

}  // namespace june::event_writer_detail
