#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <type_traits>

#include "loaders/policy_loader.h"
#include "utils/filtered_csv.h"

#ifdef USE_MPI
#include <mpi.h>
#endif

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
    const std::string& path, TransmissionPolicyKind policy_kind,
    uint16_t policy_index, double compliance_rate,
    const std::string& policy_name) {
  const csv::FilteredTable table = csv::loadFilteredCSV(path);
  const std::set<std::string> required = {"scope", "transmission_mode",
                                          "effect_channel", "multiplier"};
  std::set<std::string> columns;
  for (const auto& column : table.value_columns) {
    if (!columns.insert(column).second) {
      throw std::runtime_error(path + ": duplicate column '" + column + "'");
    }
  }
  for (const auto& column : required) {
    if (!columns.count(column)) {
      throw std::runtime_error(path + ": missing required column '" + column +
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
    effect.scope = parseScope(requireValue(row, "scope", path, row_number),
                              path, row_number);
    effect.mode_name = requireValue(row, "transmission_mode", path, row_number);
    effect.channel =
        parseChannel(requireValue(row, "effect_channel", path, row_number),
                     path, row_number);
    effect.multiplier = parseMultiplier(
        requireValue(row, "multiplier", path, row_number), path, row_number);

    const bool person_channel =
        effect.channel == TransmissionEffectChannel::SourceInfectiousness ||
        effect.channel == TransmissionEffectChannel::TargetSusceptibility;
    const bool venue_channel =
        effect.channel == TransmissionEffectChannel::ContactIntensity ||
        effect.channel == TransmissionEffectChannel::EnvironmentalRisk;
    if ((person_channel && effect.scope != TransmissionEffectScope::Person) ||
        (venue_channel && effect.scope != TransmissionEffectScope::Venue)) {
      throw std::runtime_error(
          path + ": row " + std::to_string(row_number) + ": effect_channel " +
          channelName(effect.channel) + " requires scope '" +
          (person_channel ? "person" : "venue") + "'");
    }
    if (effect.scope == TransmissionEffectScope::Venue &&
        (policy_kind != TransmissionPolicyKind::Temporal ||
         compliance_rate != 1.0)) {
      throw std::runtime_error(
          path + ": row " + std::to_string(row_number) +
          ": venue effects require a temporal policy with compliance_rate 1");
    }
    if (!seen.insert(duplicateKey(effect)).second) {
      throw std::runtime_error(path + ": row " + std::to_string(row_number) +
                               ": duplicate matching transmission effect");
    }
    effects.push_back(std::move(effect));
  }
  return effects;
}

template <typename T>
void append(std::vector<char>& bytes, T value) {
  static_assert(std::is_trivially_copyable_v<T>);
  const char* first = reinterpret_cast<const char*>(&value);
  bytes.insert(bytes.end(), first, first + sizeof(T));
}

void appendString(std::vector<char>& bytes, const std::string& value) {
  append<uint64_t>(bytes, value.size());
  bytes.insert(bytes.end(), value.begin(), value.end());
}

void appendPropertyValue(std::vector<char>& bytes, const PropertyValue& value) {
  const uint8_t tag = static_cast<uint8_t>(value.index());
  append<uint8_t>(bytes, tag);
  std::visit(
      [&bytes](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          return;
        } else if constexpr (std::is_same_v<T, std::string>) {
          appendString(bytes, item);
        } else if constexpr (std::is_same_v<T, std::vector<int32_t>> ||
                             std::is_same_v<T, std::vector<std::string>>) {
          append<uint64_t>(bytes, item.size());
          for (const auto& element : item) {
            if constexpr (std::is_same_v<T, std::vector<int32_t>>)
              append<int32_t>(bytes, element);
            else
              appendString(bytes, element);
          }
        } else {
          append<T>(bytes, item);
        }
      },
      value);
}

class Reader {
 public:
  explicit Reader(const std::vector<char>& bytes) : bytes_(bytes) {}

  template <typename T>
  T read() {
    static_assert(std::is_trivially_copyable_v<T>);
    if (bytes_.size() - offset_ < sizeof(T)) throw badData();
    T value;
    std::memcpy(&value, bytes_.data() + offset_, sizeof(T));
    offset_ += sizeof(T);
    return value;
  }

  std::string readString() {
    const uint64_t size = read<uint64_t>();
    if (size > bytes_.size() - offset_) throw badData();
    std::string value(bytes_.data() + offset_, bytes_.data() + offset_ + size);
    offset_ += size;
    return value;
  }

  PropertyValue readPropertyValue() {
    switch (read<uint8_t>()) {
      case 0:
        return std::monostate{};
      case 1:
        return read<bool>();
      case 2:
        return read<int32_t>();
      case 3:
        return read<double>();
      case 4:
        return readString();
      case 5: {
        const uint64_t size = read<uint64_t>();
        std::vector<int32_t> result;
        result.reserve(size);
        for (uint64_t i = 0; i < size; ++i) result.push_back(read<int32_t>());
        return result;
      }
      case 6: {
        const uint64_t size = read<uint64_t>();
        std::vector<std::string> result;
        result.reserve(size);
        for (uint64_t i = 0; i < size; ++i) result.push_back(readString());
        return result;
      }
      default:
        throw badData();
    }
  }

  bool atEnd() const { return offset_ == bytes_.size(); }

 private:
  static std::runtime_error badData() {
    return std::runtime_error("invalid broadcast transmission policy data");
  }

  const std::vector<char>& bytes_;
  size_t offset_ = 0;
};

std::vector<char> serializeEffects(
    const std::vector<PolicyTransmissionEffect>& effects) {
  std::vector<char> bytes;
  append<uint64_t>(bytes, effects.size());
  for (const auto& effect : effects) {
    append<uint8_t>(bytes, static_cast<uint8_t>(effect.policy_kind));
    append<uint16_t>(bytes, effect.policy_index);
    append<uint8_t>(bytes, static_cast<uint8_t>(effect.scope));
    append<uint8_t>(bytes, static_cast<uint8_t>(effect.channel));
    appendString(bytes, effect.mode_name);
    append<double>(bytes, effect.multiplier);
    append<uint64_t>(bytes, effect.criteria.size());
    for (const auto& criterion : effect.criteria) {
      appendString(bytes, criterion.property_path);
      appendString(bytes, criterion.operator_type);
      appendPropertyValue(bytes, criterion.value);
    }
  }
  return bytes;
}

std::vector<PolicyTransmissionEffect> deserializeEffects(
    const std::vector<char>& bytes) {
  Reader reader(bytes);
  const uint64_t effect_count = reader.read<uint64_t>();
  std::vector<PolicyTransmissionEffect> effects;
  effects.reserve(effect_count);
  for (uint64_t i = 0; i < effect_count; ++i) {
    PolicyTransmissionEffect effect;
    effect.policy_kind =
        static_cast<TransmissionPolicyKind>(reader.read<uint8_t>());
    effect.policy_index = reader.read<uint16_t>();
    effect.scope = static_cast<TransmissionEffectScope>(reader.read<uint8_t>());
    effect.channel =
        static_cast<TransmissionEffectChannel>(reader.read<uint8_t>());
    effect.mode_name = reader.readString();
    effect.multiplier = reader.read<double>();
    const uint64_t criterion_count = reader.read<uint64_t>();
    effect.criteria.reserve(criterion_count);
    for (uint64_t j = 0; j < criterion_count; ++j) {
      SelectionCriterion criterion;
      criterion.property_path = reader.readString();
      criterion.operator_type = reader.readString();
      criterion.value = reader.readPropertyValue();
      effect.criteria.push_back(std::move(criterion));
    }
    effects.push_back(std::move(effect));
  }
  if (!reader.atEnd()) {
    throw std::runtime_error("invalid broadcast transmission policy data");
  }
  return effects;
}

#ifdef USE_MPI
bool mpiActive() {
  int initialized = 0;
  int finalized = 0;
  MPI_Initialized(&initialized);
  if (!initialized) return false;
  MPI_Finalized(&finalized);
  return finalized == 0;
}

void broadcastBytes(std::vector<char>& bytes) {
  uint64_t size = bytes.size();
  MPI_Bcast(&size, 1, MPI_UINT64_T, 0, MPI_COMM_WORLD);
  bytes.resize(size);
  uint64_t offset = 0;
  while (offset < size) {
    const int chunk = static_cast<int>(std::min<uint64_t>(
        size - offset, static_cast<uint64_t>(std::numeric_limits<int>::max())));
    MPI_Bcast(bytes.data() + offset, chunk, MPI_BYTE, 0, MPI_COMM_WORLD);
    offset += static_cast<uint64_t>(chunk);
  }
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

  std::vector<char> bytes;
#ifdef USE_MPI
  const bool use_mpi = mpiActive();
  int rank = 0;
  if (use_mpi) MPI_Comm_rank(MPI_COMM_WORLD, &rank);
#else
  constexpr bool use_mpi = false;
  constexpr int rank = 0;
#endif

  int success = 1;
  std::string error;
  if (!use_mpi || rank == 0) {
    try {
      // Keep compatibility with existing configs whose paths are relative to
      // the process working directory. Resolve against policies.yaml only
      // when that spelling does not exist there.
      std::filesystem::path resolved_path(csv_path);
      if (resolved_path.is_relative() &&
          !std::filesystem::exists(resolved_path)) {
        resolved_path = std::filesystem::path(policies_filename).parent_path() /
                        resolved_path;
      }
      bytes = serializeEffects(parseEffects(resolved_path.string(), policy_kind,
                                            policy_index, compliance_rate,
                                            policy_name));
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
    broadcastBytes(bytes);
  }
#endif

  if (!success) throw std::runtime_error(error);
  for (auto& effect : deserializeEffects(bytes)) {
    policy_manager.addTransmissionEffect(effect);
  }
}

}  // namespace june
