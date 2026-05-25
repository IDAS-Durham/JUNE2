#ifdef USE_MPI

#include "../../include/parallel/domain_communicator.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <string>

#include "../../include/parallel/domain_manager.h"
#include "../../include/parallel/mpi_utils.h"
#include "../../include/utils/deterministic_rng.h"
#include "../../include/utils/profiler.h"
#include "../../include/utils/random.h"

namespace {

// Generic field-level pack/unpack: copies sizeof(T) bytes and advances the
// cursor. Sender and receiver must agree on field order and types; the wire
// size is `sum of sizeof(field)` per struct. These helpers eliminate the
// hand-written `memcpy(..., N); ptr += N;` pairs while preserving the exact
// byte-for-byte wire format the original code produced.
template <typename T>
inline char* packField(char* ptr, const T& v) {
  static_assert(std::is_trivially_copyable_v<T>,
                "packField requires a trivially copyable type");
  std::memcpy(ptr, &v, sizeof(T));
  return ptr + sizeof(T);
}

template <typename T>
inline const char* unpackField(const char* ptr, T& v) {
  static_assert(std::is_trivially_copyable_v<T>,
                "unpackField requires a trivially copyable type");
  std::memcpy(&v, ptr, sizeof(T));
  return ptr + sizeof(T);
}

// Fixed-size header of the visitor wire format (everything before the
// integrated_infectiousness payload): 4+4+4+4+1+1+4+1+2+8 = 33 bytes.
// Total wire size = VISITOR_WIRE_HEADER + num_modes * sizeof(double).
constexpr int VISITOR_WIRE_HEADER = 33;
inline int visitorWireSize(int num_modes) {
  return VISITOR_WIRE_HEADER + num_modes * static_cast<int>(sizeof(double));
}

char* packVisitor(char* ptr, const june::Domain::VisitorData& v,
                  int num_modes) {
  ptr = packField(ptr, v.person_id);
  ptr = packField(ptr, v.home_rank);
  ptr = packField(ptr, v.venue_id);
  ptr = packField(ptr, v.subset_idx);
  ptr = packField(ptr, v.is_infected);
  ptr = packField(ptr, v.is_infectious);
  ptr = packField(ptr, v.immunity_level);
  ptr = packField(ptr, v.encounter_type_id);
  ptr = packField(ptr, v.symptom_id);
  ptr = packField(ptr, v.time_in_stage);
  // Variable-length per-mode payload. The vector must be exactly num_modes
  // long; sender and receiver agree on this from disease->numModes(), which
  // is loaded identically on every rank. Mismatches indicate a config bug
  // and are caught loud at the build site rather than papered over here.
  if (static_cast<int>(v.integrated_infectiousness.size()) != num_modes) {
    throw std::runtime_error(
        "packVisitor: integrated_infectiousness size " +
        std::to_string(v.integrated_infectiousness.size()) + " != num_modes " +
        std::to_string(num_modes));
  }
  if (num_modes > 0) {
    std::memcpy(ptr, v.integrated_infectiousness.data(),
                num_modes * sizeof(double));
    ptr += num_modes * sizeof(double);
  }
  return ptr;
}

const char* unpackVisitor(const char* ptr, june::Domain::VisitorData& v,
                          int num_modes) {
  ptr = unpackField(ptr, v.person_id);
  ptr = unpackField(ptr, v.home_rank);
  ptr = unpackField(ptr, v.venue_id);
  ptr = unpackField(ptr, v.subset_idx);
  ptr = unpackField(ptr, v.is_infected);
  ptr = unpackField(ptr, v.is_infectious);
  ptr = unpackField(ptr, v.immunity_level);
  ptr = unpackField(ptr, v.encounter_type_id);
  ptr = unpackField(ptr, v.symptom_id);
  ptr = unpackField(ptr, v.time_in_stage);
  v.integrated_infectiousness.assign(num_modes, 0.0);
  if (num_modes > 0) {
    std::memcpy(v.integrated_infectiousness.data(), ptr,
                num_modes * sizeof(double));
    ptr += num_modes * sizeof(double);
  }
  return ptr;
}

constexpr int PROPOSAL_WIRE_SIZE = 33;  // 4*8+1

char* packProposal(char* ptr, const june::EncounterProposal& p) {
  ptr = packField(ptr, p.encounter_id);
  ptr = packField(ptr, p.host_id);
  ptr = packField(ptr, p.host_rank);
  ptr = packField(ptr, p.invitee_id);
  ptr = packField(ptr, p.venue_id);
  ptr = packField(ptr, p.venue_owner_rank);
  ptr = packField(ptr, p.venue_type_id);
  ptr = packField(ptr, p.slot);
  ptr = packField(ptr, p.encounter_type_id);
  return ptr;
}

const char* unpackProposal(const char* ptr, june::EncounterProposal& p) {
  ptr = unpackField(ptr, p.encounter_id);
  ptr = unpackField(ptr, p.host_id);
  ptr = unpackField(ptr, p.host_rank);
  ptr = unpackField(ptr, p.invitee_id);
  ptr = unpackField(ptr, p.venue_id);
  ptr = unpackField(ptr, p.venue_owner_rank);
  ptr = unpackField(ptr, p.venue_type_id);
  ptr = unpackField(ptr, p.slot);
  ptr = unpackField(ptr, p.encounter_type_id);
  return ptr;
}

constexpr int REPLY_WIRE_SIZE = 26;  // 4*6+1+1

char* packReply(char* ptr, const june::EncounterReply& r) {
  ptr = packField(ptr, r.encounter_id);
  ptr = packField(ptr, r.host_id);
  ptr = packField(ptr, r.invitee_id);
  ptr = packField(ptr, r.venue_id);
  ptr = packField(ptr, r.venue_type_id);
  ptr = packField(ptr, r.slot);
  ptr = packField(ptr, r.encounter_type_id);
  uint8_t status_byte = static_cast<uint8_t>(r.status);
  ptr = packField(ptr, status_byte);
  return ptr;
}

const char* unpackReply(const char* ptr, june::EncounterReply& r) {
  ptr = unpackField(ptr, r.encounter_id);
  ptr = unpackField(ptr, r.host_id);
  ptr = unpackField(ptr, r.invitee_id);
  ptr = unpackField(ptr, r.venue_id);
  ptr = unpackField(ptr, r.venue_type_id);
  ptr = unpackField(ptr, r.slot);
  ptr = unpackField(ptr, r.encounter_type_id);
  uint8_t status_byte;
  ptr = unpackField(ptr, status_byte);
  r.status = static_cast<june::ReplyStatus>(status_byte);
  return ptr;
}

// =============================================================================
// Diagnostic helpers for hunting MPI proposal/reply corruption.
// These check structural sanity only — no masking or clamping of values.
// On first failure the process MPI_Aborts with rich context so SLURM captures
// the diagnostic before any downstream collective can deadlock.
// =============================================================================

// Returns empty string if the proposal looks structurally sane. Otherwise
// returns a short description of the first field that failed. We use loose
// upper bounds (256 for venue_type_id/slot, encounter_type_names.size() for
// encounter_type_id) — tight enough to catch the garbage values we've seen
// (0x3FF00000 etc.), loose enough not to accuse legitimate values.
std::string validateProposal(const june::EncounterProposal& p, int num_ranks,
                             size_t num_encounter_types) {
  if (p.encounter_id < 0) return "encounter_id < 0";
  if (p.host_id < 0) return "host_id < 0";
  if (p.host_rank < 0 || p.host_rank >= num_ranks)
    return "host_rank out of [0,num_ranks)";
  if (p.invitee_id < 0) return "invitee_id < 0";
  if (p.venue_owner_rank < 0 || p.venue_owner_rank >= num_ranks)
    return "venue_owner_rank out of [0,num_ranks)";
  if (p.venue_type_id < 0 || p.venue_type_id >= 256)
    return "venue_type_id out of [0,256)";
  if (p.slot < 0 || p.slot >= 256) return "slot out of [0,256)";
  if (num_encounter_types > 0 && p.encounter_type_id >= num_encounter_types)
    return "encounter_type_id >= encounter_type_names.size()";
  return {};
}

std::string validateReply(const june::EncounterReply& r,
                          size_t num_encounter_types) {
  if (r.encounter_id < 0) return "encounter_id < 0";
  if (r.host_id < 0) return "host_id < 0";
  if (r.invitee_id < 0) return "invitee_id < 0";
  if (r.venue_type_id < 0 || r.venue_type_id >= 256)
    return "venue_type_id out of [0,256)";
  if (r.slot < 0 || r.slot >= 256) return "slot out of [0,256)";
  if (num_encounter_types > 0 && r.encounter_type_id >= num_encounter_types)
    return "encounter_type_id >= encounter_type_names.size()";
  if (static_cast<uint8_t>(r.status) > 6) return "status out of range";
  return {};
}

void dumpProposal(std::ostream& os, const june::EncounterProposal& p) {
  os << "{encounter_id=" << p.encounter_id << " host_id=" << p.host_id
     << " host_rank=" << p.host_rank << " invitee_id=" << p.invitee_id
     << " venue_id=" << p.venue_id << " venue_owner_rank=" << p.venue_owner_rank
     << " venue_type_id=" << p.venue_type_id << " slot=" << p.slot
     << " encounter_type_id=" << static_cast<int>(p.encounter_type_id) << "}";
}

void dumpReply(std::ostream& os, const june::EncounterReply& r) {
  os << "{encounter_id=" << r.encounter_id << " host_id=" << r.host_id
     << " invitee_id=" << r.invitee_id << " venue_id=" << r.venue_id
     << " venue_type_id=" << r.venue_type_id << " slot=" << r.slot
     << " encounter_type_id=" << static_cast<int>(r.encounter_type_id)
     << " status=" << static_cast<int>(r.status) << "}";
}

// Hex-dumps a range of bytes around a focal offset. 'focal' bytes are marked
// with '>' at the start of their 16-byte row so the record in question is
// easy to spot.
void hexDumpRegion(std::ostream& os, const char* base, size_t buf_len,
                   size_t focal_offset, size_t focal_len,
                   size_t context_bytes = 64) {
  auto flags = os.flags();
  auto fill = os.fill();
  size_t start =
      (focal_offset > context_bytes) ? focal_offset - context_bytes : 0;
  size_t end = std::min(buf_len, focal_offset + focal_len + context_bytes);
  for (size_t row = start - (start % 16); row < end; row += 16) {
    bool in_focal =
        (row + 16 > focal_offset) && (row < focal_offset + focal_len);
    os << (in_focal ? "  > " : "    ") << std::setw(6) << std::setfill('0')
       << std::hex << row << ":";
    for (size_t col = 0; col < 16; ++col) {
      size_t off = row + col;
      if (off < start || off >= end) {
        os << "   ";
      } else {
        os << ' ' << std::setw(2) << std::setfill('0') << std::hex
           << static_cast<int>(static_cast<unsigned char>(base[off]));
      }
    }
    os << '\n';
  }
  os.flags(flags);
  os.fill(fill);
  os << std::dec;
}

// Checks that per-rank byte counts fit in int (MPI_Alltoallv uses int counts).
// Returns true on success; on failure prints context and calls MPI_Abort.
bool checkByteOverflow(const std::vector<int>& counts, int wire_size, int rank,
                       const char* tag) {
  const int64_t int_max = std::numeric_limits<int>::max();
  int64_t running = 0;
  for (size_t r = 0; r < counts.size(); ++r) {
    int64_t bytes = static_cast<int64_t>(counts[r]) * wire_size;
    if (counts[r] < 0 || bytes > int_max) {
      std::cerr << "[Rank " << rank << "] FATAL: " << tag
                << " byte-count overflow: counts[" << r << "]=" << counts[r]
                << " * wire_size=" << wire_size << " = " << bytes
                << " (int max " << int_max << ")" << std::endl;
      MPI_Abort(MPI_COMM_WORLD, 101);
      return false;
    }
    running += bytes;
    if (running > int_max) {
      std::cerr << "[Rank " << rank << "] FATAL: " << tag
                << " cumulative byte total overflows int at rank " << r
                << " (running=" << running << ")" << std::endl;
      MPI_Abort(MPI_COMM_WORLD, 102);
      return false;
    }
  }
  return true;
}

void dumpCountsAndDispls(std::ostream& os, const std::vector<int>& send_counts,
                         const std::vector<int>& recv_counts,
                         const std::vector<int>& sd, const std::vector<int>& rd,
                         int total_send, int total_recv) {
  os << "    send_counts:";
  for (int c : send_counts) os << ' ' << c;
  os << "\n    recv_counts:";
  for (int c : recv_counts) os << ' ' << c;
  os << "\n    send_byte_displs:";
  for (int d : sd) os << ' ' << d;
  os << "\n    recv_byte_displs:";
  for (int d : rd) os << ' ' << d;
  os << "\n    total_send_bytes=" << total_send
     << " total_recv_bytes=" << total_recv << '\n';
}

// =============================================================================
// Templated route-and-Alltoallv exchange shared by encounter proposals and
// replies. Both call sites had a near-identical 150-LOC body that differed
// only in the record type, wire size, per-record validate/dump/pack/unpack
// functions, routing field, abort codes, and diagnostic tag ("proposal" /
// "reply"). All of those are passed in; the byte-level wire format is the
// caller's responsibility (delegated to pack_fn / unpack_fn).
//
// The send/recv counts are exposed via out-params so the caller can emit
// its own JUNE_MPI_DEBUG postlude (proposals and replies print different
// stats).
//
// Template parameters are deduced; pack_fn / unpack_fn / etc. are passed as
// free functions or lambdas — no std::function, no heap, no vtable dispatch.
// =============================================================================
template <typename T, typename Pack, typename Unpack, typename Validate,
          typename Dump, typename Route>
void exchangeRoutedRecords(const std::vector<T>& local_records,
                           std::vector<T>& records_for_this_rank, int rank,
                           int num_ranks, int wire_size, const char* kind,
                           int before_abort, int total_neg_abort,
                           int pack_abort, int unpack_abort, Pack pack_fn,
                           Unpack unpack_fn, Validate validate_fn,
                           Dump dump_fn, Route route_fn,
                           std::vector<int>& out_send_counts,
                           std::vector<int>& out_recv_counts) {
  // [DIAG] Validate every local record before anything else. If we see
  // corruption here, the bug is in the generator, not in MPI.
  for (size_t i = 0; i < local_records.size(); ++i) {
    std::string err = validate_fn(local_records[i]);
    if (!err.empty()) {
      std::cerr << "[Rank " << rank << "] FATAL: corrupt local " << kind
                << " BEFORE MPI exchange"
                << " at local_records[" << i << "/" << local_records.size()
                << "]: " << err << "\n  ";
      dump_fn(std::cerr, local_records[i]);
      std::cerr << std::endl;
      MPI_Abort(MPI_COMM_WORLD, before_abort);
    }
  }

  // Route each record to its target rank. Unknown rank -> keep locally.
  std::vector<std::vector<T>> per_rank(num_ranks);
  for (const auto& rec : local_records) {
    int target = route_fn(rec);
    if (target < 0 || target >= num_ranks) {
      per_rank[rank].push_back(rec);
    } else {
      per_rank[target].push_back(rec);
    }
  }

  records_for_this_rank = std::move(per_rank[rank]);

  std::vector<int> send_counts(num_ranks, 0);
  for (int r = 0; r < num_ranks; ++r) {
    send_counts[r] = static_cast<int>(per_rank[r].size());
  }
  send_counts[rank] = 0;  // Don't send to self

  std::string send_tag = std::string(kind) + " send";
  if (!checkByteOverflow(send_counts, wire_size, rank, send_tag.c_str())) {
    return;
  }

  std::vector<int> recv_counts(num_ranks, 0);
  MPI_Alltoall(send_counts.data(), 1, MPI_INT, recv_counts.data(), 1, MPI_INT,
               MPI_COMM_WORLD);

  std::string recv_tag = std::string(kind) + " recv";
  if (!checkByteOverflow(recv_counts, wire_size, rank, recv_tag.c_str())) {
    return;
  }

  std::vector<int> sd, rd;
  int total_send, total_recv;
  june::mpi_utils::computeByteDisplacements(send_counts, wire_size, sd,
                                            total_send);
  june::mpi_utils::computeByteDisplacements(recv_counts, wire_size, rd,
                                            total_recv);

  if (total_send < 0 || total_recv < 0) {
    std::cerr << "[Rank " << rank << "] FATAL: " << kind
              << " byte totals negative after "
                 "computeByteDisplacements: total_send="
              << total_send << " total_recv=" << total_recv << std::endl;
    MPI_Abort(MPI_COMM_WORLD, total_neg_abort);
  }

  std::vector<char> sbuf(total_send);
  std::vector<char> rbuf(total_recv);

  // Pack + validate each record on the way out. A second validate here
  // catches any corruption introduced between the BEFORE check and now.
  for (int r = 0; r < num_ranks; ++r) {
    if (r == rank) continue;
    char* ptr = sbuf.data() + sd[r];
    for (size_t i = 0; i < per_rank[r].size(); ++i) {
      const auto& rec = per_rank[r][i];
      std::string err = validate_fn(rec);
      if (!err.empty()) {
        std::cerr << "[Rank " << rank << "] FATAL: corrupt " << kind
                  << " at PACK time for target rank " << r << ", per_rank[" << r
                  << "][" << i << "/" << per_rank[r].size() << "]: " << err
                  << "\n  ";
        dump_fn(std::cerr, rec);
        std::cerr << std::endl;
        MPI_Abort(MPI_COMM_WORLD, pack_abort);
      }
      ptr = pack_fn(ptr, rec);
    }
  }

  std::vector<int> sc(num_ranks), rc(num_ranks);
  for (int i = 0; i < num_ranks; ++i) {
    sc[i] = send_counts[i] * wire_size;
    rc[i] = recv_counts[i] * wire_size;
  }
  MPI_Alltoallv(sbuf.data(), sc.data(), sd.data(), MPI_BYTE, rbuf.data(),
                rc.data(), rd.data(), MPI_BYTE, MPI_COMM_WORLD);

  // Unpack + validate. First corrupt record gets a rich dump then MPI_Abort.
  for (int r = 0; r < num_ranks; ++r) {
    if (r == rank) continue;
    const char* ptr = rbuf.data() + rd[r];
    for (int i = 0; i < recv_counts[r]; ++i) {
      const char* record_start = ptr;
      T rec;
      ptr = unpack_fn(ptr, rec);
      std::string err = validate_fn(rec);
      if (!err.empty()) {
        size_t offset = static_cast<size_t>(record_start - rbuf.data());
        std::cerr << "[Rank " << rank << "] FATAL: corrupt " << kind
                  << " AFTER MPI unpack"
                  << " from rank " << r << ", record " << i << "/"
                  << recv_counts[r] << " at buffer offset " << offset
                  << " (wire_size=" << wire_size << "): " << err << "\n  ";
        dump_fn(std::cerr, rec);
        std::cerr << "\n";
        dumpCountsAndDispls(std::cerr, send_counts, recv_counts, sd, rd,
                            total_send, total_recv);
        std::cerr << "  hex dump of recv buffer around offset " << offset
                  << ":\n";
        hexDumpRegion(std::cerr, rbuf.data(), static_cast<size_t>(total_recv),
                      offset, static_cast<size_t>(wire_size));
        std::cerr.flush();
        MPI_Abort(MPI_COMM_WORLD, unpack_abort);
      }
      records_for_this_rank.push_back(rec);
    }
  }

  out_send_counts = std::move(send_counts);
  out_recv_counts = std::move(recv_counts);
}

// Builds a fully-populated VisitorData for a person attending a remote
// venue. Pre-computes integrated_infectiousness per mode using the SAME
// code path as local people (getIntegratedInfectiousness) so FP results
// are bit-identical regardless of where a person is processed. The
// integrated_infectiousness vector is always sized to num_modes
// (zero-filled for non-infectious people) — the wire format expects
// exactly that many doubles.
june::Domain::VisitorData buildVisitorPayload(
    const june::PersonLocation& loc, const june::Person& person, int home_rank,
    double current_time, double delta_hours, int num_modes,
    const june::Disease* disease) {
  june::Domain::VisitorData visitor;
  visitor.person_id = loc.person_id;
  visitor.home_rank = home_rank;
  visitor.venue_id = loc.venue_id;
  visitor.subset_idx = loc.subset_index;
  visitor.is_infected = (person.infection != nullptr);
  visitor.is_infectious =
      visitor.is_infected && person.infection->isInfectious(current_time);

  double susceptibility = 1.0;
  if (disease) {
    susceptibility = person.getSusceptibility(current_time, disease->getName());
  } else {
    susceptibility = 1.0 - person.immunity.natural_level;
  }
  visitor.immunity_level = static_cast<float>(1.0 - susceptibility);

  visitor.encounter_type_id = loc.encounter_type_id;
  visitor.newly_infected = false;
  visitor.new_infection_time = -1.0;

  visitor.symptom_id = 0;
  visitor.time_in_stage = 0.0;
  visitor.integrated_infectiousness.assign(num_modes, 0.0);
  if (visitor.is_infected && person.infection) {
    const june::InfectionTrajectory& traj = person.infection->getTrajectory();
    double stage_start_time = traj.infection_time;
    uint16_t cur_symptom_id = 0;
    for (const auto& trans : traj.transitions) {
      if (current_time >= trans.first) {
        stage_start_time = trans.first;
        cur_symptom_id = trans.second;
      } else {
        break;
      }
    }
    visitor.symptom_id = cur_symptom_id;
    visitor.time_in_stage = current_time - stage_start_time;

    if (visitor.is_infectious && disease) {
      double t1 = current_time + delta_hours / 24.0;
      for (int m = 0; m < num_modes; ++m) {
        visitor.integrated_infectiousness[m] =
            person.infection->getIntegratedInfectiousness(m, current_time, t1);
      }
    }
  }
  return visitor;
}

}  // anonymous namespace

namespace june {

DomainCommunicator::DomainCommunicator(WorldState& world, const Config& config,
                                       Domain& domain)
    : world_(world), config_(config), domain_(domain), disease_(nullptr) {
  MPI_Comm_rank(MPI_COMM_WORLD, &rank_);
  MPI_Comm_size(MPI_COMM_WORLD, &num_ranks_);
}

void DomainCommunicator::exchangeVisitors(
    const std::vector<PersonLocation>& locations, const DomainManager& dm,
    double current_time, double delta_hours) {
  domain_.clearVisitors();
  std::vector<std::vector<Domain::VisitorData>> outgoing(num_ranks_);
  std::vector<int> send_counts(num_ranks_, 0);
  // Mode count is fixed for the run (Disease YAML defines it once).
  // Hoist here so the same value sizes every visitor's payload below.
  // Wire size for MPI buffers is computed inside the helpers (Pack/Unpack
  // and exchangeAllToAll/performP2PVisitorExchange) since they each own
  // their own buffer layout.
  const int num_modes =
      (disease_ && disease_->numModes() > 0) ? disease_->numModes() : 1;

  for (const auto& loc : locations) {
    if (loc.venue_id == -1) continue;
    if (!domain_.ownsPerson(loc.person_id)) continue;
    if (domain_.ownsVenue(loc.venue_id)) continue;

    int target_rank = dm.getVenueRank(loc.venue_id);
    if (target_rank == -1) continue;
    if (target_rank == rank_) {
      // Venue is on this rank (e.g., virtual venue hosted locally) — no
      // need to send as visitor; interaction will be computed locally.
      continue;
    }

    Person* person = world_.getPerson(loc.person_id);
    if (!person) continue;

    outgoing[target_rank].push_back(buildVisitorPayload(
        loc, *person, rank_, current_time, delta_hours, num_modes, disease_));
    send_counts[target_rank]++;
  }

#ifdef JUNE_MPI_DEBUG
  if (config_.parallel.verbose_mpi) {
    int total_outgoing = 0, total_infectious_out = 0;
    for (int r = 0; r < num_ranks_; ++r) {
      for (const auto& v : outgoing[r]) {
        total_outgoing++;
        if (v.is_infectious) total_infectious_out++;
      }
    }
    std::cout << "[MPI_XRANK] Rank " << rank_
              << " SENDING: total_visitors=" << total_outgoing
              << " infectious=" << total_infectious_out
              << " at time=" << current_time << std::endl;
  }
#endif

  dispatchVisitorExchange(outgoing, send_counts);
}

void DomainCommunicator::dispatchVisitorExchange(
    const std::vector<std::vector<Domain::VisitorData>>& outgoing,
    const std::vector<int>& send_counts) {
  int local_pairs = 0;
  for (int c : send_counts)
    if (c > 0) local_pairs++;
  int global_pairs;

  MPI_Allreduce(&local_pairs, &global_pairs, 1, MPI_INT, MPI_SUM,
                MPI_COMM_WORLD);
  double sparsity = 100.0 * global_pairs / (num_ranks_ * (num_ranks_ - 1));

  if (sparsity >= 70.0)
    exchangeAllToAll(outgoing, send_counts);
  else if (sparsity >= 15.0)
    exchangePointToPoint(outgoing, send_counts);
  else
    exchangePointToPointVerySparse(outgoing, send_counts);

  for (int r = 0; r < num_ranks_; ++r) {
    for (const auto& v : outgoing[r]) domain_.addOutgoingVisitor(v);
  }
}

void DomainCommunicator::exchangeAllToAll(
    const std::vector<std::vector<Domain::VisitorData>>& outgoing,
    const std::vector<int>& send_counts_in) {
  std::vector<int> send_counts = send_counts_in;
  std::vector<int> recv_counts(num_ranks_, 0);
  MPI_Alltoall(send_counts.data(), 1, MPI_INT, recv_counts.data(), 1, MPI_INT,
               MPI_COMM_WORLD);

  const int num_modes =
      (disease_ && disease_->numModes() > 0) ? disease_->numModes() : 1;
  const int wire_size = visitorWireSize(num_modes);

  std::vector<int> sdisp, rdisp;
  int stotal, rtotal;
  mpi_utils::computeByteDisplacements(send_counts, wire_size, sdisp, stotal);
  mpi_utils::computeByteDisplacements(recv_counts, wire_size, rdisp, rtotal);

  std::vector<char> sbuf(stotal);
  std::vector<char> rbuf(rtotal);

  for (int r = 0; r < num_ranks_; ++r) {
    char* ptr = sbuf.data() + sdisp[r];
    for (const auto& v : outgoing[r]) {
      ptr = packVisitor(ptr, v, num_modes);
    }
  }

  std::vector<int> sc(num_ranks_), rc(num_ranks_);
  for (int i = 0; i < num_ranks_; ++i) {
    sc[i] = send_counts[i] * wire_size;
    rc[i] = recv_counts[i] * wire_size;
  }

#ifdef JUNE_MPI_DEBUG
  // Performance statistics
  int active_send_pairs = 0;
  int total_visitors_sent = 0;
  for (int c : send_counts_in) {
    if (c > 0) {
      active_send_pairs++;
      total_visitors_sent += c;
    }
  }

  int global_min_send_pairs, global_max_send_pairs, global_total_send_pairs;
  int global_min_visitors, global_max_visitors, global_total_visitors;

  MPI_Reduce(&active_send_pairs, &global_min_send_pairs, 1, MPI_INT, MPI_MIN, 0,
             MPI_COMM_WORLD);
  MPI_Reduce(&active_send_pairs, &global_max_send_pairs, 1, MPI_INT, MPI_MAX, 0,
             MPI_COMM_WORLD);
  MPI_Reduce(&active_send_pairs, &global_total_send_pairs, 1, MPI_INT, MPI_SUM,
             0, MPI_COMM_WORLD);
  MPI_Reduce(&total_visitors_sent, &global_min_visitors, 1, MPI_INT, MPI_MIN, 0,
             MPI_COMM_WORLD);
  MPI_Reduce(&total_visitors_sent, &global_max_visitors, 1, MPI_INT, MPI_MAX, 0,
             MPI_COMM_WORLD);
  MPI_Reduce(&total_visitors_sent, &global_total_visitors, 1, MPI_INT, MPI_SUM,
             0, MPI_COMM_WORLD);

  if (rank_ == 0 && global_total_visitors > 0) {
    double avg_send_pairs = (double)global_total_send_pairs / num_ranks_;
    double avg_visitors = (double)global_total_visitors / num_ranks_;
    double sparsity = 100.0 * avg_send_pairs / num_ranks_;
    double total_mb = (global_total_visitors * wire_size) / (1024.0 * 1024.0);

    std::cout << "      [MPI STATS] All-to-all exchange across " << num_ranks_
              << " ranks:\n"
              << "        Send pairs: min=" << global_min_send_pairs
              << ", max=" << global_max_send_pairs << ", avg=" << std::fixed
              << std::setprecision(1) << avg_send_pairs << "\n"
              << "        Visitors sent: min=" << global_min_visitors
              << ", max=" << global_max_visitors
              << ", avg=" << std::setprecision(0) << avg_visitors << "\n"
              << "        Sparsity: " << std::setprecision(1) << sparsity
              << "% of rank pairs active\n"
              << "        Total bandwidth: " << std::setprecision(2) << total_mb
              << " MB" << std::endl;
  }
#endif

  MPI_Alltoallv(sbuf.data(), sc.data(), sdisp.data(), MPI_BYTE, rbuf.data(),
                rc.data(), rdisp.data(), MPI_BYTE, MPI_COMM_WORLD);

  for (int r = 0; r < num_ranks_; ++r) {
    const char* ptr = rbuf.data() + rdisp[r];
    for (int i = 0; i < recv_counts[r]; ++i) {
      Domain::VisitorData v;
      ptr = unpackVisitor(ptr, v, num_modes);
      if (domain_.ownsVenue(v.venue_id)) domain_.addIncomingVisitor(v);
    }
  }

#ifdef JUNE_MPI_DEBUG
  if (config_.parallel.verbose_mpi) {
    int total_recv = 0, recv_infectious = 0, recv_susceptible = 0;
    for (const auto& v : domain_.incoming_visitors) {
      total_recv++;
      if (v.is_infectious)
        recv_infectious++;
      else if (!v.is_infected)
        recv_susceptible++;
    }
    std::cout << "[MPI_XRANK] Rank " << rank_
              << " RECEIVED (AllToAll): total_visitors=" << total_recv
              << " infectious=" << recv_infectious
              << " susceptible=" << recv_susceptible << std::endl;
  }
#endif
}

void DomainCommunicator::exchangePointToPoint(
    const std::vector<std::vector<Domain::VisitorData>>& outgoing,
    const std::vector<int>& send_counts) {
  std::vector<int> recv_counts(num_ranks_, 0);
  MPI_Alltoall(send_counts.data(), 1, MPI_INT, recv_counts.data(), 1, MPI_INT,
               MPI_COMM_WORLD);

  performP2PVisitorExchange(outgoing, send_counts, recv_counts);
}

void DomainCommunicator::performP2PVisitorExchange(
    const std::vector<std::vector<Domain::VisitorData>>& outgoing,
    const std::vector<int>& send_counts, const std::vector<int>& recv_counts) {
  std::vector<std::vector<char>> sbufs(num_ranks_);
  std::vector<MPI_Request> sreqs, rreqs;
  std::vector<std::vector<char>> rbufs(num_ranks_);

  const int num_modes =
      (disease_ && disease_->numModes() > 0) ? disease_->numModes() : 1;
  const int wire_size = visitorWireSize(num_modes);

  for (int r = 0; r < num_ranks_; ++r) {
    if (r != rank_ && recv_counts[r] > 0) {
      try {
        rbufs[r].resize(recv_counts[r] * wire_size);
      } catch (const std::exception& e) {
        std::cerr << "[MPI] rbufs resize failed: recv_counts[r]="
                  << recv_counts[r] << " error: " << e.what() << std::endl;
        throw;
      }
      MPI_Request req;
      MPI_Irecv(rbufs[r].data(), rbufs[r].size(), MPI_BYTE, r, 101,
                MPI_COMM_WORLD, &req);
      rreqs.push_back(req);
    }
  }

  for (int r = 0; r < num_ranks_; ++r) {
    if (r != rank_ && send_counts[r] > 0) {
      try {
        sbufs[r].resize(send_counts[r] * wire_size);
      } catch (const std::exception& e) {
        std::cerr << "[MPI] sbufs resize failed: send_counts[r]="
                  << send_counts[r] << " error: " << e.what() << std::endl;
        throw;
      }
      char* ptr = sbufs[r].data();
      for (const auto& v : outgoing[r]) {
        ptr = packVisitor(ptr, v, num_modes);
      }
      MPI_Request req;
      MPI_Isend(sbufs[r].data(), sbufs[r].size(), MPI_BYTE, r, 101,
                MPI_COMM_WORLD, &req);
      sreqs.push_back(req);
    }
  }

  if (!rreqs.empty())
    MPI_Waitall(rreqs.size(), rreqs.data(), MPI_STATUSES_IGNORE);

  for (int r = 0; r < num_ranks_; ++r) {
    if (r != rank_ && recv_counts[r] > 0) {
      const char* ptr = rbufs[r].data();
      for (int i = 0; i < recv_counts[r]; ++i) {
        Domain::VisitorData v;
        ptr = unpackVisitor(ptr, v, num_modes);
        if (domain_.ownsVenue(v.venue_id)) domain_.addIncomingVisitor(v);
      }
    }
  }

#ifdef JUNE_MPI_DEBUG
  if (config_.parallel.verbose_mpi) {
    int total_recv = 0, recv_infectious = 0, recv_susceptible = 0;
    for (const auto& v : domain_.incoming_visitors) {
      total_recv++;
      if (v.is_infectious)
        recv_infectious++;
      else if (!v.is_infected)
        recv_susceptible++;
    }
    std::cout << "[MPI_XRANK] Rank " << rank_
              << " RECEIVED (P2P): total_visitors=" << total_recv
              << " infectious=" << recv_infectious
              << " susceptible=" << recv_susceptible << std::endl;
    int recv_examples = 0;
    for (const auto& v : domain_.incoming_visitors) {
      if (recv_examples >= 3) break;
      if (v.is_infectious) {
        std::cout << "  [RECV_EXAMPLE] Rank " << rank_ << " <- Rank "
                  << v.home_rank << ": Person " << v.person_id << " INFECTIOUS"
                  << " venue=" << v.venue_id << std::endl;
        recv_examples++;
      }
    }
  }
#endif

  if (!sreqs.empty())
    MPI_Waitall(sreqs.size(), sreqs.data(), MPI_STATUSES_IGNORE);
}

void DomainCommunicator::exchangePointToPointVerySparse(
    const std::vector<std::vector<Domain::VisitorData>>& outgoing,
    const std::vector<int>& send_counts) {
  std::vector<int> pattern;
  for (int r = 0; r < num_ranks_; ++r) {
    if (send_counts[r] > 0) {
      pattern.push_back(r);
      pattern.push_back(send_counts[r]);
    }
  }

  int sz = static_cast<int>(pattern.size());
  std::vector<int> all_sz(num_ranks_);
  MPI_Allgather(&sz, 1, MPI_INT, all_sz.data(), 1, MPI_INT, MPI_COMM_WORLD);

  std::vector<int> disp(num_ranks_, 0);
  int tot = 0;
  for (int r = 0; r < num_ranks_; ++r) {
    disp[r] = tot;
    tot += all_sz[r];
  }
  std::vector<int> all_pat(tot);
  MPI_Allgatherv(pattern.data(), sz, MPI_INT, all_pat.data(), all_sz.data(),
                 disp.data(), MPI_INT, MPI_COMM_WORLD);

  std::vector<int> recv_counts(num_ranks_, 0);
  for (int src = 0; src < num_ranks_; ++src) {
    for (int i = 0; i < all_sz[src] / 2; ++i) {
      if (all_pat[disp[src] + i * 2] == rank_) {
        recv_counts[src] = all_pat[disp[src] + i * 2 + 1];
      }
    }
  }

  // Call the shared point-to-point logic with reconstructed receive counts
  performP2PVisitorExchange(outgoing, send_counts, recv_counts);
}

std::vector<PendingInfection> DomainCommunicator::receivePendingInfections(
    const std::vector<PendingInfection>& pending) {
  std::vector<std::vector<PendingInfection>> updates;
  std::vector<int> send_counts;
  routePendingByHomeRank(pending, updates, send_counts);

  std::vector<int> recv_counts(num_ranks_, 0);
  MPI_Alltoall(send_counts.data(), 1, MPI_INT, recv_counts.data(), 1, MPI_INT,
               MPI_COMM_WORLD);

  // DEBUG: show received infection updates
  // (printed after Alltoall so we have recv_counts)

  // Preserving metadata
  // 25 bytes: person_id(4) + infector_id(4) + infection_time(8)
  //         + venue_type_id(1) + encounter_type_id(1) + venue_id(4)
  //         + infector_symptom_id(2) + transmission_mode_index(1)
  const int INFECTION_SIZE = 25;
  std::vector<int> sd, rd;
  int stotal, rtotal;
  mpi_utils::computeByteDisplacements(send_counts, INFECTION_SIZE, sd, stotal);
  mpi_utils::computeByteDisplacements(recv_counts, INFECTION_SIZE, rd, rtotal);

  std::vector<char> sbuf(stotal);
  std::vector<char> rbuf(rtotal);

  for (int r = 0; r < num_ranks_; ++r) {
    char* ptr = sbuf.data() + sd[r];
    for (const auto& u : updates[r]) {
      ptr = packField(ptr, u.person_id);
      ptr = packField(ptr, u.infector_id);
      ptr = packField(ptr, u.infection_time);
      ptr = packField(ptr, u.venue_type_id);
      ptr = packField(ptr, u.encounter_type_id);
      ptr = packField(ptr, u.venue_id);
      ptr = packField(ptr, u.infector_symptom_id);
      ptr = packField(ptr, u.transmission_mode_index);
    }
  }

  std::vector<int> sc(num_ranks_), rc(num_ranks_);
  for (int i = 0; i < num_ranks_; ++i) {
    sc[i] = send_counts[i] * INFECTION_SIZE;
    rc[i] = recv_counts[i] * INFECTION_SIZE;
  }
  MPI_Alltoallv(sbuf.data(), sc.data(), sd.data(), MPI_BYTE, rbuf.data(),
                rc.data(), rd.data(), MPI_BYTE, MPI_COMM_WORLD);

  int applied_count = 0;
  int skipped_count = 0;
  std::vector<PendingInfection> newly_infected;
  for (int r = 0; r < num_ranks_; ++r) {
    if (r == rank_) continue;
    const char* ptr = rbuf.data() + rd[r];
    for (int i = 0; i < recv_counts[r]; ++i) {
      PersonId pid;
      PersonId infector_id;
      double t;
      uint8_t v_type;
      uint8_t enc_type_id;
      VenueId v_id;
      uint16_t infector_symptom_id = 0;
      uint8_t transmission_mode_index = 0;
      ptr = unpackField(ptr, pid);
      ptr = unpackField(ptr, infector_id);
      ptr = unpackField(ptr, t);
      ptr = unpackField(ptr, v_type);
      ptr = unpackField(ptr, enc_type_id);
      ptr = unpackField(ptr, v_id);
      ptr = unpackField(ptr, infector_symptom_id);
      ptr = unpackField(ptr, transmission_mode_index);

      auto applied = applyOnePendingInfection(
          pid, infector_id, t, v_type, enc_type_id, v_id, infector_symptom_id,
          transmission_mode_index, r);
      if (applied) {
        newly_infected.push_back(*applied);
        applied_count++;
      } else {
        skipped_count++;
      }
    }
  }

#ifdef JUNE_MPI_DEBUG
  if (config_.parallel.verbose_mpi) {
    int total_recv = 0;
    for (int r = 0; r < num_ranks_; ++r)
      if (r != rank_) total_recv += recv_counts[r];
    std::cout << "[MPI_XRANK] Rank " << rank_
              << " APPLYING pending infections from other ranks:"
              << " received=" << total_recv << " applied=" << applied_count
              << " skipped=" << skipped_count
              << " (already infected or not owned)" << std::endl;
  }
#endif
  return newly_infected;
}

void DomainCommunicator::routePendingByHomeRank(
    const std::vector<PendingInfection>& pending,
    std::vector<std::vector<PendingInfection>>& updates,
    std::vector<int>& send_counts) {
  updates.assign(num_ranks_, {});
  send_counts.assign(num_ranks_, 0);

  for (const auto& p : pending) {
    for (const auto& v : domain_.incoming_visitors) {
      if (v.person_id == p.person_id) {
        updates[v.home_rank].push_back(p);
        send_counts[v.home_rank]++;
        break;
      }
    }
  }

#ifdef JUNE_MPI_DEBUG
  if (config_.parallel.verbose_mpi) {
    int total_pending_out = 0;
    for (int r = 0; r < num_ranks_; ++r) total_pending_out += send_counts[r];
    std::cout << "[MPI_XRANK] Rank " << rank_
              << " SENDING pending infections to home ranks: count="
              << total_pending_out << std::endl;
  }
#endif
}

std::optional<PendingInfection> DomainCommunicator::applyOnePendingInfection(
    PersonId pid, PersonId infector_id, double t, uint8_t v_type,
    uint8_t enc_type_id, VenueId v_id, uint16_t infector_symptom_id,
    uint8_t transmission_mode_index, int from_rank) {
  Person* person = world_.getPerson(pid);
  if (!person || person->infection || !domain_.ownsPerson(pid) || !disease_) {
#ifdef JUNE_MPI_DEBUG
    if (person && person->infection && config_.parallel.verbose_mpi) {
      std::cout << "  [CROSS_RANK_SKIP] Rank " << rank_ << ": Person " << pid
                << " already infected, skipping" << std::endl;
    }
#endif
    (void)from_rank;
    return std::nullopt;
  }

  std::string venue_type_name = "";
  if (v_type < world_.venue_type_names.size()) {
    venue_type_name = world_.venue_type_names[v_type];
  }

  float severity_factor = 1.0f;
  auto* gu = world_.getGeoUnit(person->geo_unit_id);
  if (gu) severity_factor = gu->severity_factor;

  // venue_key consistent with the local infection path
  // (interaction_manager.cpp): for virtual venues (id <= -1000), extract
  // the host's person_id so the infection seed is deterministic
  // regardless of which rank creates it.
  uint64_t venue_key = static_cast<uint64_t>(v_id);
  if (v_id <= -1000) {
    venue_key = static_cast<uint64_t>(-static_cast<int64_t>(v_id) - 1000);
  }
  uint64_t infection_seed =
      mix_seed(config_.simulation.random_seed, pid,
               static_cast<uint64_t>(t * 1000), venue_key);
  person->infection = std::make_unique<Infection>(
      disease_, t, person, static_cast<unsigned int>(infection_seed), &world_,
      venue_type_name, v_id, severity_factor, infector_symptom_id, "", "",
      transmission_mode_index);

  PendingInfection applied;
  applied.person_id = pid;
  applied.infector_id = infector_id;
  applied.infection_time = t;
  applied.venue_type_id = v_type;
  applied.encounter_type_id = enc_type_id;
  applied.venue_id = v_id;
  applied.infector_symptom_id = infector_symptom_id;
  applied.transmission_mode_index = transmission_mode_index;

#ifdef JUNE_MPI_DEBUG
  if (config_.parallel.verbose_mpi) {
    std::cout << "  [CROSS_RANK_INFECTION_APPLIED] Rank " << rank_
              << ": Person " << pid << " (age=" << person->age
              << " geo=" << person->geo_unit_id << ")"
              << " INFECTED at remote venue " << v_id
              << " (type=" << venue_type_name << ")"
              << " from rank " << from_rank
              << " infector_symptom=" << infector_symptom_id
              << " mode=" << (int)transmission_mode_index
              << " infection_time=" << t << std::endl;
  }
#else
  (void)from_rank;
#endif
  return applied;
}

// =============================================================================
// Cross-rank coordinated encounter exchange
// =============================================================================

void DomainCommunicator::exchangeEncounterProposals(
    const std::vector<EncounterProposal>& local_proposals,
    const DomainManager& dm,
    std::vector<EncounterProposal>& proposals_for_this_rank) {
  const size_t num_enc_types = world_.encounter_type_names.size();
  const int num_ranks_capture = num_ranks_;

  std::vector<int> send_counts, recv_counts;
  exchangeRoutedRecords<EncounterProposal>(
      local_proposals, proposals_for_this_rank, rank_, num_ranks_,
      PROPOSAL_WIRE_SIZE, "proposal",
      /*before_abort=*/110, /*total_neg_abort=*/103,
      /*pack_abort=*/111, /*unpack_abort=*/112, packProposal, unpackProposal,
      [num_ranks_capture, num_enc_types](const EncounterProposal& p) {
        return validateProposal(p, num_ranks_capture, num_enc_types);
      },
      dumpProposal,
      [&dm](const EncounterProposal& p) {
        return dm.getPersonRank(p.invitee_id);
      },
      send_counts, recv_counts);

#ifdef JUNE_MPI_DEBUG
  if (config_.parallel.verbose_mpi) {
    int total_sent = 0;
    for (int r = 0; r < num_ranks_; ++r)
      if (r != rank_) total_sent += send_counts[r];
    int total_received = 0;
    for (int r = 0; r < num_ranks_; ++r)
      if (r != rank_) total_received += recv_counts[r];
    std::cout << "[MPI_ENC] Rank " << rank_
              << " proposal exchange: sent=" << total_sent
              << " received=" << total_received
              << " local=" << proposals_for_this_rank.size() << std::endl;
  }
#endif
}

void DomainCommunicator::exchangeEncounterReplies(
    const std::vector<EncounterReply>& local_replies, const DomainManager& dm,
    std::vector<EncounterReply>& replies_for_this_rank) {
  const size_t num_enc_types = world_.encounter_type_names.size();

  std::vector<int> send_counts, recv_counts;
  exchangeRoutedRecords<EncounterReply>(
      local_replies, replies_for_this_rank, rank_, num_ranks_, REPLY_WIRE_SIZE,
      "reply",
      /*before_abort=*/120, /*total_neg_abort=*/104,
      /*pack_abort=*/121, /*unpack_abort=*/122, packReply, unpackReply,
      [num_enc_types](const EncounterReply& r) {
        return validateReply(r, num_enc_types);
      },
      dumpReply,
      [&dm](const EncounterReply& r) { return dm.getPersonRank(r.host_id); },
      send_counts, recv_counts);

#ifdef JUNE_MPI_DEBUG
  if (config_.parallel.verbose_mpi) {
    int total_sent = 0, total_received = 0;
    int accepted = 0, rejected = 0;
    for (int r = 0; r < num_ranks_; ++r)
      if (r != rank_) total_sent += send_counts[r];
    for (int r = 0; r < num_ranks_; ++r)
      if (r != rank_) total_received += recv_counts[r];
    for (const auto& r : replies_for_this_rank) {
      if (r.status == ReplyStatus::ACCEPTED)
        accepted++;
      else
        rejected++;
    }
    std::cout << "[MPI_ENC] Rank " << rank_
              << " reply exchange: sent=" << total_sent
              << " received=" << total_received
              << " total_replies=" << replies_for_this_rank.size()
              << " (accepted=" << accepted << " rejected=" << rejected << ")"
              << std::endl;
  }
#endif
}

void DomainCommunicator::exchangeFinalizedEncounters(
    const std::vector<CoordinatedEncounter>& local_finalized,
    const DomainManager& dm,
    std::vector<CoordinatedEncounter>& finalized_for_this_rank) {
  // Use Allgatherv: each rank broadcasts its finalized encounters.
  // Other ranks filter for encounters containing their local people.
  // Variable-length serialization: header(25 bytes) + participant_count * 4

  // First, serialize local finalized encounters to a byte buffer
  std::vector<char> local_buf;
  for (const auto& enc : local_finalized) {
    int participant_count = static_cast<int>(enc.participants.size());
    size_t entry_size = 25 + participant_count * 4;
    size_t offset = local_buf.size();
    local_buf.resize(offset + entry_size);
    char* ptr = local_buf.data() + offset;

    ptr = packField(ptr, enc.encounter_id);
    ptr = packField(ptr, enc.host_id);
    ptr = packField(ptr, enc.venue_id);
    ptr = packField(ptr, enc.venue_type_id);
    ptr = packField(ptr, enc.slot);
    ptr = packField(ptr, enc.encounter_type_id);
    ptr = packField(ptr, participant_count);
    for (PersonId pid : enc.participants) {
      ptr = packField(ptr, pid);
    }
  }

  int local_size = static_cast<int>(local_buf.size());
  std::vector<int> all_sizes(num_ranks_);
  MPI_Allgather(&local_size, 1, MPI_INT, all_sizes.data(), 1, MPI_INT,
                MPI_COMM_WORLD);

  std::vector<int> displs(num_ranks_, 0);
  int total_size = 0;
  for (int r = 0; r < num_ranks_; ++r) {
    displs[r] = total_size;
    total_size += all_sizes[r];
  }

  std::vector<char> all_buf(total_size);
  MPI_Allgatherv(local_buf.data(), local_size, MPI_BYTE, all_buf.data(),
                 all_sizes.data(), displs.data(), MPI_BYTE, MPI_COMM_WORLD);

  // Deserialize and filter: keep encounters with at least one local participant
  finalized_for_this_rank.clear();
  for (int r = 0; r < num_ranks_; ++r) {
    if (r == rank_) continue;  // Skip our own (already have them)
    const char* ptr = all_buf.data() + displs[r];
    const char* end = ptr + all_sizes[r];
    while (ptr < end) {
      CoordinatedEncounter enc;
      ptr = unpackField(ptr, enc.encounter_id);
      ptr = unpackField(ptr, enc.host_id);
      ptr = unpackField(ptr, enc.venue_id);
      ptr = unpackField(ptr, enc.venue_type_id);
      ptr = unpackField(ptr, enc.slot);
      ptr = unpackField(ptr, enc.encounter_type_id);
      int participant_count;
      ptr = unpackField(ptr, participant_count);
      bool has_local = false;
      for (int i = 0; i < participant_count; ++i) {
        PersonId pid;
        ptr = unpackField(ptr, pid);
        enc.participants.insert(pid);
        if (dm.getPersonRank(pid) == rank_) has_local = true;
      }
      if (has_local) {
        finalized_for_this_rank.push_back(enc);
      }
    }
  }

#ifdef JUNE_MPI_DEBUG
  if (config_.parallel.verbose_mpi) {
    int cross_rank_participants = 0;
    for (const auto& enc : finalized_for_this_rank) {
      for (PersonId pid : enc.participants) {
        if (dm.getPersonRank(pid) == rank_) cross_rank_participants++;
      }
    }
    std::cout << "[MPI_ENC] Rank " << rank_
              << " finalized encounter exchange: local_finalized="
              << local_finalized.size()
              << " remote_relevant=" << finalized_for_this_rank.size()
              << " local_participants_in_remote=" << cross_rank_participants
              << std::endl;
  }
#endif
}

}  // namespace june

#endif  // USE_MPI
