#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <hdf5.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "doctest.h"
#include "mock_compartmental_model.h"
#include "simulation/compartmental_model_manager.h"

using namespace june;

#ifdef MOCK_COMPARTMENTAL_MODEL_SO_PATH
// Evaluated once at process start; guards all dlsym-dependent tests so they
// degrade to a logged skip rather than a hard failure when the .so is absent.
static const bool kMockSoAccessible =
    std::filesystem::exists(MOCK_COMPARTMENTAL_MODEL_SO_PATH);

#define SKIP_IF_NO_MOCK()                        \
  do {                                           \
    if (!kMockSoAccessible) {                    \
      WARN("mock .so not found — test skipped"); \
      return;                                    \
    }                                            \
  } while (0)
#endif

// Writes a minimal sidecar YAML to a temp file and returns the path.
static std::string writeTempSidecar(const std::string& so_path,
                                    const std::string& model_config = "",
                                    const std::string& graph_h5 = "") {
  std::string path = "/tmp/test_compartmental_sidecar.yaml";
  std::ofstream f(path);
  f << "plugin_so_path: \"" << so_path << "\"\n";
  f << "model_config_path: \"" << model_config << "\"\n";
  f << "graph_hdf5_path: \"" << graph_h5 << "\"\n";
  f << "coupling:\n  beta: 0.001\n";
  return path;
}

// Creates a synthetic 4-node graph HDF5:
//   Node 0 → venues [10, 20]   Node 1 → venues [30]
//   Node 2 → venues [40, 50]   Node 3 → venues [60]
static std::string writeSyntheticGraph(const std::string& path) {
  hid_t file_id =
      H5Fcreate(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
  REQUIRE(file_id >= 0);
  H5Gcreate2(file_id, "/nodes", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);

  auto write1D = [&](const char* name, const int32_t* buf, hsize_t n) {
    hid_t space = H5Screate_simple(1, &n, nullptr);
    hid_t ds = H5Dcreate2(file_id, name, H5T_NATIVE_INT32, space, H5P_DEFAULT,
                          H5P_DEFAULT, H5P_DEFAULT);
    H5Dwrite(ds, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf);
    H5Dclose(ds);
    H5Sclose(space);
  };

  const int32_t ptr[] = {0, 2, 3, 5, 6};
  const int32_t data[] = {10, 20, 30, 40, 50, 60};
  write1D("/nodes/venue_id_ptr", ptr, 5);
  write1D("/nodes/venue_id_data", data, 6);
  H5Fclose(file_id);
  return path;
}

// =============================================================================
// Null-object tests (no .so required)
// =============================================================================

TEST_CASE("CompartmentalModelManager null-object: no plugin configured") {
  CompartmentalModelManager mgr("", nullptr);

  CHECK(mgr.isActive() == false);
  CHECK(mgr.ownedNodeCount() == 0);
  CHECK_NOTHROW(mgr.advance(1.0f / 24.0f, 0.0f));
  CHECK_NOTHROW(mgr.maybeSnapshot(0.0f));
  CHECK(mgr.readCouplingOutputs() == nullptr);
  CHECK(mgr.venueToLocalNodeIndex(42) == -1);
}

TEST_CASE(
    "CompartmentalModelManager null-object: writeCouplingInputs is no-op") {
  CompartmentalModelManager mgr("", nullptr);
  const float data[3] = {1.0f, 2.0f, 3.0f};
  CHECK_NOTHROW(mgr.writeCouplingInputs(data, 3));
}

// =============================================================================
// dlsym-based tests (require mock .so at runtime)
// =============================================================================

#ifdef MOCK_COMPARTMENTAL_MODEL_SO_PATH
TEST_CASE("CompartmentalModelManager loads mock .so and is active") {
  SKIP_IF_NO_MOCK();
  std::string sidecar = writeTempSidecar(MOCK_COMPARTMENTAL_MODEL_SO_PATH);
  CompartmentalModelManager mgr(sidecar, nullptr);

  CHECK(mgr.isActive() == true);
  CHECK(mgr.ownedNodeCount() == 7);  // sentinel from mock
}

TEST_CASE(
    "CompartmentalModelManager forwards advance and maybeSnapshot to plugin") {
  SKIP_IF_NO_MOCK();
  std::string sidecar = writeTempSidecar(MOCK_COMPARTMENTAL_MODEL_SO_PATH);
  CompartmentalModelManager mgr(sidecar, nullptr);

  REQUIRE(mgr.isActive());
  CHECK_NOTHROW(mgr.advance(1.0f / 24.0f, 0.0f));
  CHECK_NOTHROW(mgr.maybeSnapshot(0.0f));
}

TEST_CASE(
    "CompartmentalModelManager readCouplingOutputs returns buffer of correct "
    "length") {
  SKIP_IF_NO_MOCK();
  std::string sidecar = writeTempSidecar(MOCK_COMPARTMENTAL_MODEL_SO_PATH);
  CompartmentalModelManager mgr(sidecar, nullptr);

  REQUIRE(mgr.isActive());
  const float* buf = mgr.readCouplingOutputs();
  REQUIRE(buf != nullptr);
  CHECK(buf[0] == doctest::Approx(0.0f));
}
#endif

TEST_CASE("CompartmentalModelManager: nonexistent .so path is inactive") {
  std::string sidecar = writeTempSidecar("/nonexistent/path/libplugin.so");
  CompartmentalModelManager mgr(sidecar, nullptr);
  CHECK(mgr.isActive() == false);
}

#ifdef MOCK_COMPARTMENTAL_MODEL_SO_PATH
TEST_CASE(
    "CompartmentalModelManager: venue-to-node map built from graph HDF5") {
  SKIP_IF_NO_MOCK();
  std::string graph = writeSyntheticGraph("/tmp/test_compartmental_graph.h5");
  std::string sidecar =
      writeTempSidecar(MOCK_COMPARTMENTAL_MODEL_SO_PATH, "", graph);
  CompartmentalModelManager mgr(sidecar, nullptr);

  REQUIRE(mgr.isActive());
  CHECK(mgr.venueToLocalNodeIndex(10) == 0);
  CHECK(mgr.venueToLocalNodeIndex(20) == 0);
  CHECK(mgr.venueToLocalNodeIndex(30) == 1);
  CHECK(mgr.venueToLocalNodeIndex(40) == 2);
  CHECK(mgr.venueToLocalNodeIndex(50) == 2);
  CHECK(mgr.venueToLocalNodeIndex(60) == 3);
  CHECK(mgr.venueToLocalNodeIndex(99) == -1);
}

TEST_CASE("Manager forwards advance() arguments correctly to plugin") {
  SKIP_IF_NO_MOCK();
  std::string sidecar = writeTempSidecar(MOCK_COMPARTMENTAL_MODEL_SO_PATH);
  MockQueryHandle q(MOCK_COMPARTMENTAL_MODEL_SO_PATH);
  REQUIRE(q.valid);
  q.reset();

  {
    CompartmentalModelManager mgr(sidecar, nullptr);
    REQUIRE(mgr.isActive());
    mgr.advance(8.0f / 24.0f, 0.0f);
    mgr.advance(6.0f / 24.0f, 8.0f / 24.0f);
  }

  CHECK(q.advance_count() == 2);
  CHECK(q.advance_dt(0) == doctest::Approx(8.0f / 24.0f).epsilon(1e-5f));
  CHECK(q.advance_time(0) == doctest::Approx(0.0f).epsilon(1e-5f));
  CHECK(q.advance_dt(1) == doctest::Approx(6.0f / 24.0f).epsilon(1e-5f));
  CHECK(q.advance_time(1) == doctest::Approx(8.0f / 24.0f).epsilon(1e-5f));
  CHECK(q.advance_time(1) > q.advance_time(0));
}

TEST_CASE("Manager forwards maybeSnapshot() arguments correctly to plugin") {
  SKIP_IF_NO_MOCK();
  std::string sidecar = writeTempSidecar(MOCK_COMPARTMENTAL_MODEL_SO_PATH);
  MockQueryHandle q(MOCK_COMPARTMENTAL_MODEL_SO_PATH);
  REQUIRE(q.valid);
  q.reset();

  {
    CompartmentalModelManager mgr(sidecar, nullptr);
    REQUIRE(mgr.isActive());
    mgr.maybeSnapshot(0.0f);
    mgr.maybeSnapshot(8.0f / 24.0f);
    mgr.maybeSnapshot(14.0f / 24.0f);
  }

  CHECK(q.snapshot_count() == 3);
  CHECK(q.snapshot_time(0) == doctest::Approx(0.0f).epsilon(1e-5f));
  CHECK(q.snapshot_time(1) == doctest::Approx(8.0f / 24.0f).epsilon(1e-5f));
  CHECK(q.snapshot_time(2) == doctest::Approx(14.0f / 24.0f).epsilon(1e-5f));
}

TEST_CASE("Null-object manager makes zero advance and snapshot calls") {
  SKIP_IF_NO_MOCK();
  MockQueryHandle q(MOCK_COMPARTMENTAL_MODEL_SO_PATH);
  REQUIRE(q.valid);
  q.reset();

  {
    CompartmentalModelManager null_mgr("", nullptr);
    REQUIRE_FALSE(null_mgr.isActive());
    null_mgr.advance(1.0f / 24.0f, 0.0f);
    null_mgr.maybeSnapshot(0.0f);
  }

  CHECK(q.advance_count() == 0);
  CHECK(q.snapshot_count() == 0);
}
#endif

// =============================================================================
// Step-injection tests — no .so or HDF5 required
// =============================================================================

TEST_CASE(
    "Step injection: loadSidecar throws → manager inactive, later steps not "
    "called") {
  bool plugin_loaded = false;
  CompartmentalModelSteps steps;
  steps.loadSidecar = [](const std::string&) -> PluginSidecarConfig {
    throw std::runtime_error("simulated bad YAML");
  };
  steps.loadPlugin = [&](const std::string&, DestroyCompartmentalModelFn&,
                         void*&) -> ICompartmentalModel* {
    plugin_loaded = true;
    return nullptr;
  };

  CompartmentalModelManager mgr("any.yaml", nullptr, std::move(steps));
  CHECK(mgr.isActive() == false);
  CHECK(plugin_loaded == false);
}

TEST_CASE(
    "Step injection: loadPlugin returns nullptr → manager inactive, "
    "buildPartition not called") {
  bool build_called = false;
  CompartmentalModelSteps steps;
  steps.loadSidecar = [](const std::string&) -> PluginSidecarConfig {
    PluginSidecarConfig cfg;
    cfg.plugin_so_path = "libplugin.so";
    cfg.model_config_path = "cfg.yaml";
    cfg.graph_hdf5_path = "graph.h5";
    return cfg;
  };
  steps.loadPlugin = [](const std::string&, DestroyCompartmentalModelFn&,
                        void*&) -> ICompartmentalModel* { return nullptr; };
  steps.buildPartition = [&](const std::string&, DomainManager*,
                             std::unordered_map<int, int>&) -> NodePartition {
    build_called = true;
    return {};
  };

  CompartmentalModelManager mgr("any.yaml", nullptr, std::move(steps));
  CHECK(mgr.isActive() == false);
  CHECK(build_called == false);
}

TEST_CASE("Step injection: venue map built from injected partition (no HDF5)") {
  InProcessMockPlugin mock_plugin;
  CompartmentalModelSteps steps;
  steps.loadSidecar = [](const std::string&) -> PluginSidecarConfig {
    PluginSidecarConfig cfg;
    cfg.plugin_so_path = "ignored.so";
    cfg.model_config_path = "cfg.yaml";
    cfg.graph_hdf5_path = "ignored.h5";
    return cfg;
  };
  steps.loadPlugin = [&](const std::string&,
                         DestroyCompartmentalModelFn& out_destroy,
                         void*& out_handle) -> ICompartmentalModel* {
    out_destroy = [](ICompartmentalModel*) {};
    out_handle = nullptr;
    return &mock_plugin;
  };
  steps.buildPartition =
      [](const std::string&, DomainManager*,
         std::unordered_map<int, int>& venue_map) -> NodePartition {
    NodePartition p;
    p.owned_node_indices = {0, 1};
    p.node_venue_ids = {{10, 20}, {30}};
    venue_map[10] = 0;
    venue_map[20] = 0;
    venue_map[30] = 1;
    return p;
  };

  CompartmentalModelManager mgr("any.yaml", nullptr, std::move(steps));
  REQUIRE(mgr.isActive());
  CHECK(mgr.venueToLocalNodeIndex(10) == 0);
  CHECK(mgr.venueToLocalNodeIndex(20) == 0);
  CHECK(mgr.venueToLocalNodeIndex(30) == 1);
  CHECK(mgr.venueToLocalNodeIndex(99) == -1);
}

TEST_CASE(
    "Step injection: initialise() receives correct partition and config path") {
  InProcessMockPlugin mock_plugin;
  CompartmentalModelSteps steps;
  steps.loadSidecar = [](const std::string&) -> PluginSidecarConfig {
    PluginSidecarConfig cfg;
    cfg.plugin_so_path = "ignored.so";
    cfg.model_config_path = "/model/config.yaml";
    cfg.graph_hdf5_path = "ignored.h5";
    return cfg;
  };
  steps.loadPlugin = [&](const std::string&,
                         DestroyCompartmentalModelFn& out_destroy,
                         void*& out_handle) -> ICompartmentalModel* {
    out_destroy = [](ICompartmentalModel*) {};
    out_handle = nullptr;
    return &mock_plugin;
  };
  steps.buildPartition =
      [](const std::string&, DomainManager*,
         std::unordered_map<int, int>& venue_map) -> NodePartition {
    NodePartition p;
    p.owned_node_indices = {5};
    p.node_venue_ids = {{42}};
    venue_map[42] = 0;
    return p;
  };

  CompartmentalModelManager mgr("any.yaml", nullptr, std::move(steps));
  REQUIRE(mgr.isActive());
  CHECK(mock_plugin.received_config == "/model/config.yaml");
  CHECK(mock_plugin.received_partition.owned_node_indices ==
        std::vector<int>{5});
}

TEST_CASE(
    "Step injection: advance() forwards to in-process plugin (no .so needed)") {
  InProcessMockPlugin mock_plugin;
  CompartmentalModelSteps steps;
  steps.loadSidecar = [](const std::string&) -> PluginSidecarConfig {
    PluginSidecarConfig cfg;
    cfg.plugin_so_path = "ignored.so";
    return cfg;
  };
  steps.loadPlugin = [&](const std::string&,
                         DestroyCompartmentalModelFn& out_destroy,
                         void*& out_handle) -> ICompartmentalModel* {
    out_destroy = [](ICompartmentalModel*) {};
    out_handle = nullptr;
    return &mock_plugin;
  };
  // buildPartition not set → real impl runs against empty path → empty
  // partition.

  CompartmentalModelManager mgr("any.yaml", nullptr, std::move(steps));
  REQUIRE(mgr.isActive());
  mgr.advance(8.0f / 24.0f, 0.0f);
  mgr.advance(6.0f / 24.0f, 8.0f / 24.0f);
  CHECK(mock_plugin.advance_count == 2);
  CHECK(mock_plugin.last_advance_dt ==
        doctest::Approx(6.0f / 24.0f).epsilon(1e-5f));
  CHECK(mock_plugin.last_advance_time ==
        doctest::Approx(8.0f / 24.0f).epsilon(1e-5f));
}

// =============================================================================
// Slice 1: realLoadSidecar parses compartmental_model_output_to_human_foi map
// =============================================================================

TEST_CASE(
    "realLoadSidecar: parses compartmental_model_output_to_human_foi as venue "
    "map") {
  const std::string path = "/tmp/test_output_foi_sidecar.yaml";
  {
    std::ofstream f(path);
    f << "plugin_so_path: \"\"\n";
    f << "model_config_path: \"\"\n";
    f << "graph_hdf5_path: \"\"\n";
    f << "compartmental_model_output_to_human_foi:\n";
    f << "  default:\n";
    f << "    values: [0.01]\n";
    f << "  household:\n";
    f << "    values: [0.02]\n";
    f << "  church:\n";
    f << "    values: [0.005]\n";
    f << "  land:\n";
    f << "    values: [0.015]\n";
  }

  auto cfg = CompartmentalModelManager::realLoadSidecar(path);

  REQUIRE(cfg.output_foi_matrix.matrices.count("household") == 1);
  REQUIRE(cfg.output_foi_matrix.matrices.count("church") == 1);
  REQUIRE(cfg.output_foi_matrix.matrices.count("land") == 1);
  CHECK(cfg.output_foi_matrix.default_value == doctest::Approx(0.01f));
  CHECK(cfg.output_foi_matrix.matrices.at("household").getValue(0) ==
        doctest::Approx(0.02f));
  CHECK(cfg.output_foi_matrix.matrices.at("church").getValue(0) ==
        doctest::Approx(0.005f));
  CHECK(cfg.output_foi_matrix.matrices.at("land").getValue(0) ==
        doctest::Approx(0.015f));

  // After resolve — id-based lookup works correctly
  cfg.output_foi_matrix.resolve({"office", "household", "land", "church"});
  CHECK(cfg.output_foi_matrix.getValue(0, 0) ==
        doctest::Approx(0.01f));  // office → default
  CHECK(cfg.output_foi_matrix.getValue(1, 0) ==
        doctest::Approx(0.02f));  // household
  CHECK(cfg.output_foi_matrix.getValue(2, 0) ==
        doctest::Approx(0.015f));  // land
  CHECK(cfg.output_foi_matrix.getValue(3, 0) ==
        doctest::Approx(0.005f));  // church
  CHECK(cfg.output_foi_matrix.getValue(99, 0) ==
        doctest::Approx(0.01f));  // unknown → default
}

// =============================================================================
// Slice 2: getOutputFOIScale returns correct scale per venue type
// =============================================================================

TEST_CASE(
    "getOutputFOIScale: correct scale per venue type after resolveVenueTypes") {
  InProcessMockPlugin mock_plugin;
  CompartmentalModelSteps steps;
  steps.loadSidecar = [](const std::string&) -> PluginSidecarConfig {
    PluginSidecarConfig cfg;
    cfg.plugin_so_path = "ignored.so";
    cfg.output_foi_matrix.default_value = 0.01f;
    CouplingMatrix hh;
    hh.values = {0.02f};
    cfg.output_foi_matrix.matrices["household"] = hh;
    CouplingMatrix ch;
    ch.values = {0.005f};
    cfg.output_foi_matrix.matrices["church"] = ch;
    return cfg;
  };
  steps.loadPlugin = [&](const std::string&,
                         DestroyCompartmentalModelFn& out_destroy,
                         void*& out_handle) -> ICompartmentalModel* {
    out_destroy = [](ICompartmentalModel*) {};
    out_handle = nullptr;
    return &mock_plugin;
  };

  CompartmentalModelManager mgr("any.yaml", nullptr, std::move(steps));
  REQUIRE(mgr.isActive());

  // Before resolve: all fall through to default_value
  CHECK(mgr.getOutputFOIScale(1, 0) == doctest::Approx(0.01f));

  mgr.resolveVenueTypes({"office", "household", "church"});
  CHECK(mgr.getOutputFOIScale(0, 0) ==
        doctest::Approx(0.01f));  // office → default
  CHECK(mgr.getOutputFOIScale(1, 0) == doctest::Approx(0.02f));   // household
  CHECK(mgr.getOutputFOIScale(2, 0) == doctest::Approx(0.005f));  // church
  CHECK(mgr.getOutputFOIScale(99, 0) ==
        doctest::Approx(0.01f));  // unknown → default
}

// =============================================================================
// Slice 3: readCouplingOutputs returns raw unscaled buffer
// =============================================================================

TEST_CASE(
    "readCouplingOutputs: returns raw buffer — output_foi_matrix not applied") {
  const float raw_output = 3.7f;
  InProcessMockPlugin mock_plugin;
  mock_plugin.coupling_output_value = raw_output;

  CompartmentalModelSteps steps;
  steps.loadSidecar = [](const std::string&) -> PluginSidecarConfig {
    PluginSidecarConfig cfg;
    cfg.plugin_so_path = "ignored.so";
    cfg.output_foi_matrix.default_value =
        0.5f;  // must NOT be applied to buffer
    return cfg;
  };
  steps.loadPlugin = [&](const std::string&,
                         DestroyCompartmentalModelFn& out_destroy,
                         void*& out_handle) -> ICompartmentalModel* {
    out_destroy = [](ICompartmentalModel*) {};
    out_handle = nullptr;
    return &mock_plugin;
  };
  steps.buildPartition = [](const std::string&, DomainManager*,
                            std::unordered_map<int, int>& vm) -> NodePartition {
    NodePartition p;
    p.owned_node_indices = {0};
    p.node_venue_ids = {{42}};
    vm[42] = 0;
    return p;
  };

  CompartmentalModelManager mgr("any.yaml", nullptr, std::move(steps));
  REQUIRE(mgr.isActive());

  mgr.advance(1.0f / 24.0f, 0.0f);  // invalidate cache
  const float* buf = mgr.readCouplingOutputs();
  REQUIRE(buf != nullptr);
  CHECK(buf[0] == doctest::Approx(raw_output));  // raw — not multiplied by 0.5
}

// =============================================================================
// Interface constants
// =============================================================================

TEST_CASE("ICompartmentalModel interface version constant is positive") {
  CHECK(COMPARTMENTAL_MODEL_INTERFACE_VERSION > 0);
}

TEST_CASE("NodePartition default-constructs empty") {
  NodePartition p;
  CHECK(p.owned_node_indices.empty());
  CHECK(p.node_venue_ids.empty());
}
