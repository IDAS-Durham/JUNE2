#pragma once

#include <filesystem>
#include <string>

namespace june {

// Layout version of a checkpoint directory. Bump it whenever a dataset in
// state.h5 or a shard changes shape or meaning; restore reads only this one.
// Policy decisions are stored as 128-bit masks per person, and state.h5 holds
// the policy names they were written against.
constexpr int kCheckpointFormatVersion = 4;

// Write manifest.yaml into dir. It is written LAST; its presence is the
// atomic commit marker for a complete checkpoint.
void writeManifest(const std::filesystem::path& dir, int completed_day,
                   const std::string& date_iso, int nranks,
                   double current_simulation_time, unsigned int random_seed);

// Open the checkpoint's manifest.yaml and return the completed_day to resume
// from. Throws if the manifest is missing, its format_version is not
// kCheckpointFormatVersion, or its seed differs from config_seed (no silent
// override).
int readAndValidateManifest(const std::filesystem::path& checkpoint_dir,
                            unsigned int config_seed);

}  // namespace june
