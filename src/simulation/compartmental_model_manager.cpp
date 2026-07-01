#include "simulation/compartmental_model_manager.h"

#include <dlfcn.h>
#include <hdf5.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "core/world_state.h"

#ifdef USE_MPI
#include "parallel/domain_manager.h"
#endif

namespace june {

// HDF5 dataset paths for the spatial graph (compartmental node → venue
// mapping).
namespace CompartmentalGraphSchema {
constexpr const char* VENUE_ID_PTR = "/nodes/venue_id_ptr";
constexpr const char* VENUE_ID_DATA = "/nodes/venue_id_data";
}  // namespace CompartmentalGraphSchema

namespace {

// Case-insensitive "does haystack contain needle".
bool containsNoCase(const std::string& haystack, const std::string& needle) {
  auto lower = [](unsigned char c) { return std::tolower(c); };
  auto it = std::search(haystack.begin(), haystack.end(), needle.begin(),
                        needle.end(),
                        [&](char a, char b) { return lower(a) == lower(b); });
  return it != haystack.end();
}

}  // namespace

// =============================================================================
// loadCouplingMatrix: free function
// =============================================================================

CouplingMatrixConfig loadCouplingMatrix(const std::string& yaml_path) {
  CouplingMatrixConfig cfg;
  if (yaml_path.empty()) return cfg;

  try {
    YAML::Node root = YAML::LoadFile(yaml_path);
    if (root["default"]) cfg.default_value = root["default"].as<float>();

    if (root["contact_matrices"]) {
      for (auto it = root["contact_matrices"].begin();
           it != root["contact_matrices"].end(); ++it) {
        std::string venue_type = it->first.as<std::string>();
        // yaml-cpp Nodes are reference-counted handles; value-copy
        // (not const-ref) is required to keep the underlying storage
        // alive across subsequent operator[] / iterator operations.
        YAML::Node entry = it->second;

        CouplingMatrix cm;
        cm.default_value = cfg.default_value;
        if (entry["bins"]) {
          for (const auto& b : entry["bins"])
            cm.bins.push_back(b.as<std::string>());
        }
        if (entry["values"]) {
          for (const auto& v : entry["values"])
            cm.values.push_back(v.as<float>());
        }
        cfg.matrices[venue_type] = std::move(cm);
      }
    }
  } catch (const std::exception& e) {
    std::cerr << "[loadCouplingMatrix] Failed to load '" << yaml_path
              << "': " << e.what() << '\n';
  }
  return cfg;
}

// =============================================================================
// CouplingMatrixConfig::resolve
// =============================================================================

void CouplingMatrixConfig::resolve(
    const std::vector<std::string>& venue_type_names) {
  matrices_by_id.assign(venue_type_names.size(), nullptr);
  for (size_t i = 0; i < venue_type_names.size(); ++i) {
    auto it = matrices.find(venue_type_names[i]);
    if (it != matrices.end()) matrices_by_id[i] = &it->second;
  }
}

// =============================================================================
// Constructors
// =============================================================================

CompartmentalModelManager::CompartmentalModelManager(
    const std::string& sidecar_path, DomainManager* domain_mgr) {
  init(sidecar_path, domain_mgr, {});
}

CompartmentalModelManager::CompartmentalModelManager(
    const std::string& sidecar_path, DomainManager* domain_mgr,
    CompartmentalModelSteps steps) {
  init(sidecar_path, domain_mgr, std::move(steps));
}

// =============================================================================
// init(): shared construction body; each step dispatches to the injected
// function if set, otherwise falls back to the real implementation.
// =============================================================================

void CompartmentalModelManager::init(const std::string& sidecar_path,
                                     DomainManager* domain_mgr,
                                     CompartmentalModelSteps steps) {
  if (sidecar_path.empty()) return;

  // Step 1: parse sidecar YAML.
  PluginSidecarConfig cfg;
  try {
    cfg = steps.loadSidecar ? steps.loadSidecar(sidecar_path)
                            : realLoadSidecar(sidecar_path);
  } catch (const std::exception& e) {
    std::cerr << "[CompartmentalModelManager] Failed to parse sidecar '"
              << sidecar_path << "': " << e.what() << '\n';
    return;
  }

  if (cfg.plugin_so_path.empty()) {
    std::cerr
        << "[CompartmentalModelManager] plugin_so_path not set in sidecar.\n";
    return;
  }

  // Step 1b: load coupling matrix.
  // coupling_matrix_file: full per-venue-type per-bin YAML matrix.
  // human_to_compartmental_model_input: simple per-venue-type scalar map.
  // resolve() is called by Simulator after WorldState is built.
  if (!cfg.coupling_matrix_file.empty()) {
    coupling_matrix_ = loadCouplingMatrix(cfg.coupling_matrix_file);
  } else if (!cfg.human_to_compartmental_model_input.empty()) {
    for (const auto& [vtype, rate] : cfg.human_to_compartmental_model_input) {
      CouplingMatrix mat;
      mat.default_value = rate;
      coupling_matrix_.matrices[vtype] = mat;
    }
  } else {
    coupling_matrix_.default_value =
        cfg.default_human_to_compartmental_model_input;
  }
  output_foi_matrix_ = cfg.output_foi_matrix;

  // Step 2: open .so, verify version, call factory.
  plugin_ = steps.loadPlugin
                ? steps.loadPlugin(cfg.plugin_so_path, destroy_fn_, dl_handle_)
                : realLoadPlugin(cfg.plugin_so_path, destroy_fn_, dl_handle_);
  if (!plugin_) return;

  // Step 3: read graph HDF5 and build MPI partition.
  NodePartition partition =
      steps.buildPartition
          ? steps.buildPartition(cfg.graph_hdf5_path, domain_mgr,
                                 venue_to_local_node_)
          : realBuildPartition(cfg.graph_hdf5_path, domain_mgr,
                               venue_to_local_node_);

  // Step 4: call plugin->initialise().
  if (steps.initialise)
    steps.initialise(plugin_, partition, cfg.model_config_path);
  else
    realInitialise(plugin_, partition, cfg.model_config_path);

  coupling_output_buffer_.assign(static_cast<size_t>(plugin_->ownedNodeCount()),
                                 0.0f);
}

// =============================================================================
// Destructor
// =============================================================================

CompartmentalModelManager::~CompartmentalModelManager() {
  if (plugin_ && destroy_fn_) {
    destroy_fn_(plugin_);
    plugin_ = nullptr;
  }
  if (dl_handle_) {
    dlclose(dl_handle_);
    dl_handle_ = nullptr;
  }
}

// =============================================================================
// Real step implementations (production path)
// =============================================================================

PluginSidecarConfig CompartmentalModelManager::realLoadSidecar(
    const std::string& path) {
  PluginSidecarConfig cfg;
  YAML::Node node = YAML::LoadFile(path);
  cfg.plugin_so_path =
      node["plugin_so_path"] ? node["plugin_so_path"].as<std::string>() : "";
  cfg.model_config_path = node["model_config_path"]
                              ? node["model_config_path"].as<std::string>()
                              : "";
  cfg.graph_hdf5_path =
      node["graph_hdf5_path"] ? node["graph_hdf5_path"].as<std::string>() : "";

  bool has_matrix_file =
      node["coupling_matrix_file"] && node["coupling_matrix_file"].IsScalar();
  bool has_rates = node["human_to_compartmental_model_input"] &&
                   node["human_to_compartmental_model_input"].IsMap();
  if (has_matrix_file && has_rates) {
    std::cerr << "[CompartmentalModelManager] Sidecar '" << path
              << "' has both coupling_matrix_file and "
                 "human_to_compartmental_model_input — "
                 "they are mutually exclusive. Using coupling_matrix_file.\n";
    has_rates = false;
  }
  if (has_matrix_file) {
    cfg.coupling_matrix_file = node["coupling_matrix_file"].as<std::string>();
  } else if (has_rates) {
    for (const auto& kv : node["human_to_compartmental_model_input"]) {
      cfg.human_to_compartmental_model_input[kv.first.as<std::string>()] =
          kv.second.as<float>();
    }
  }
  if (node["default_human_to_compartmental_model_input"])
    cfg.default_human_to_compartmental_model_input =
        node["default_human_to_compartmental_model_input"].as<float>();
  if (node["compartmental_model_output_to_human_foi"]) {
    // yaml-cpp Nodes are reference-counted handles; value-copy (not
    // const-ref) is required to keep the underlying storage alive
    // across iterator and operator[] operations.
    YAML::Node foi = node["compartmental_model_output_to_human_foi"];
    if (foi.IsMap()) {
      for (auto it = foi.begin(); it != foi.end(); ++it) {
        const std::string key = it->first.as<std::string>();
        YAML::Node entry = it->second;
        if (key == "default") {
          if (entry["values"] && entry["values"].IsSequence() &&
              entry["values"].size() > 0)
            cfg.output_foi_matrix.default_value =
                entry["values"][0].as<float>();
        } else {
          CouplingMatrix cm;
          cm.default_value = cfg.output_foi_matrix.default_value;
          if (entry["values"])
            for (const auto& v : entry["values"])
              cm.values.push_back(v.as<float>());
          cfg.output_foi_matrix.matrices[key] = std::move(cm);
        }
      }
    }
  }
  return cfg;
}

bool isHdf5DuplicateConstantError(const std::string& func_name,
                                  const std::string& detail_msg) {
  // The HDF5 C++ library throws this from DataSpace::getConstant() when its
  // global ALL_ constant is initialised a second time — i.e. a second
  // libhdf5_cpp has been loaded into the process. Match either token so the
  // probe survives wording changes across HDF5 versions.
  return containsNoCase(func_name, "getConstant") ||
         containsNoCase(detail_msg, "ALL_");
}

std::string formatHdf5AbiMismatchMessage(const std::string& plugin_path,
                                         const std::string& detail_msg) {
  std::ostringstream os;
  os << '\n'
     << "+--------------------------------------------------------------------"
        "--------+\n"
     << "|  FATAL: HDF5 C++ library (libhdf5_cpp) ABI clash                    "
        "        |\n"
     << "+--------------------------------------------------------------------"
        "--------+\n"
     << "The compartmental-model plugin was linked against a DIFFERENT "
        "libhdf5_cpp\n"
     << "than this disease_sim binary. Loading it pulled a second HDF5 C++ "
        "library\n"
     << "into the process, so HDF5's global DataSpace::ALL constant was "
        "initialised\n"
     << "twice and the loader aborted with:\n"
     << "    " << detail_msg << '\n'
     << '\n'
     << "Plugin: " << plugin_path << '\n'
     << '\n'
     << "Fix: rebuild the plugin against the SAME HDF5 (same conda env / "
        "toolchain)\n"
     << "as disease_sim, then confirm the sonames match:\n"
     << "    ldd " << plugin_path << " | grep libhdf5_cpp\n"
     << "    ldd <path-to>/disease_sim   | grep libhdf5_cpp\n"
     << "Both must report the same libhdf5_cpp.so.<N>.\n"
     << "+--------------------------------------------------------------------"
        "--------+\n";
  return os.str();
}

ICompartmentalModel* CompartmentalModelManager::realLoadPlugin(
    const std::string& so_path, DestroyCompartmentalModelFn& out_destroy_fn,
    void*& out_dl_handle) {
  dlerror();
  // NB: if the plugin links a different libhdf5_cpp than the host, dlopen runs
  // that second library's global constructors, HDF5's DataSpace::ALL constant
  // is initialised twice, and an H5::Exception is thrown from inside ld.so's
  // call_init. That exception cannot be caught at this frame (a handler here
  // is bypassed, or worse turns clean propagation into std::terminate) — it is
  // detected instead in main's top-level H5::Exception handler, which is the
  // proven-reliable seam. See isHdf5DuplicateConstantError / main.cpp.
  out_dl_handle = dlopen(so_path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!out_dl_handle) {
    std::cerr << "[CompartmentalModelManager] dlopen failed for '" << so_path
              << "': " << dlerror() << '\n';
    return nullptr;
  }

  dlerror();
  auto* version_ptr = reinterpret_cast<int*>(
      dlsym(out_dl_handle, COMPARTMENTAL_MODEL_VERSION_SYMBOL));
  if (!version_ptr) {
    std::cerr << "[CompartmentalModelManager] Plugin missing version symbol '"
              << COMPARTMENTAL_MODEL_VERSION_SYMBOL << "': " << dlerror()
              << '\n';
    dlclose(out_dl_handle);
    out_dl_handle = nullptr;
    return nullptr;
  }
  if (*version_ptr != COMPARTMENTAL_MODEL_INTERFACE_VERSION) {
    std::cerr << "[CompartmentalModelManager] Version mismatch: plugin="
              << *version_ptr
              << " june=" << COMPARTMENTAL_MODEL_INTERFACE_VERSION << '\n';
    dlclose(out_dl_handle);
    out_dl_handle = nullptr;
    return nullptr;
  }

  dlerror();
  auto create_fn = reinterpret_cast<CreateCompartmentalModelFn>(
      dlsym(out_dl_handle, COMPARTMENTAL_MODEL_CREATE_SYMBOL));
  if (!create_fn) {
    std::cerr << "[CompartmentalModelManager] Plugin missing '"
              << COMPARTMENTAL_MODEL_CREATE_SYMBOL << "': " << dlerror()
              << '\n';
    dlclose(out_dl_handle);
    out_dl_handle = nullptr;
    return nullptr;
  }

  dlerror();
  out_destroy_fn = reinterpret_cast<DestroyCompartmentalModelFn>(
      dlsym(out_dl_handle, COMPARTMENTAL_MODEL_DESTROY_SYMBOL));
  if (!out_destroy_fn) {
    std::cerr << "[CompartmentalModelManager] Plugin missing '"
              << COMPARTMENTAL_MODEL_DESTROY_SYMBOL << "': " << dlerror()
              << '\n';
    dlclose(out_dl_handle);
    out_dl_handle = nullptr;
    return nullptr;
  }

  ICompartmentalModel* plugin = create_fn();
  if (!plugin) {
    std::cerr << "[CompartmentalModelManager] create_compartmental_model() "
                 "returned null.\n";
    dlclose(out_dl_handle);
    out_dl_handle = nullptr;
    out_destroy_fn = nullptr;
  }
  return plugin;
}

NodePartition CompartmentalModelManager::realBuildPartition(
    const std::string& graph_hdf5_path, DomainManager* domain_mgr,
    std::unordered_map<int, int>& out_venue_to_local_node) {
  NodePartition partition;
  if (graph_hdf5_path.empty()) return partition;

  hid_t file_id = H5Fopen(graph_hdf5_path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
  if (file_id < 0) {
    std::cerr << "[CompartmentalModelManager] Cannot open graph HDF5: "
              << graph_hdf5_path << '\n';
    return partition;
  }

  auto readInt32Dataset = [&](const char* name) -> std::vector<int32_t> {
    hid_t ds = H5Dopen2(file_id, name, H5P_DEFAULT);
    if (ds < 0) {
      std::cerr << "[CompartmentalModelManager] Dataset not found: " << name
                << '\n';
      return {};
    }
    hid_t space = H5Dget_space(ds);
    hsize_t dims[1] = {0};
    H5Sget_simple_extent_dims(space, dims, nullptr);
    std::vector<int32_t> data(dims[0]);
    H5Dread(ds, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT, data.data());
    H5Sclose(space);
    H5Dclose(ds);
    return data;
  };

  namespace Schema = CompartmentalGraphSchema;
  auto venue_id_ptr = readInt32Dataset(Schema::VENUE_ID_PTR);
  auto venue_id_data = readInt32Dataset(Schema::VENUE_ID_DATA);
  H5Fclose(file_id);

  if (venue_id_ptr.empty()) return partition;

  const int n_nodes = static_cast<int>(venue_id_ptr.size()) - 1;

  int my_rank = 0;
#ifdef USE_MPI
  if (domain_mgr) my_rank = domain_mgr->getRank();
#endif

  for (int node = 0; node < n_nodes; ++node) {
    const int start = venue_id_ptr[node];
    const int end = venue_id_ptr[node + 1];

    std::vector<int> venues(venue_id_data.begin() + start,
                            venue_id_data.begin() + end);

    int owner_rank = 0;
#ifdef USE_MPI
    if (domain_mgr && !venues.empty()) {
      int r = domain_mgr->getVenueRank(venues[0]);
      if (r >= 0) owner_rank = r;
    }
#else
    (void)domain_mgr;
#endif

    if (owner_rank != my_rank) continue;

    const int local_idx = static_cast<int>(partition.owned_node_indices.size());
    partition.owned_node_indices.push_back(node);
    partition.node_venue_ids.push_back(venues);
    for (int vid : venues) out_venue_to_local_node[vid] = local_idx;
  }

  return partition;
}

void CompartmentalModelManager::realInitialise(ICompartmentalModel* plugin,
                                               const NodePartition& partition,
                                               const std::string& config_path) {
  plugin->initialise(partition, config_path);
}

// =============================================================================
// Public methods (hot-path, unchanged)
// =============================================================================

int CompartmentalModelManager::ownedNodeCount() const {
  return plugin_ ? plugin_->ownedNodeCount() : 0;
}

void CompartmentalModelManager::advance(float dt_days,
                                        float current_time_days) {
  if (plugin_) {
    plugin_->advance(dt_days, current_time_days);
    coupling_output_valid_ = false;
  }
}

const float* CompartmentalModelManager::readCouplingOutputs() const {
  if (!plugin_) return nullptr;
  if (!coupling_output_valid_) {
    plugin_->getCouplingOutputs(
        coupling_output_buffer_.data(),
        static_cast<int>(coupling_output_buffer_.size()));
    coupling_output_valid_ = true;
  }
  return coupling_output_buffer_.data();
}

void CompartmentalModelManager::writeCouplingInputs(
    const float* infected_per_node, int n_nodes) {
  if (plugin_) plugin_->setCouplingInputs(infected_per_node, n_nodes);
}

void CompartmentalModelManager::maybeSnapshot(float current_time_days) {
  if (plugin_) plugin_->maybeSnapshot(current_time_days);
}

int CompartmentalModelManager::venueToLocalNodeIndex(int venue_id) const {
  auto it = venue_to_local_node_.find(venue_id);
  return it != venue_to_local_node_.end() ? it->second : -1;
}

float CompartmentalModelManager::getOutputFOIScale(int venue_type_id,
                                                   int bin_idx) const {
  return output_foi_matrix_.getValue(venue_type_id, bin_idx);
}

void CompartmentalModelManager::resolveVenueTypes(
    const std::vector<std::string>& venue_type_names) {
  coupling_matrix_.resolve(venue_type_names);
  output_foi_matrix_.resolve(venue_type_names);
}

// =============================================================================
// CompartmentalModelManager::computeDepositionWriteback
// =============================================================================

void CompartmentalModelManager::computeDepositionWriteback(
    const std::vector<PersonLocation>& locations, WorldState& world,
    const Disease& disease, double t0, double t1) {
  if (!isActive()) return;

  const auto& tp = disease.getTransmissionParams();

  // Check for any CompartmentalDeposition modes.
  bool has_deposition_mode = false;
  for (const auto& tmode : tp.modes) {
    if (tmode.type == TransmissionModeType::CompartmentalDeposition) {
      has_deposition_mode = true;
      break;
    }
  }

  int n_nodes = ownedNodeCount();
  if (!has_deposition_mode) {
    // No deposition modes; write zeros so plugin gets a consistent call.
    if (n_nodes > 0) {
      std::vector<float> zeros(static_cast<size_t>(n_nodes), 0.0f);
      writeCouplingInputs(zeros.data(), n_nodes);
    }
    return;
  }

  std::vector<float> deposition(static_cast<size_t>(n_nodes), 0.0f);

  for (const auto& loc : locations) {
    if (loc.venue_id < 0) continue;
    int node_idx = venueToLocalNodeIndex(static_cast<int>(loc.venue_id));
    if (node_idx < 0) continue;

    const Person* person = world.getPerson(loc.person_id);
    if (!person || !person->infection) continue;

    const auto& traj = person->infection->getTrajectory();

    // Walk transitions to find symptom at t0 and the time it started.
    uint16_t symptom_id = 0;
    double stage_start = person->infection->getInfectionTime();
    for (const auto& trans : traj.transitions) {
      if (t0 >= trans.first) {
        stage_start = trans.first;
        symptom_id = trans.second;
      } else {
        break;
      }
    }

    double t_in_stage_start = t0 - stage_start;
    double t_in_stage_end = t1 - stage_start;

    // Coupling matrix weight for this venue type (pre-resolved or default).
    const Venue* venue = world.getVenue(loc.venue_id);
    int venue_type_id = venue ? static_cast<int>(venue->type_id) : -1;
    float cm_val = coupling_matrix_.getValue(venue_type_id, 0);

    for (const auto& tmode : tp.modes) {
      if (tmode.type != TransmissionModeType::CompartmentalDeposition) continue;
      const auto& dcfg = std::get<CompartmentalDepositionConfig>(tmode.config);
      if (symptom_id >= dcfg.deposition_by_symptom.size()) continue;
      const auto& curve = dcfg.deposition_by_symptom[symptom_id];
      if (!curve) continue;
      double dep = curve->integrate(t_in_stage_start, t_in_stage_end) * 24.0;
      if (dep > 0.0) {
        deposition[static_cast<size_t>(node_idx)] +=
            static_cast<float>(dep) * cm_val;
      }
    }
  }

  writeCouplingInputs(deposition.data(), n_nodes);
}

}  // namespace june
