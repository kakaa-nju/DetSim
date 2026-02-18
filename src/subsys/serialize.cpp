#include "cereal/archives/binary.hpp"
#include "cereal/types/deque.hpp"
#include "cereal/types/list.hpp"
#include "cereal/types/string.hpp"
#include "cereal/types/unordered_map.hpp"
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
  ar(si, pid, brk, tv.tv_sec, tv.tv_usec, fs_state, sock_list, tcp_buffer_list,
     udp_buffer_list);
}

template <class Archive>
void sys_state::serialize(Archive &ar)
{
  ar(ts_hash, exited);
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

template <class Archive>
void tcp_buffer::save(Archive &ar) const
{
  ar(ss.str());
}

template <class Archive>
void tcp_buffer::load(Archive &ar)
{
  std::string s;
  ar(s);
  ss.str("");  // Clear existing content
  ss << s;
}

template <class Archive>
void ptmc_datagram::serialize(Archive &ar)
{
  ar(content, from);
}

template <class Archive>
void ptmc_sock::serialize(Archive &ar)
{
  ar(fd, domain, type, protocol, backlog, addr, dest);
}

template void ptmc_datagram::serialize<cereal::BinaryInputArchive>(
    cereal::BinaryInputArchive &);
template void ptmc_datagram::serialize<cereal::BinaryOutputArchive>(
    cereal::BinaryOutputArchive &);
template void
ptmc_sock::serialize<cereal::BinaryInputArchive>(cereal::BinaryInputArchive &);
template void ptmc_sock::serialize<cereal::BinaryOutputArchive>(
    cereal::BinaryOutputArchive &);
template void
tcp_buffer::load<cereal::BinaryInputArchive>(cereal::BinaryInputArchive &);
template void tcp_buffer::save<cereal::BinaryOutputArchive>(
    cereal::BinaryOutputArchive &) const;

