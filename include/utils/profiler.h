#pragma once

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stack>
#include <string>
#include <unordered_map>
#include <vector>

namespace june {

// =============================================================================
// Enhanced Profiler - Detailed function-level profiling
// =============================================================================

struct CallInfo {
  std::string caller;
  size_t call_count = 0;
  double total_time = 0.0;
};

struct ProfileEntry {
  std::string name;
  size_t call_count = 0;       // Number of times called
  size_t primitive_calls = 0;  // Non-recursive calls
  double total_time =
      0.0;  // Total time in this function only (excludes subcalls)
  double cumulative_time = 0.0;  // Cumulative time including subcalls
  std::chrono::steady_clock::time_point start_time;

  // Track callers and callees
  std::map<std::string, CallInfo> callers;  // Who called this function
  std::map<std::string, CallInfo> callees;  // Who this function called

  double avg_total_time() const {
    return call_count > 0 ? total_time / call_count : 0.0;
  }

  double avg_cumulative_time() const {
    return call_count > 0 ? cumulative_time / call_count : 0.0;
  }
};

class Profiler {
 public:
  static Profiler& instance() {
    static Profiler prof;
    return prof;
  }

  // Enable/disable profiling
  void enable() { enabled_ = true; }
  void disable() { enabled_ = false; }
  bool isEnabled() const { return enabled_; }

  // Start timing a function
  void start(const std::string& name) {
    if (!enabled_) return;

    auto now = std::chrono::steady_clock::now();

    // Track parent-child relationship
    if (!call_stack_.empty()) {
      const std::string& parent = call_stack_.top().function_name;

      // Record that parent calls this function
      auto& parent_entry = entries_[parent];
      parent_entry.callees[name].call_count++;
      parent_entry.callees[name].caller = parent;

      // Record that this function is called by parent
      auto& child_entry = entries_[name];
      child_entry.callers[parent].call_count++;
      child_entry.callers[parent].caller = parent;
    }

    // Push to call stack
    CallFrame frame;
    frame.function_name = name;
    frame.start_time = now;
    frame.cumulative_start = now;
    call_stack_.push(frame);

    auto& entry = entries_[name];
    entry.name = name;
    entry.start_time = now;
    entry.call_count++;
    entry.primitive_calls++;
  }

  // Stop timing a function
  void stop(const std::string& name) {
    if (!enabled_) return;

    auto end_time = std::chrono::steady_clock::now();

    if (call_stack_.empty()) {
      std::cerr << "Warning: stop() called without matching start() for "
                << name << std::endl;
      return;
    }

    CallFrame& frame = call_stack_.top();
    if (frame.function_name != name) {
      std::cerr << "Warning: stop() mismatch. Expected " << frame.function_name
                << " but got " << name << std::endl;
      return;
    }

    auto& entry = entries_[name];

    // Calculate time spent in this function (excluding time spent in children)
    double elapsed =
        std::chrono::duration<double>(end_time - frame.start_time).count();
    double self_time = elapsed - frame.time_in_children;

    entry.total_time += self_time;

    // Calculate cumulative time (including all children)
    double cumulative =
        std::chrono::duration<double>(end_time - frame.cumulative_start)
            .count();
    entry.cumulative_time += cumulative;

    // Update caller's time_in_children
    call_stack_.pop();
    if (!call_stack_.empty()) {
      call_stack_.top().time_in_children += cumulative;

      // Update callee timing in parent
      const std::string& parent = call_stack_.top().function_name;
      auto& parent_entry = entries_[parent];
      parent_entry.callees[name].total_time += cumulative;
    }
  }

  // Reset all profiling data
  void reset() {
    entries_.clear();
    while (!call_stack_.empty()) {
      call_stack_.pop();
    }
  }

  // Get total primitive calls
  size_t getTotalCalls() const {
    size_t total = 0;
    for (const auto& [name, entry] : entries_) {
      total += entry.call_count;
    }
    return total;
  }

  // Get total primitive calls (non-recursive)
  size_t getTotalPrimitiveCalls() const {
    size_t total = 0;
    for (const auto& [name, entry] : entries_) {
      total += entry.primitive_calls;
    }
    return total;
  }

  // Print detailed results
  void printDetailedResults(std::ostream& os = std::cout,
                            size_t top_n = 100) const {
    if (entries_.empty()) {
      os << "No profiling data collected." << std::endl;
      return;
    }

    // Calculate totals
    double total_time = 0.0;
    for (const auto& [name, entry] : entries_) {
      total_time += entry.total_time;
    }

    size_t total_calls = getTotalCalls();
    size_t primitive_calls = getTotalPrimitiveCalls();

    // Sort by cumulative time
    std::vector<const ProfileEntry*> sorted_entries;
    for (const auto& [name, entry] : entries_) {
      sorted_entries.push_back(&entry);
    }
    std::sort(sorted_entries.begin(), sorted_entries.end(),
              [](const ProfileEntry* a, const ProfileEntry* b) {
                return a->cumulative_time > b->cumulative_time;
              });

    // Print header
    os << "\n" << std::string(120, '=') << "\n";
    os << "DETAILED PROFILING RESULTS\n";
    os << std::string(120, '=') << "\n";
    os << "Total function calls: " << total_calls << " (" << primitive_calls
       << " primitive)\n";
    os << "Total time: " << formatTime(total_time) << "\n\n";

    os << std::string(120, '=') << "\n";
    os << "TOP " << std::min(top_n, sorted_entries.size())
       << " FUNCTIONS BY CUMULATIVE TIME\n";
    os << std::string(120, '=') << "\n\n";

    // Print table header
    os << std::right << std::setw(10) << "ncalls" << std::setw(12) << "tottime"
       << std::setw(12) << "percall" << std::setw(12) << "cumtime"
       << std::setw(12) << "percall"
       << "  " << std::left << "function"
       << "\n";
    os << std::string(120, '-') << "\n";

    // Print entries
    size_t count = 0;
    for (const auto* entry : sorted_entries) {
      if (count++ >= top_n) break;

      os << std::right << std::setw(10) << entry->call_count << std::setw(12)
         << formatTime(entry->total_time) << std::setw(12)
         << formatTime(entry->avg_total_time()) << std::setw(12)
         << formatTime(entry->cumulative_time) << std::setw(12)
         << formatTime(entry->avg_cumulative_time()) << "  " << std::left
         << entry->name << "\n";
    }

    os << "\n" << std::string(120, '=') << "\n\n";
  }

  // Print results sorted by total time
  void printByTotalTime(std::ostream& os = std::cout, size_t top_n = 0) const {
    std::vector<const ProfileEntry*> sorted_entries;
    for (const auto& [name, entry] : entries_) {
      sorted_entries.push_back(&entry);
    }
    std::sort(sorted_entries.begin(), sorted_entries.end(),
              [](const ProfileEntry* a, const ProfileEntry* b) {
                return a->total_time > b->total_time;
              });

    double total_time = 0.0;
    for (const auto& [name, entry] : entries_) {
      total_time += entry.total_time;
    }

    // If top_n is 0, show all entries
    size_t num_to_show = (top_n == 0) ? sorted_entries.size()
                                      : std::min(top_n, sorted_entries.size());

    os << "\n" << std::string(120, '=') << "\n";
    if (top_n == 0) {
      os << "ALL " << sorted_entries.size() << " FUNCTIONS BY TOTAL TIME\n";
    } else {
      os << "TOP " << num_to_show << " FUNCTIONS BY TOTAL TIME\n";
    }
    os << std::string(120, '=') << "\n\n";

    os << std::right << std::setw(10) << "ncalls" << std::setw(12) << "tottime"
       << std::setw(12) << "percall" << std::setw(12) << "cumtime"
       << std::setw(12) << "percall"
       << "  " << std::left << "function"
       << "\n";
    os << std::string(120, '-') << "\n";

    for (size_t i = 0; i < num_to_show; ++i) {
      const auto* entry = sorted_entries[i];
      os << std::right << std::setw(10) << entry->call_count << std::setw(12)
         << formatTime(entry->total_time) << std::setw(12)
         << formatTime(entry->avg_total_time()) << std::setw(12)
         << formatTime(entry->cumulative_time) << std::setw(12)
         << formatTime(entry->avg_cumulative_time()) << "  " << std::left
         << entry->name << "\n";
    }

    os << "\n" << std::string(120, '=') << "\n\n";
  }

  // Print caller/callee information for a specific function
  void printCallersCallees(const std::string& function_name,
                           std::ostream& os = std::cout) const {
    auto it = entries_.find(function_name);
    if (it == entries_.end()) {
      os << "Function '" << function_name << "' not found in profile data.\n";
      return;
    }

    const ProfileEntry& entry = it->second;

    os << "\n" << std::string(120, '=') << "\n";
    os << "CALLERS/CALLEES FOR: " << function_name << "\n";
    os << std::string(120, '=') << "\n\n";

    // Print callers
    if (!entry.callers.empty()) {
      os << "Called by:\n";
      os << std::string(120, '-') << "\n";
      os << std::right << std::setw(10) << "ncalls" << std::setw(15)
         << "cumtime"
         << "  " << std::left << "caller"
         << "\n";
      os << std::string(120, '-') << "\n";

      for (const auto& [caller_name, info] : entry.callers) {
        os << std::right << std::setw(10) << info.call_count << std::setw(15)
           << formatTime(info.total_time) << "  " << std::left << caller_name
           << "\n";
      }
      os << "\n";
    } else {
      os << "No callers (root function)\n\n";
    }

    // Print callees
    if (!entry.callees.empty()) {
      os << "Calls:\n";
      os << std::string(120, '-') << "\n";
      os << std::right << std::setw(10) << "ncalls" << std::setw(15)
         << "cumtime"
         << "  " << std::left << "callee"
         << "\n";
      os << std::string(120, '-') << "\n";

      // Sort callees by cumulative time
      std::vector<std::pair<std::string, CallInfo>> sorted_callees(
          entry.callees.begin(), entry.callees.end());
      std::sort(sorted_callees.begin(), sorted_callees.end(),
                [](const auto& a, const auto& b) {
                  return a.second.total_time > b.second.total_time;
                });

      for (const auto& [callee_name, info] : sorted_callees) {
        os << std::right << std::setw(10) << info.call_count << std::setw(15)
           << formatTime(info.total_time) << "  " << std::left << callee_name
           << "\n";
      }
      os << "\n";
    } else {
      os << "No callees (leaf function)\n\n";
    }

    os << std::string(120, '=') << "\n";
  }

  // Print bottlenecks (functions taking >5% of total time)
  void printBottlenecks(std::ostream& os = std::cout) const {
    std::vector<const ProfileEntry*> sorted_entries;
    for (const auto& [name, entry] : entries_) {
      sorted_entries.push_back(&entry);
    }
    std::sort(sorted_entries.begin(), sorted_entries.end(),
              [](const ProfileEntry* a, const ProfileEntry* b) {
                return a->total_time > b->total_time;
              });

    double total_time = 0.0;
    for (const auto& [name, entry] : entries_) {
      total_time += entry.total_time;
    }

    os << "\n" << std::string(120, '=') << "\n";
    os << "POTENTIAL BOTTLENECKS (>5% of total time)\n";
    os << std::string(120, '=') << "\n\n";

    bool found_bottleneck = false;
    for (const auto* entry : sorted_entries) {
      double percent =
          (total_time > 0) ? (entry->total_time / total_time * 100.0) : 0.0;
      if (percent > 5.0) {
        found_bottleneck = true;
        os << "  • " << entry->name << "\n"
           << "    Total time: " << formatTime(entry->total_time) << " ("
           << std::fixed << std::setprecision(2) << percent << "% of total)\n"
           << "    Cumulative time: " << formatTime(entry->cumulative_time)
           << "\n"
           << "    Calls: " << entry->call_count
           << ", Avg (total): " << formatTime(entry->avg_total_time())
           << ", Avg (cumulative): " << formatTime(entry->avg_cumulative_time())
           << "\n\n";
      }
    }

    if (!found_bottleneck) {
      os << "No clear bottlenecks identified (no function taking >5% of total "
            "time).\n";
    }

    os << std::string(120, '=') << "\n";
  }

  // Save complete analysis to file
  void saveCompleteAnalysis(const std::string& filename) const {
    std::ofstream file(filename);
    if (!file.is_open()) {
      std::cerr << "Failed to open file: " << filename << std::endl;
      return;
    }

    // Print all analyses (0 = show ALL functions)
    printDetailedResults(file, 0);
    printByTotalTime(file, 0);
    printBottlenecks(file);

    // Print caller/callee for top 10 functions
    std::vector<const ProfileEntry*> sorted_entries;
    for (const auto& [name, entry] : entries_) {
      sorted_entries.push_back(&entry);
    }
    std::sort(sorted_entries.begin(), sorted_entries.end(),
              [](const ProfileEntry* a, const ProfileEntry* b) {
                return a->cumulative_time > b->cumulative_time;
              });

    file << "\n" << std::string(120, '=') << "\n";
    file << "DETAILED CALLER/CALLEE ANALYSIS FOR TOP 10 FUNCTIONS\n";
    file << std::string(120, '=') << "\n";

    for (size_t i = 0; i < std::min(size_t(10), sorted_entries.size()); ++i) {
      printCallersCallees(sorted_entries[i]->name, file);
    }

    file.close();
    std::cout << "Complete profiling analysis saved to: " << filename
              << std::endl;
  }

  // Legacy simple output
  void printResults(std::ostream& os = std::cout) const {
    printDetailedResults(os, 50);
  }

 private:
  Profiler() : enabled_(false) {}

  struct CallFrame {
    std::string function_name;
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point cumulative_start;
    double time_in_children = 0.0;
  };

  bool enabled_;
  std::unordered_map<std::string, ProfileEntry> entries_;
  std::stack<CallFrame> call_stack_;

  // Format time in human-readable format
  static std::string formatTime(double seconds) {
    if (seconds < 0.000001) {
      return std::to_string(static_cast<int>(seconds * 1e9)) + " ns";
    } else if (seconds < 0.001) {
      std::ostringstream oss;
      oss << std::fixed << std::setprecision(2) << (seconds * 1e6) << " μs";
      return oss.str();
    } else if (seconds < 1.0) {
      std::ostringstream oss;
      oss << std::fixed << std::setprecision(2) << (seconds * 1000) << " ms";
      return oss.str();
    } else {
      std::ostringstream oss;
      oss << std::fixed << std::setprecision(4) << seconds << " s";
      return oss.str();
    }
  }
};

// =============================================================================
// RAII-style scope timer for automatic start/stop
// =============================================================================

class ScopedTimer {
 public:
  explicit ScopedTimer(const std::string& name) : name_(name) {
    Profiler::instance().start(name_);
  }

  ~ScopedTimer() { Profiler::instance().stop(name_); }

 private:
  std::string name_;
};

// Attribute to exclude functions from automatic profiling (when using
// -finstrument-functions)
#define NO_PROFILE __attribute__((no_instrument_function))

}  // namespace june

// =============================================================================
// Compiler instrumentation hooks for automatic profiling
// =============================================================================

extern "C" {
void __cyg_profile_func_enter(void* func, void* caller)
    __attribute__((no_instrument_function));
void __cyg_profile_func_exit(void* func, void* caller)
    __attribute__((no_instrument_function));
}
