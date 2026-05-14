#pragma once

namespace june {

struct Config;
struct WorldState;

// Run consistency checks on the fully-loaded Config against the WorldState.
// Throws std::runtime_error with a diagnostic message if any check fails.
// Should be called from Config::resolve() before sub-resolvers run.
void checkConfigConsistency(const Config& config, const WorldState& world);

}  // namespace june
