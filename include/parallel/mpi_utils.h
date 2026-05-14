#pragma once

#ifdef USE_MPI

#include <vector>

namespace june {
namespace mpi_utils {

// Compute MPI displacements from per-rank item counts.
// Used by Allgatherv, Alltoallv, and similar collectives.
inline void computeDisplacements(const std::vector<int>& counts,
                                 std::vector<int>& displs, int& total) {
  displs.resize(counts.size(), 0);
  total = 0;
  for (size_t r = 0; r < counts.size(); ++r) {
    displs[r] = total;
    total += counts[r];
  }
}

// Compute byte-scaled displacements from per-rank item counts.
// Multiplies each count by item_size for MPI_BYTE transfers.
inline void computeByteDisplacements(const std::vector<int>& counts,
                                     int item_size, std::vector<int>& displs,
                                     int& total_bytes) {
  displs.resize(counts.size(), 0);
  total_bytes = 0;
  for (size_t r = 0; r < counts.size(); ++r) {
    displs[r] = total_bytes;
    total_bytes += counts[r] * item_size;
  }
}

// Scale item counts and displacements to byte counts and displacements.
inline void scaleCountsToBytes(const std::vector<int>& counts,
                               const std::vector<int>& displs, int item_size,
                               std::vector<int>& byte_counts,
                               std::vector<int>& byte_displs) {
  byte_counts.resize(counts.size());
  byte_displs.resize(counts.size());
  for (size_t i = 0; i < counts.size(); ++i) {
    byte_counts[i] = counts[i] * item_size;
    byte_displs[i] = displs[i] * item_size;
  }
}

}  // namespace mpi_utils
}  // namespace june

#endif  // USE_MPI
