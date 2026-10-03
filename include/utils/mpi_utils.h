#pragma once

#ifdef USE_MPI
#include <mpi.h>
#endif

namespace june::mpi_runtime {

struct State {
  int rank = 0;
  int size = 1;
  bool active = false;
};

// Return the current communicator state, or serial defaults before MPI_Init,
// after MPI_Finalize, or in a serial build.
inline State state() {
  State result;
#ifdef USE_MPI
  int initialized = 0;
  MPI_Initialized(&initialized);
  if (!initialized) return result;

  int finalized = 0;
  MPI_Finalized(&finalized);
  if (finalized) return result;

  MPI_Comm_rank(MPI_COMM_WORLD, &result.rank);
  MPI_Comm_size(MPI_COMM_WORLD, &result.size);
  result.active = true;
#endif
  return result;
}

}  // namespace june::mpi_runtime
