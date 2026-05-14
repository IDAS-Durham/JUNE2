#pragma once

#include <fstream>
#include <iostream>
#include <string>
#ifdef USE_MPI
#include <mpi.h>
#endif
#include <unistd.h>

#include <iomanip>

#ifdef __APPLE__
#include <mach/mach.h>
#endif

namespace june {

class MemoryUtils {
 public:
  // Gets the current memory usage (RSS) in Kilobytes
  static size_t getRSS() {
#ifdef __APPLE__
    struct mach_task_basic_info info;
    mach_msg_type_number_t infoCount = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info,
                  &infoCount) != KERN_SUCCESS) {
      return 0;
    }
    return static_cast<size_t>(info.resident_size / 1024);
#else
    std::ifstream stat_stream("/proc/self/statm", std::ios_base::in);
    if (!stat_stream.is_open()) return 0;

    unsigned long size, resident, share, text, lib, data, dt;
    stat_stream >> size >> resident >> share >> text >> lib >> data >> dt;
    stat_stream.close();

    long page_size_kb = sysconf(_SC_PAGE_SIZE) / 1024;
    return resident * page_size_kb;
#endif
  }

  // Prints out current memory usage with a nice label
  static void logMemory(const std::string& label) {
    size_t rss_kb = getRSS();
    if (rss_kb == 0) return;

    double rss_gb = rss_kb / (1024.0 * 1024.0);

    int rank = 0;
#ifdef USE_MPI
    int initialized = 0;
    MPI_Initialized(&initialized);
    if (initialized) {
      MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    }
#endif
    if (rank == 0) {
      std::cout << "[MEMORY] " << std::left << std::setw(30) << label << ": "
                << std::fixed << std::setprecision(2) << rss_gb << " GB"
                << std::endl;
    }
  }

  // Collects and prints memory stats across all MPI ranks (Min/Max/Avg)
  static void logGlobalMemoryStats(const std::string& label) {
    size_t rss_kb = getRSS();
    double rss_gb = rss_kb / (1024.0 * 1024.0);

    double min_gb = rss_gb;
    double max_gb = rss_gb;
    double sum_gb = rss_gb;
    int rank = 0;
    int size = 1;

#ifdef USE_MPI
    int initialized = 0;
    MPI_Initialized(&initialized);
    if (initialized) {
      MPI_Comm_rank(MPI_COMM_WORLD, &rank);
      MPI_Comm_size(MPI_COMM_WORLD, &size);
      MPI_Allreduce(&rss_gb, &min_gb, 1, MPI_DOUBLE, MPI_MIN, MPI_COMM_WORLD);
      MPI_Allreduce(&rss_gb, &max_gb, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
      MPI_Allreduce(&rss_gb, &sum_gb, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
    }
#endif

    if (rank == 0) {
      double avg_gb = sum_gb / size;
      double imbalance = (avg_gb > 0) ? (max_gb / avg_gb - 1.0) * 100.0 : 0.0;

      std::cout << "[MEM_STATS] " << std::left << std::setw(30) << label << "\n"
                << "    Min: " << std::fixed << std::setprecision(2) << min_gb
                << " GB | "
                << "Max: " << max_gb << " GB | "
                << "Avg: " << avg_gb << " GB | "
                << "Imbalance: " << imbalance << "%" << std::endl;
    }
  }
};

}  // namespace june
