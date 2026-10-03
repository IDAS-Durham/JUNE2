#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#ifdef USE_MPI
#include <mpi.h>
#endif

int main(int argc, char** argv) {
#ifdef USE_MPI
  MPI_Init(&argc, &argv);
#endif

  doctest::Context context;
  context.applyCommandLine(argc, argv);
  const int result = context.run();

#ifdef USE_MPI
  MPI_Finalize();
#endif
  return result;
}
