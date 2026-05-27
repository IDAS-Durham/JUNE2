#pragma once

#ifdef USE_MPI

#include <cstring>
#include <type_traits>

// Field-level pack/unpack used by both translation units of
// DomainCommunicator (the visitor-phase exchanges in
// src/parallel/domain_communicator.cpp and the post-interaction-phase
// exchanges in src/parallel/domain_communicator_encounters.cpp). The
// helpers copy sizeof(T) bytes and advance the cursor; sender and
// receiver must agree on field order and types. The wire size of a
// record is the sum of sizeof(field) across its packField calls.
namespace june::domain_comm_detail {

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

}  // namespace june::domain_comm_detail

#endif  // USE_MPI
