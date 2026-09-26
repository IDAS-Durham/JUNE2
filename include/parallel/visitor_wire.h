#pragma once

#ifdef USE_MPI

#include <utility>
#include <vector>

#include "parallel/domain.h"

// Wire format of one Visitor record: a fixed header (WireRecord over
// VisitorData's plain fields) followed by four count-known-elsewhere tails:
// emission.infectiousness_by_mode, target_susceptibility,
// deposition_source_multiplier, emission.fomite_deposits. Source tails travel
// only when the Visitor is infected, and arrive empty otherwise;
// target_susceptibility always travels, since any visitor can be a target:
//
//   !is_infected   header + ts
//   is_infected    header + ii + ts + dsm + deposits
//
// The receiver derives nothing from disease state.
namespace june::visitor_wire {

// Lengths of a visitor record's tails. Derived from the Disease and timestep,
// so identical on every rank and fixed for one exchange.
struct TailCounts {
  int num_modes;             // infectiousness_by_mode, target_susceptibility
  int num_deposition_modes;  // deposition_source_multiplier
  int fomite_sub_bins;       // emission.fomite_deposits
};

// Bytes `visitor` occupies on the wire; depends on its header bools.
int recordSize(const Domain::VisitorData& visitor, const TailCounts& tails);

// Writes `visitor` at `ptr`, returns the end of the record. Throws if a sent
// tail's length differs from its count in `tails`, or if a skipped tail is
// not empty or all zero.
char* pack(char* ptr, const Domain::VisitorData& visitor,
           const TailCounts& tails);

// Reads one record at `ptr` into `visitor`, returns the end of the record.
// Skipped tails come back empty.
const char* unpack(const char* ptr, Domain::VisitorData& visitor,
                   const TailCounts& tails);

// Bytes `visitors` occupy on the wire, packed back to back.
int sliceSize(const std::vector<Domain::VisitorData>& visitors,
              const TailCounts& tails);

namespace detail {
// unpack(), but throws if the record would run past `end`.
const char* unpackWithin(const char* ptr, const char* end,
                         Domain::VisitorData& visitor, const TailCounts& tails);
}  // namespace detail

// Unpacks every record in [begin, end), passing each to `sink` as an rvalue.
// Throws if the records don't end exactly at `end`.
template <typename Sink>
void unpackSlice(const char* begin, const char* end, const TailCounts& tails,
                 Sink&& sink) {
  const char* ptr = begin;
  while (ptr < end) {
    Domain::VisitorData visitor;
    ptr = detail::unpackWithin(ptr, end, visitor, tails);
    sink(std::move(visitor));
  }
}

}  // namespace june::visitor_wire

#endif  // USE_MPI
