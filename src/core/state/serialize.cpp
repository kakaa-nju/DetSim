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
  ar(ts_hash, status, error_bound, property_failed);
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
  uint32_t count = monitor_states.size();
  ar(count);
  if (count > 1024) throw std::runtime_error("Too many monitor states");
  if constexpr (Archive::is_loading::value) monitor_states.clear();
  uint64_t remaining = limit;
  if constexpr (Archive::is_loading::value) {
    for (uint32_t i = 0; i < count; ++i) {
      std::string key; std::vector<uint8_t> value;
      bytes(key, 4096); bytes(value, remaining); remaining -= value.size();
      if (key.empty() || !monitor_states.emplace(key, std::move(value)).second)
        throw std::runtime_error("Invalid duplicate monitor identity");
    }
  } else {
    for (auto& [name, value] : monitor_states) {
      auto key = name;
      if (key.empty()) throw std::runtime_error("Empty monitor identity");
      bytes(key, 4096); bytes(value, remaining); remaining -= value.size();
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
