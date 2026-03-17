#include "cereal/archives/binary.hpp"
#include "cereal/types/deque.hpp"
#include "cereal/types/list.hpp"
#include "cereal/types/map.hpp"
#include "cereal/types/memory.hpp"
#include "cereal/types/string.hpp"
#include "cereal/types/unordered_map.hpp"
#include "cereal/types/vector.hpp"
#include "fd_manager.h"
#include "fsstate.h"
#include "sockstate.h"
#include "state.h"

template <class Archive>
void syscall_info::serialize(Archive &ar)
{
  ar(nr, rval, args);
}

template <class Archive>
void tracee_state::serialize(Archive &ar)
{
  ar(si, pid, brk, tv.tv_sec, tv.tv_usec, fs_state, sock_state, raft_state, fd_manager_state);
}

template <class Archive>
void sys_state::serialize(Archive &ar)
{
  ar(ts_hash, status);
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

/* Old structures' serialization is now inline in sockstate.h */

/* New SockState serialization - explicit instantiations are in sockstate_new.cpp */

