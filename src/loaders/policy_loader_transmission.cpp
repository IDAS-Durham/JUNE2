#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <type_traits>

#include "loaders/policy_loader.h"
#include "parallel/mpi_utils.h"
#include "utils/filtered_csv.h"
#include "utils/mpi_utils.h"

namespace june {
namespace {

const char* channelName(TransmissionEffectChannel channel) {
  switch (channel) {
    case TransmissionEffectChannel::SourceInfectiousness:
      return "source_infectiousness";
    case TransmissionEffectChannel::TargetSusceptibility:
      return "target_susceptibility";
    case TransmissionEffectChannel::ContactIntensity:
      return "contact_intensity";
    case TransmissionEffectChannel::EnvironmentalRisk:
      return "environmental_risk";
  }
  return "unknown";
}

TransmissionEffectChannel parseChannel(const std::string& value,
                                       const std::string& source, size_t row) {
  if (value == "source_infectiousness")
    return TransmissionEffectChannel::SourceInfectiousness;
  if (value == "target_susceptibility")
    return TransmissionEffectChannel::TargetSusceptibility;
  if (value == "contact_intensity")
    return TransmissionEffectChannel::ContactIntensity;
  if (value == "environmental_risk")
    return TransmissionEffectChannel::EnvironmentalRisk;
  throw std::runtime_error(source + ": row " + std::to_string(row) +
                           ": unknown effect_channel '" + value + "'");
}

TransmissionEffectScope parseScope(const std::string& value,
                                   const std::string& source, size_t row) {
  if (value == "person") return TransmissionEffectScope::Person;
  if (value == "venue") return TransmissionEffectScope::Venue;
  throw std::runtime_error(source + ": row " + std::to_string(row) +
                           ": scope must be 'person' or 'venue'");
}

double parseMultiplier(const std::string& value, const std::string& source,
                       size_t row) {
  if (value.empty()) {
    throw std::runtime_error(source + ": row " + std::to_string(row) +
                             ": multiplier is required");
  }
  size_t consumed = 0;
  double result = 0.0;
  try {
    result = std::stod(value, &consumed);
  } catch (...) {
    throw std::runtime_error(source + ": row " + std::to_string(row) +
                             ": invalid multiplier '" + value + "'");
  }
  if (consumed != value.size() || !std::isfinite(result) || result < 0.0) {
    throw std::runtime_error(source + ": row " + std::to_string(row) +
                             ": multiplier must be a finite non-negative "
                             "number");
  }
  return result;
}

std::string propertyValueKey(const PropertyValue& value) {
  std::ostringstream out;
  out << std::setprecision(std::numeric_limits<double>::max_digits10);
  out << value.index() << ':';
  std::visit(
      [&out](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          out << "null";
        } else if constexpr (std::is_same_v<T, std::vector<int32_t>> ||
                             std::is_same_v<T, std::vector<std::string>>) {
          out << '[';
          for (const auto& element : item) out << element << ';';
          out << ']';
        } else {
          out << item;
        }
      },
      value);
  return out.str();
}

std::string duplicateKey(const PolicyTransmissionEffect& effect) {
  std::vector<std::string> criteria;
  criteria.reserve(effect.criteria.size());
  for (const auto& criterion : effect.criteria) {
    criteria.push_back(criterion.property_path + "\x1f" +
                       criterion.operator_type + "\x1f" +
                       propertyValueKey(criterion.value));
  }
  std::sort(criteria.begin(), criteria.end());

  std::ostringstream out;
  out << static_cast<int>(effect.policy_kind) << ':' << effect.policy_index
      << ':' << static_cast<int>(effect.scope) << ':'
      << static_cast<int>(effect.channel) << ':' << effect.mode_name << ':';
  for (const auto& criterion : criteria) out << criterion << '\x1e';
  return out.str();
}

std::string requireValue(const csv::FilteredRow& row, const char* name,
                         const std::string& source, size_t row_number) {
  auto it = row.values.find(name);
  if (it == row.values.end() || it->second.empty()) {
    throw std::runtime_error(source + ": row " + std::to_string(row_number) +
                             ": '" + name + "' is required");
  }
  return it->second;
}

std::vector<PolicyTransmissionEffect> parseEffects(
    const std::string& csv_text, const std::string& source,
    TransmissionPolicyKind policy_kind, uint16_t policy_index,
    double compliance_rate, const std::string& policy_name) {
  std::istringstream input(csv_text);
  const csv::FilteredTable table = csv::loadFilteredCSV(input, source);
  const std::set<std::string> required = {"scope", "transmission_mode",
                                          "effect_channel", "multiplier"};
  std::set<std::string> columns;
  for (const auto& column : table.value_columns) {
    if (!columns.insert(column).second) {
      throw std::runtime_error(source + ": duplicate column '" + column + "'");
    }
  }
  for (const auto& column : required) {
    if (!columns.count(column)) {
      throw std::runtime_error(source + ": missing required column '" + column +
                               "'");
    }
  }

  std::vector<PolicyTransmissionEffect> effects;
  std::set<std::string> seen;
  for (size_t i = 0; i < table.rows.size(); ++i) {
    const size_t row_number = i + 2;  // header is row one
    const auto& row = table.rows[i];
    const auto policy_it = row.values.find("policy");
    if (policy_it != row.values.end() && !policy_it->second.empty() &&
        policy_it->second != policy_name)
      continue;
    PolicyTransmissionEffect effect;
    effect.policy_kind = policy_kind;
    effect.policy_index = policy_index;
    effect.criteria = row.criteria;
    effect.scope = parseScope(requireValue(row, "scope", source, row_number),
                              source, row_number);
    effect.mode_name =
        requireValue(row, "transmission_mode", source, row_number);
    effect.channel =
        parseChannel(requireValue(row, "effect_channel", source, row_number),
                     source, row_number);
    effect.multiplier =
        parseMultiplier(requireValue(row, "multiplier", source, row_number),
                        source, row_number);

    const bool person_channel =
        effect.channel == TransmissionEffectChannel::SourceInfectiousness ||
        effect.channel == TransmissionEffectChannel::TargetSusceptibility;
    const bool venue_channel =
        effect.channel == TransmissionEffectChannel::ContactIntensity ||
        effect.channel == TransmissionEffectChannel::EnvironmentalRisk;
    if ((person_channel && effect.scope != TransmissionEffectScope::Person) ||
        (venue_channel && effect.scope != TransmissionEffectScope::Venue)) {
      throw std::runtime_error(
          source + ": row " + std::to_string(row_number) + ": effect_channel " +
          channelName(effect.channel) + " requires scope '" +
          (person_channel ? "person" : "venue") + "'");
    }
    if (effect.scope == TransmissionEffectScope::Venue &&
        (policy_kind != TransmissionPolicyKind::Temporal ||
         compliance_rate != 1.0)) {
      throw std::runtime_error(
          source + ": row " + std::to_string(row_number) +
          ": venue effects require a temporal policy with compliance_rate 1");
    }
    if (!seen.insert(duplicateKey(effect)).second) {
      throw std::runtime_error(source + ": row " + std::to_string(row_number) +
                               ": duplicate matching transmission effect");
    }
    effects.push_back(std::move(effect));
  }
  return effects;
}

#ifdef USE_MPI
void broadcastBytes(std::vector<char>& bytes) {
  uint64_t size = bytes.size();
  MPI_Bcast(&size, 1, MPI_UINT64_T, 0, MPI_COMM_WORLD);
  bytes.resize(size);
  mpi_utils::broadcastChunked(bytes.data(), size, sizeof(char), MPI_BYTE, 0);
}
#endif

}  // namespace

void PolicyLoader::loadTransmissionEffects(
    PolicyManager& policy_manager, const std::string& csv_path,
    TransmissionPolicyKind policy_kind, uint16_t policy_index,
    double compliance_rate, const std::string& policy_name,
    const std::string& policies_filename) {
  if (csv_path.empty()) {
    throw std::runtime_error("transmission_effects_file must not be empty");
  }

  std::filesystem::path resolved_path(csv_path);
  if (resolved_path.is_relative() && !std::filesystem::exists(resolved_path)) {
    resolved_path =
        std::filesystem::path(policies_filename).parent_path() / resolved_path;
  }

  std::vector<char> csv_bytes;
#ifdef USE_MPI
  const auto mpi = mpi_runtime::state();
  const bool use_mpi = mpi.active;
  const int rank = mpi.rank;
#else
  constexpr bool use_mpi = false;
  constexpr int rank = 0;
#endif

  int success = 1;
  std::string error;
  if (!use_mpi || rank == 0) {
    try {
      std::ifstream input(resolved_path);
      if (!input.is_open()) {
        throw std::runtime_error("Failed to open transmission effects CSV: " +
                                 resolved_path.string());
      }
      csv_bytes.assign(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
      if (input.bad()) {
        throw std::runtime_error("Failed to read transmission effects CSV: " +
                                 resolved_path.string());
      }
    } catch (const std::exception& e) {
      success = 0;
      error = e.what();
    }
  }

#ifdef USE_MPI
  if (use_mpi) {
    MPI_Bcast(&success, 1, MPI_INT, 0, MPI_COMM_WORLD);
    if (!success) {
      uint64_t error_size = error.size();
      MPI_Bcast(&error_size, 1, MPI_UINT64_T, 0, MPI_COMM_WORLD);
      std::vector<char> error_bytes;
      if (rank == 0) error_bytes.assign(error.begin(), error.end());
      error_bytes.resize(error_size);
      if (error_size > 0)
        MPI_Bcast(error_bytes.data(), static_cast<int>(error_size), MPI_BYTE, 0,
                  MPI_COMM_WORLD);
      if (rank != 0) error.assign(error_bytes.begin(), error_bytes.end());
      throw std::runtime_error(error);
    }
    broadcastBytes(csv_bytes);
  }
#endif

  if (!success) throw std::runtime_error(error);
  const std::string csv_text(csv_bytes.begin(), csv_bytes.end());
  for (auto& effect :
       parseEffects(csv_text, resolved_path.string(), policy_kind, policy_index,
                    compliance_rate, policy_name)) {
    policy_manager.addTransmissionEffect(effect);
  }
}

}  // namespace june
