#pragma once

#include <string>
#include <vector>

namespace june {

// Increment whenever the ICompartmentalModel ABI changes. Plugin .so files
// must export a symbol of this value so the manager can detect mismatches.
constexpr int COMPARTMENTAL_MODEL_INTERFACE_VERSION = 1;

// Spatial partition data passed to the plugin at initialise(). Index i
// corresponds to the i-th locally-owned compartmental node.
struct NodePartition {
  std::vector<int> owned_node_indices;  // global node IDs on this rank
  std::vector<std::vector<int>>
      node_venue_ids;  // june venue IDs per owned node
};

// Abstract interface every compartmental model plugin must implement.
// The plugin .so exports extern "C" factory functions (see symbol constants
// below) so the manager can construct/destroy instances without knowing the
// concrete type.
class ICompartmentalModel {
 public:
  virtual ~ICompartmentalModel() = default;

  // Called once after MPI domain decomposition. partition describes which
  // compartmental nodes this rank owns and which june venues they contain.
  // config_path is the plugin's own YAML configuration file.
  virtual void initialise(const NodePartition& partition,
                          const std::string& config_path) = 0;

  // Advance ODE state by dt_days, starting at current_time_days (days since
  // simulation start). Called once per june TimeSlot before human transmission.
  virtual void advance(float dt_days, float current_time_days) = 0;

  // Write one float per owned node into out[0..n_owned_nodes). Semantics are
  // model-defined (e.g. infectious free flea density). Called by june
  // immediately after advance() to drive human transmission.
  virtual void getCouplingOutputs(float* out, int n_owned_nodes) const = 0;

  // Receive one float per owned node from june (e.g. total currently-infected
  // humans aggregated across venues in each node). Called after june's human
  // transmission step each TimeSlot.
  virtual void setCouplingInputs(const float* in, int n_owned_nodes) = 0;

  // Called at the end of each TimeSlot. Plugin decides whether to flush state
  // snapshots to disk based on its own configured interval.
  virtual void maybeSnapshot(float current_time_days) = 0;

  // Number of compartmental nodes owned by this MPI rank.
  virtual int ownedNodeCount() const = 0;
};

// Function pointer types resolved via dlsym when loading a plugin .so.
using CreateCompartmentalModelFn = ICompartmentalModel* (*)();
using DestroyCompartmentalModelFn = void (*)(ICompartmentalModel*);

// Symbol names the plugin .so must export.
constexpr const char* COMPARTMENTAL_MODEL_CREATE_SYMBOL =
    "create_compartmental_model";
constexpr const char* COMPARTMENTAL_MODEL_DESTROY_SYMBOL =
    "destroy_compartmental_model";
// Must point to an int equal to COMPARTMENTAL_MODEL_INTERFACE_VERSION.
constexpr const char* COMPARTMENTAL_MODEL_VERSION_SYMBOL =
    "compartmental_model_interface_version";

}  // namespace june
