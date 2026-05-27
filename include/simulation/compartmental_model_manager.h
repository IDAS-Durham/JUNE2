#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/types.h"
#include "epidemiology/disease.h"
#include "simulation/compartmental_model_plugin.h"

namespace june {

class DomainManager;
class WorldState;

// ---------------------------------------------------------------------------
// Coupling matrix: per-venue-type, per-person-bin scalar weights.
// Mirrors ContactMatrix bin resolution (age_to_bin, bin_by_subset_type, etc.).
// ---------------------------------------------------------------------------

struct CouplingMatrix {
  float default_value = 0.001f;
  std::vector<std::string> bins;
  std::vector<float> values;

  // Pre-resolved bin lookup arrays (filled by CouplingMatrixConfig::resolve)
  int age_to_bin[100] = {};
  bool has_age_bins = false;
  std::vector<int> bin_by_subset_type;
  int male_bin = -1;
  int female_bin = -1;

  // Returns values[bin_idx] if in range, else default_value.
  float getValue(int bin_idx) const {
    if (bin_idx >= 0 && bin_idx < static_cast<int>(values.size()))
      return values[static_cast<size_t>(bin_idx)];
    return default_value;
  }
};

struct CouplingMatrixConfig {
  float default_value = 0.001f;
  std::unordered_map<std::string, CouplingMatrix> matrices;

  // Pre-resolved: [venue_type_id] → CouplingMatrix* (nullptr = use default)
  std::vector<const CouplingMatrix*> matrices_by_id;

  float getDefault() const { return default_value; }

  // Hot-path lookup: returns value for venue_type_id + bin_idx.
  // Falls back to default_value when venue type not in matrix or bin out of
  // range.
  float getValue(int venue_type_id, int bin_idx) const {
    if (venue_type_id >= 0 &&
        venue_type_id < static_cast<int>(matrices_by_id.size())) {
      const CouplingMatrix* m =
          matrices_by_id[static_cast<size_t>(venue_type_id)];
      if (m) return m->getValue(bin_idx);
    }
    return default_value;
  }

  // Resolves string-keyed matrices to integer-indexed arrays.
  // Call after WorldState venue_type_names are available.
  void resolve(const std::vector<std::string>& venue_type_names);
};

// Load a coupling matrix YAML file. Returns default-only config on failure.
CouplingMatrixConfig loadCouplingMatrix(const std::string& yaml_path);

// ---------------------------------------------------------------------------
// Sidecar YAML fields. Promoted to header so tests can construct directly.
// ---------------------------------------------------------------------------

struct PluginSidecarConfig {
  std::string plugin_so_path;
  std::string model_config_path;
  std::string graph_hdf5_path;
  // Exactly one of coupling_matrix_file or human_to_compartmental_model_input
  // must be set. coupling_matrix_file: path to a full per-venue-type per-bin
  // YAML matrix. human_to_compartmental_model_input: map of venue_type_name →
  // rate at which one
  //   unit of human deposition drives the compartmental model input per venue
  //   type. Simple case: one scalar per venue type, no per-bin breakdown.
  // If neither is set, default_human_to_compartmental_model_input is used as a
  // global fallback.
  std::string coupling_matrix_file;
  std::unordered_map<std::string, float> human_to_compartmental_model_input;
  float default_human_to_compartmental_model_input = 0.001f;

  // Per-venue-type FOI scaling applied at the interaction site (not to the
  // buffer). bins follow the same definition as ContactMatrices and Fomite
  // infections, as defined in the contact matrices .yaml file; currently
  // bin_idx=0. Default (1.0) = no scaling, matching the old global scalar
  // default.
  CouplingMatrixConfig output_foi_matrix = []() noexcept {
    CouplingMatrixConfig c;
    c.default_value = 1.0f;
    return c;
  }();
};

// ---------------------------------------------------------------------------
// Step-injection struct for unit tests.
// Any nullptr field falls back to the real implementation.
// ---------------------------------------------------------------------------

struct CompartmentalModelSteps {
  std::function<PluginSidecarConfig(const std::string& sidecar_path)>
      loadSidecar;

  std::function<ICompartmentalModel*(
      const std::string& so_path, DestroyCompartmentalModelFn& out_destroy_fn,
      void*& out_dl_handle)>
      loadPlugin;

  std::function<NodePartition(
      const std::string& graph_hdf5_path, DomainManager* domain_mgr,
      std::unordered_map<int, int>& out_venue_to_local_node)>
      buildPartition;

  std::function<void(ICompartmentalModel* plugin,
                     const NodePartition& partition,
                     const std::string& config_path)>
      initialise;
};

// ---------------------------------------------------------------------------
// Owns the plugin .so handle and the ICompartmentalModel instance.
// When no plugin is configured (sidecar_path empty at construction), all
// methods are no-ops: no virtual dispatch or heap activity on the hot path.
// ---------------------------------------------------------------------------

class CompartmentalModelManager {
 public:
  explicit CompartmentalModelManager(const std::string& sidecar_path,
                                     DomainManager* domain_mgr = nullptr);

  CompartmentalModelManager(const std::string& sidecar_path,
                            DomainManager* domain_mgr,
                            CompartmentalModelSteps steps);

  ~CompartmentalModelManager();

  CompartmentalModelManager(const CompartmentalModelManager&) = delete;
  CompartmentalModelManager& operator=(const CompartmentalModelManager&) =
      delete;

  bool isActive() const { return plugin_ != nullptr; }
  int ownedNodeCount() const;

  void advance(float dt_days, float current_time_days);
  const float* readCouplingOutputs() const;
  void writeCouplingInputs(const float* infected_per_node, int n_nodes);
  void maybeSnapshot(float current_time_days);
  int venueToLocalNodeIndex(int venue_id) const;

  const CouplingMatrixConfig& getCouplingMatrix() const {
    return coupling_matrix_;
  }
  const std::unordered_map<VenueId, int>& getVenueToNodeMap() const {
    return venue_to_local_node_;
  }

  float getOutputFOIScale(int venue_type_id, int bin_idx = 0) const;
  // Resolves both coupling_matrix_ and output_foi_matrix_ to id-based lookup
  // tables. Call once after WorldState venue_type_names are known.
  void resolveVenueTypes(const std::vector<std::string>& venue_type_names);

  // Aggregate per-person deposition contributions over owned compartmental
  // nodes and forward to the plugin via writeCouplingInputs(). No-op when
  // inactive or when the disease has no CompartmentalDeposition modes.
  void computeDepositionWriteback(const std::vector<PersonLocation>& locations,
                                  WorldState& world, const Disease& disease,
                                  double t0, double t1);

  static PluginSidecarConfig realLoadSidecar(const std::string& sidecar_path);

 private:
  void* dl_handle_ = nullptr;
  DestroyCompartmentalModelFn destroy_fn_ = nullptr;
  ICompartmentalModel* plugin_ = nullptr;

  mutable std::vector<float> coupling_output_buffer_;
  mutable bool coupling_output_valid_ = false;
  std::unordered_map<int, int> venue_to_local_node_;
  CouplingMatrixConfig coupling_matrix_;
  CouplingMatrixConfig output_foi_matrix_;

  void init(const std::string& sidecar_path, DomainManager* domain_mgr,
            CompartmentalModelSteps steps);

  static ICompartmentalModel* realLoadPlugin(
      const std::string& so_path, DestroyCompartmentalModelFn& out_destroy_fn,
      void*& out_dl_handle);
  static NodePartition realBuildPartition(
      const std::string& graph_hdf5_path, DomainManager* domain_mgr,
      std::unordered_map<int, int>& out_venue_to_local_node);
  static void realInitialise(ICompartmentalModel* plugin,
                             const NodePartition& partition,
                             const std::string& config_path);
};

}  // namespace june
