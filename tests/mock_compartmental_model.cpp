#include "simulation/compartmental_model_plugin.h"

using namespace june;

// Sentinel value for ownedNodeCount() so tests can confirm the factory was
// used.
static constexpr int MOCK_OWNED_NODE_COUNT = 7;
static constexpr int MAX_RECORDED_CALLS = 1000;

// Call history — static so they survive plugin instance lifetime and can be
// queried via dlsym after the manager destroys the plugin instance.
static int g_advance_count = 0;
static float g_advance_dt[MAX_RECORDED_CALLS] = {};
static float g_advance_time[MAX_RECORDED_CALLS] = {};

static int g_snapshot_count = 0;
static float g_snapshot_time[MAX_RECORDED_CALLS] = {};

class MockCompartmentalModel : public ICompartmentalModel {
 public:
  void initialise(const NodePartition&, const std::string&) override {}

  void advance(float dt_days, float current_time_days) override {
    if (g_advance_count < MAX_RECORDED_CALLS) {
      g_advance_dt[g_advance_count] = dt_days;
      g_advance_time[g_advance_count] = current_time_days;
    }
    ++g_advance_count;
  }

  void getCouplingOutputs(float* out, int n_owned_nodes) const override {
    for (int i = 0; i < n_owned_nodes; ++i) out[i] = 0.0f;
  }

  void setCouplingInputs(const float*, int) override {}

  void maybeSnapshot(float current_time_days) override {
    if (g_snapshot_count < MAX_RECORDED_CALLS)
      g_snapshot_time[g_snapshot_count] = current_time_days;
    ++g_snapshot_count;
  }

  int ownedNodeCount() const override { return MOCK_OWNED_NODE_COUNT; }
};

extern "C" {
int compartmental_model_interface_version =
    COMPARTMENTAL_MODEL_INTERFACE_VERSION;

ICompartmentalModel* create_compartmental_model() {
  return new MockCompartmentalModel();
}

void destroy_compartmental_model(ICompartmentalModel* model) { delete model; }

// Query functions used by tests via dlsym.
void mock_reset() { g_advance_count = g_snapshot_count = 0; }
int mock_advance_count() { return g_advance_count; }
float mock_advance_dt(int i) {
  return (i >= 0 && i < g_advance_count) ? g_advance_dt[i] : -1.0f;
}
float mock_advance_time(int i) {
  return (i >= 0 && i < g_advance_count) ? g_advance_time[i] : -1.0f;
}
int mock_snapshot_count() { return g_snapshot_count; }
float mock_snapshot_time(int i) {
  return (i >= 0 && i < g_snapshot_count) ? g_snapshot_time[i] : -1.0f;
}
}
