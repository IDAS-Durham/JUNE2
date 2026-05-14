#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <fstream>
#include <string>

#include "doctest.h"
#include "simulation/compartmental_model_manager.h"

using namespace june;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string writeCouplingMatrixYaml() {
  const char* path = "/tmp/test_coupling_matrix.yaml";
  std::ofstream f(path);
  f << R"(
default: 0.001
contact_matrices:
  school:
    bins: [teacher, student]
    values: [0.005, 0.002]
  office:
    bins: [worker]
    values: [0.003]
)";
  return path;
}

static std::string writeSidecarWithMatrix(const std::string& matrix_path) {
  const char* path = "/tmp/test_coupling_sidecar_matrix.yaml";
  std::ofstream f(path);
  f << "plugin_so_path: \"/nonexistent/libplugin.so\"\n";
  f << "model_config_path: \"\"\n";
  f << "graph_hdf5_path: \"\"\n";
  f << "coupling_matrix_file: \"" << matrix_path << "\"\n";
  f << "coupling:\n  beta: 0.0042\n";
  return path;
}

static std::string writeSidecarNoMatrix() {
  const char* path = "/tmp/test_coupling_sidecar_no_matrix.yaml";
  std::ofstream f(path);
  f << "plugin_so_path: \"/nonexistent/libplugin.so\"\n";
  f << "model_config_path: \"\"\n";
  f << "graph_hdf5_path: \"\"\n";
  f << "coupling:\n  beta: 0.0042\n";
  return path;
}

// ---------------------------------------------------------------------------
// CouplingMatrix struct tests
// ---------------------------------------------------------------------------

TEST_CASE("CouplingMatrix default-constructs") {
  CouplingMatrix cm;
  CHECK(cm.default_value == doctest::Approx(0.001f));
  CHECK(cm.bins.empty());
  CHECK(cm.values.empty());
}

TEST_CASE("CouplingMatrix getValue falls back to default when out of range") {
  CouplingMatrix cm;
  cm.default_value = 0.005f;
  cm.bins = {"worker"};
  cm.values = {0.003f};

  CHECK(cm.getValue(0) == doctest::Approx(0.003f));
  CHECK(cm.getValue(1) == doctest::Approx(0.005f));   // out of range → default
  CHECK(cm.getValue(-1) == doctest::Approx(0.005f));  // negative → default
}

// ---------------------------------------------------------------------------
// CouplingMatrixConfig tests
// ---------------------------------------------------------------------------

TEST_CASE("CouplingMatrixConfig default-constructs with scalar fallback") {
  CouplingMatrixConfig cfg;
  cfg.default_value = 0.001f;
  CHECK(cfg.getDefault() == doctest::Approx(0.001f));
  CHECK(cfg.matrices.empty());
}

// ---------------------------------------------------------------------------
// YAML loading tests
// ---------------------------------------------------------------------------

TEST_CASE("loadCouplingMatrix: per-venue per-bin values parsed correctly") {
  CouplingMatrixConfig cfg = loadCouplingMatrix(writeCouplingMatrixYaml());
  CHECK(cfg.default_value == doctest::Approx(0.001f));

  REQUIRE(cfg.matrices.count("school") == 1);
  const CouplingMatrix& school = cfg.matrices.at("school");
  REQUIRE(school.bins.size() == 2);
  CHECK(school.bins[0] == "teacher");
  CHECK(school.bins[1] == "student");
  REQUIRE(school.values.size() == 2);
  CHECK(school.values[0] == doctest::Approx(0.005f));
  CHECK(school.values[1] == doctest::Approx(0.002f));

  REQUIRE(cfg.matrices.count("office") == 1);
  const CouplingMatrix& office = cfg.matrices.at("office");
  REQUIRE(office.bins.size() == 1);
  CHECK(office.values[0] == doctest::Approx(0.003f));
}

TEST_CASE("loadCouplingMatrix: unknown venue falls back to default") {
  CouplingMatrixConfig cfg = loadCouplingMatrix(writeCouplingMatrixYaml());
  CHECK(cfg.matrices.count("hospital") == 0);
  CHECK(cfg.getDefault() == doctest::Approx(0.001f));
}

// ---------------------------------------------------------------------------
// Sidecar parsing: coupling_matrix_file field
// ---------------------------------------------------------------------------

TEST_CASE("realLoadSidecar: coupling_matrix_file parsed from sidecar") {
  std::string matrix_yaml = writeCouplingMatrixYaml();
  std::string sidecar = writeSidecarWithMatrix(matrix_yaml);

  PluginSidecarConfig cfg = CompartmentalModelManager::realLoadSidecar(sidecar);
  CHECK(cfg.coupling_matrix_file == matrix_yaml);
}

TEST_CASE("realLoadSidecar: coupling_matrix_file absent → empty string") {
  std::string sidecar = writeSidecarNoMatrix();
  PluginSidecarConfig cfg = CompartmentalModelManager::realLoadSidecar(sidecar);
  CHECK(cfg.coupling_matrix_file.empty());
}

TEST_CASE(
    "realLoadSidecar: default_human_to_compartmental_model_input defaults to "
    "0.001 when coupling.beta absent") {
  std::string sidecar = writeSidecarNoMatrix();
  PluginSidecarConfig cfg = CompartmentalModelManager::realLoadSidecar(sidecar);
  CHECK(cfg.default_human_to_compartmental_model_input ==
        doctest::Approx(0.001f));
}

// ---------------------------------------------------------------------------
// Manager: coupling matrix accessible via getCouplingMatrix()
// ---------------------------------------------------------------------------

TEST_CASE("Manager with no matrix file uses coupling.beta as default") {
  CompartmentalModelSteps steps;
  steps.loadSidecar = [](const std::string&) -> PluginSidecarConfig {
    PluginSidecarConfig cfg;
    cfg.plugin_so_path = "ignored.so";
    cfg.default_human_to_compartmental_model_input = 0.007f;
    cfg.coupling_matrix_file = "";
    return cfg;
  };
  steps.loadPlugin = [](const std::string&, DestroyCompartmentalModelFn&,
                        void*&) -> ICompartmentalModel* {
    return nullptr;  // inactive manager
  };

  CompartmentalModelManager mgr("any.yaml", nullptr, std::move(steps));
  CHECK(mgr.isActive() == false);
  const CouplingMatrixConfig& cm = mgr.getCouplingMatrix();
  CHECK(cm.getDefault() == doctest::Approx(0.007f));
  CHECK(cm.matrices.empty());
}

TEST_CASE("Manager with matrix file loads per-venue-type values") {
  std::string matrix_yaml = writeCouplingMatrixYaml();
  CompartmentalModelSteps steps;
  steps.loadSidecar = [&](const std::string&) -> PluginSidecarConfig {
    PluginSidecarConfig cfg;
    cfg.plugin_so_path = "ignored.so";
    cfg.default_human_to_compartmental_model_input = 0.001f;
    cfg.coupling_matrix_file = matrix_yaml;
    return cfg;
  };
  steps.loadPlugin = [](const std::string&, DestroyCompartmentalModelFn&,
                        void*&) -> ICompartmentalModel* {
    return nullptr;  // we only care about the matrix, not the plugin
  };

  CompartmentalModelManager mgr("any.yaml", nullptr, std::move(steps));
  const CouplingMatrixConfig& cm = mgr.getCouplingMatrix();
  CHECK(cm.matrices.count("school") == 1);
  CHECK(cm.matrices.at("school").values[0] == doctest::Approx(0.005f));
  CHECK(cm.matrices.at("school").values[1] == doctest::Approx(0.002f));
  CHECK(cm.matrices.count("office") == 1);
  CHECK(cm.matrices.at("office").values[0] == doctest::Approx(0.003f));
}
