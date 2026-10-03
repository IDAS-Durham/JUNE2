#pragma once

#ifdef USE_MPI

#include <mpi.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace june {
namespace mpi_utils {

// Broadcast large buffers in MPI_INT-sized chunks.
inline void broadcastChunked(void* data, uint64_t count, size_t element_size,
                             MPI_Datatype type, int root) {
  const uint64_t max_mpi_count =
      static_cast<uint64_t>(std::numeric_limits<int>::max());
  uint64_t sent = 0;
  while (sent < count) {
    const int chunk = static_cast<int>(std::min(count - sent, max_mpi_count));
    MPI_Bcast(static_cast<char*>(data) + sent * element_size, chunk, type, root,
              MPI_COMM_WORLD);
    sent += static_cast<uint64_t>(chunk);
  }
}

// Compute MPI displacements from per-rank counts. Set scale to the byte size
// of each item when the counts are being converted for MPI_BYTE transfers.
inline void computeDisplacements(const std::vector<int>& counts,
                                 std::vector<int>& displs, int& total,
                                 int scale = 1) {
  displs.resize(counts.size(), 0);
  total = 0;
  for (size_t r = 0; r < counts.size(); ++r) {
    displs[r] = total;
    total += counts[r] * scale;
  }
}

// Gather packed 32-bit integer records from every rank, preserving rank
// boundaries in the returned slices.
inline std::vector<std::vector<int32_t>> allgathervInt32ByRank(
    const std::vector<int32_t>& local) {
  static_assert(sizeof(int32_t) == sizeof(int),
                "MPI_INT must be a 32-bit integer for packed exchanges");

  int world_size = 1;
  MPI_Comm_size(MPI_COMM_WORLD, &world_size);

  int local_count = static_cast<int>(local.size());
  std::vector<int> counts(world_size);
  MPI_Allgather(&local_count, 1, MPI_INT, counts.data(), 1, MPI_INT,
                MPI_COMM_WORLD);

  std::vector<int> displs;
  int total = 0;
  computeDisplacements(counts, displs, total);

  std::vector<int32_t> all(total);
  MPI_Allgatherv(local.data(), local_count, MPI_INT, all.data(), counts.data(),
                 displs.data(), MPI_INT, MPI_COMM_WORLD);

  std::vector<std::vector<int32_t>> by_rank(world_size);
  for (int r = 0; r < world_size; ++r) {
    by_rank[r].assign(all.begin() + displs[r],
                      all.begin() + displs[r] + counts[r]);
  }
  return by_rank;
}

// Gather packed 32-bit integer records from every rank in rank order.
inline std::vector<int32_t> allgathervInt32(const std::vector<int32_t>& local) {
  std::vector<int32_t> all;
  for (const auto& rank_values : allgathervInt32ByRank(local)) {
    all.insert(all.end(), rank_values.begin(), rank_values.end());
  }
  return all;
}

}  // namespace mpi_utils
}  // namespace june

#endif  // USE_MPI
