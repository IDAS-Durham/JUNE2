#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "utils/event_logging/event_merger.h"

int main(int argc, char* argv[]) {
  if (argc < 3) {
    std::cerr << "Usage: merge_events <output.h5> <rank0.h5> [rank1.h5 ...]\n"
              << "   or: merge_events <run_dir> <n_ranks>\n";
    return 1;
  }

  std::string output_file;
  std::vector<std::string> input_files;

  // Shorthand: merge_events <run_dir> <n_ranks>
  // Expands to: run_dir/simulation_events_rank{0..n-1}.h5 -> run_dir/simulation_events.h5
  if (argc == 3 && std::filesystem::is_directory(argv[1])) {
    std::filesystem::path run_dir(argv[1]);
    int n_ranks = std::stoi(argv[2]);
    output_file = (run_dir / "simulation_events.h5").string();
    for (int r = 0; r < n_ranks; ++r) {
      input_files.push_back(
          (run_dir / ("simulation_events_rank" + std::to_string(r) + ".h5"))
              .string());
    }
  } else {
    output_file = argv[1];
    for (int i = 2; i < argc; ++i) input_files.push_back(argv[i]);
  }

  std::cout << "Output: " << output_file << "\n";
  std::cout << "Inputs (" << input_files.size() << "):\n";
  for (const auto& f : input_files) std::cout << "  " << f << "\n";

  june::EventMerger::mergeEventFiles(input_files, output_file);
  return 0;
}
