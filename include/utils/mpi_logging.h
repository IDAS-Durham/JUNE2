#pragma once

#include "utils/mpi_utils.h"

namespace june {

// True on MPI rank 0 (and unconditionally true when MPI is not initialised
// or not compiled in). Gates load-time log lines that would otherwise be
// repeated once per rank.
inline bool logRank0() { return mpi_runtime::state().rank == 0; }

}  // namespace june
