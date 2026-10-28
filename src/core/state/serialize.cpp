#include "cereal/archives/binary.hpp"
#include "cereal/types/map.hpp"
#include "cereal/types/vector.hpp"
#include "fd_manager.h"
#include "fsstate.h"
#include "futexstate.h"
#include "sockstate.h"
#include "state/state.h"
#include "log_wrapper.h"
#include <array>
#include <cstring>

template <class Archive>
void syscall_info::serialize(Archive &ar)
{
  ar(nr, rval, args);
}

namespace {
template<class Archive, size_t Count>
void archive_syscall_array(Archive& ar, syscall_info (&calls)[Count])
{
  // Preserve the existing member-wise wire bytes, excluding struct padding.
  constexpr size_t record_bytes = sizeof(calls[0].nr) + sizeof(calls[0].rval) +
                                  sizeof(calls[0].args);
  std::array<unsigned char,Count*record_bytes> wire{};
  if constexpr (Archive::is_saving::value) {
    auto* out=wire.data();
    for (const auto& call:calls) {
      std::memcpy(out,&call.nr,sizeof(call.nr)); out+=sizeof(call.nr);
      std::memcpy(out,&call.rval,sizeof(call.rval)); out+=sizeof(call.rval);
      std::memcpy(out,call.args,sizeof(call.args)); out+=sizeof(call.args);
    }
  }
  ar(cereal::binary_data(wire.data(),wire.size()));
  if constexpr (Archive::is_loading::value) {
    const auto* in=wire.data();
    for (auto& call:calls) {
      std::memcpy(&call.nr,in,sizeof(call.nr)); in+=sizeof(call.nr);
      std::memcpy(&call.rval,in,sizeof(call.rval)); in+=sizeof(call.rval);
      std::memcpy(call.args,in,sizeof(call.args)); in+=sizeof(call.args);
    }
  }
}
}

template <class Archive>
void tracee_state::serialize(Archive &ar)
{
  bool has_futex = futex_state.has_value();
  ar(has_futex);
  if constexpr (Archive::is_loading::value) {
    if (has_futex) futex_state.emplace();
    else futex_state.reset();
  }
  if (has_futex) ar(*futex_state);

  if (threads.size() > 10000)
    throw std::runtime_error("Thread archive exceeds supported limit");

  archive_syscall_array(ar,si);
  ar(brk, tv.tv_sec, tv.tv_usec, fs_state, sock_state, process_state, fd_manager_state,
     threads, thread_create_records, main_tid, current_thread_idx);
}

template <class Archive>
void sys_state::serialize(Archive &ar)
{
  uint32_t magic = 0x4453534d, version = 2, nodes = NP;
  ar(magic, version, nodes);
  if (magic != 0x4453534d || version != 2 || nodes != NP)
    throw std::runtime_error("Incompatible system-state archive");
  /* The model budget is a per-run statistic with no semantic effect, so it is
   * deliberately excluded from the archived identity: otherwise identical
   * states reached after different numbers of approximations would never
   * deduplicate. */
  ar(ts_hash, status, property_failed);
  // Check lengths before allocation; monitor data belongs to a search branch.
  constexpr uint64_t limit = 64ULL * 1024 * 1024;
  auto bytes = [&](auto& value, uint64_t maximum) {
    uint64_t length = value.size();
    ar(length);
    if (length > maximum) throw std::runtime_error("Monitor archive exceeds limit");
    if constexpr (Archive::is_loading::value) value.resize(length);
    if (length) ar(cereal::binary_data(value.data(), length));
  };
  bytes(model_identity, 4096);
  /* One opaque UserState blob per logical node, in fixed node order. */
  uint64_t user_sizes[NP] = {};
  for (int node = 0; node < NP; ++node) user_sizes[node] = user_state[node].size();
  ar(cereal::binary_data(user_sizes, sizeof(user_sizes)));
  uint64_t user_total = 0;
  for (int node = 0; node < NP; ++node) {
    if (user_sizes[node] > limit) throw std::runtime_error("User state exceeds limit");
    user_total += user_sizes[node];
    if (user_total > limit) throw std::runtime_error("User state exceeds limit");
  }
  if (user_total) {
    for (int node = 0; node < NP; ++node) {
      if constexpr (Archive::is_loading::value) user_state[node].resize(user_sizes[node]);
      if (user_sizes[node])
        ar(cereal::binary_data(user_state[node].data(), user_sizes[node]));
    }
  }
}

template void syscall_info::serialize<cereal::BinaryInputArchive>(
    cereal::BinaryInputArchive &);
template void syscall_info::serialize<cereal::BinaryOutputArchive>(
    cereal::BinaryOutputArchive &);
template void tracee_state::serialize<cereal::BinaryInputArchive>(
    cereal::BinaryInputArchive &);
template void tracee_state::serialize<cereal::BinaryOutputArchive>(
    cereal::BinaryOutputArchive &);
template void
sys_state::serialize<cereal::BinaryInputArchive>(cereal::BinaryInputArchive &);
template void sys_state::serialize<cereal::BinaryOutputArchive>(
    cereal::BinaryOutputArchive &);
