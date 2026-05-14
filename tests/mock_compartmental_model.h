#pragma once

#include <dlfcn.h>

#include <string>
#include <vector>

#include "simulation/compartmental_model_plugin.h"

namespace june {

// ---------------------------------------------------------------------------
// InProcessMockPlugin
// Instantiate directly in step-injection tests — no .so or dlopen needed.
// ---------------------------------------------------------------------------
class InProcessMockPlugin : public ICompartmentalModel {
 public:
  int advance_count = 0;
  int snapshot_count = 0;
  float last_advance_dt = 0.0f;
  float last_advance_time = 0.0f;
  NodePartition received_partition;
  std::string received_config;

  // Records the last setCouplingInputs call for writeback verification.
  std::vector<float> last_coupling_inputs;
  int coupling_inputs_call_count = 0;

  void initialise(const NodePartition& p, const std::string& cfg) override {
    received_partition = p;
    received_config = cfg;
    owned_node_count_ = static_cast<int>(p.owned_node_indices.size());
  }
  void advance(float dt, float t) override {
    ++advance_count;
    last_advance_dt = dt;
    last_advance_time = t;
  }
  // Set to a non-zero value to drive uptake FOI in integration tests.
  float coupling_output_value = 0.0f;

  void getCouplingOutputs(float* out, int n) const override {
    for (int i = 0; i < n; ++i) out[i] = coupling_output_value;
  }
  void setCouplingInputs(const float* inputs, int n) override {
    ++coupling_inputs_call_count;
    last_coupling_inputs.assign(inputs, inputs + n);
  }
  void maybeSnapshot(float) override { ++snapshot_count; }
  int ownedNodeCount() const override { return owned_node_count_; }

 private:
  int owned_node_count_ = 0;
};

// ---------------------------------------------------------------------------
// MockQueryHandle
// Opens the mock .so (already loaded by the manager) via dlopen and resolves
// the extern "C" query symbols exported by mock_compartmental_model.cpp.
// Caller must ensure the .so exists at so_path; check valid before use.
// ---------------------------------------------------------------------------
struct MockQueryHandle {
  void* handle = nullptr;
  void (*reset)() = nullptr;
  int (*advance_count)() = nullptr;
  float (*advance_dt)(int) = nullptr;
  float (*advance_time)(int) = nullptr;
  int (*snapshot_count)() = nullptr;
  float (*snapshot_time)(int) = nullptr;
  bool valid = false;

  explicit MockQueryHandle(const char* so_path) {
    handle = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) return;
    reset = reinterpret_cast<void (*)()>(dlsym(handle, "mock_reset"));
    advance_count =
        reinterpret_cast<int (*)()>(dlsym(handle, "mock_advance_count"));
    advance_dt =
        reinterpret_cast<float (*)(int)>(dlsym(handle, "mock_advance_dt"));
    advance_time =
        reinterpret_cast<float (*)(int)>(dlsym(handle, "mock_advance_time"));
    snapshot_count =
        reinterpret_cast<int (*)()>(dlsym(handle, "mock_snapshot_count"));
    snapshot_time =
        reinterpret_cast<float (*)(int)>(dlsym(handle, "mock_snapshot_time"));
    valid = reset && advance_count && advance_dt && advance_time &&
            snapshot_count && snapshot_time;
  }
  ~MockQueryHandle() {
    if (handle) dlclose(handle);
  }

  MockQueryHandle(const MockQueryHandle&) = delete;
  MockQueryHandle& operator=(const MockQueryHandle&) = delete;
};

}  // namespace june
