/*
 * sockstate.cpp - Socket state implementation
 */

#include "sockstate.h"
#include "cereal/archives/binary.hpp"
#include "cereal/types/deque.hpp"
#include "cereal/types/map.hpp"
#include "cereal/types/string.hpp"
#include "cereal/types/unordered_map.hpp"
#include "cereal/types/vector.hpp"
#include "debug.h"
#include "monitor.h"
#include <arpa/inet.h>
#include <cstring>
#include <fmt/format.h>

/* ==============================================================================
 * Utility Functions
 * ==============================================================================
 */

std::string format_sockaddr(const struct sockaddr_in &addr)
{
  char ip[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
  return fmt::format("{}:{}", ip, ntohs(addr.sin_port));
}

bool parse_sockaddr(const std::string &str, struct sockaddr_in &addr)
{
  size_t colon = str.find(':');
  if (colon == std::string::npos)
    return false;

  std::string ip_str = str.substr(0, colon);
  int port = std::stoi(str.substr(colon + 1));

  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  return inet_pton(AF_INET, ip_str.c_str(), &addr.sin_addr) == 1;
}

/* ==============================================================================
 * SockState Private Helpers
 * ==============================================================================
 */

int SockState::allocate_fd()
{
  if (!fd_manager_)
  {
    LOG_ERROR("FdManager not set!");
    return -1;
  }
  return fd_manager_->allocate_fd(FdType::SOCKET);
}

void SockState::release_fd(int fd)
{
  if (fd_manager_)
  {
    fd_manager_->release_fd(fd);
  }
}

Socket *SockState::get_socket_mutable(int fd)
{
  auto it = sockets_.find(fd);
  if (it != sockets_.end())
    return &it->second;
  return nullptr;
}

int SockState::find_socket_by_local_addr(const struct sockaddr_in &addr) const
{
  for (const auto &kv : sockets_)
  {
    const Socket &sock = kv.second;
    if (sock.bound && sock.local_addr.sin_addr.s_addr == addr.sin_addr.s_addr &&
        sock.local_addr.sin_port == addr.sin_port)
    {
      return kv.first;
    }
  }
  return -1;
}

int SockState::find_socket_by_port(uint16_t port) const
{
  for (const auto &kv : sockets_)
  {
    const Socket &sock = kv.second;
    if (sock.bound && sock.local_addr.sin_port == port)
    {
      return kv.first;
    }
  }
  return -1;
}

/* ==============================================================================
 * SockState Public API - Socket Operations
 * ==============================================================================
 */

int SockState::do_socket(int domain, int type, int protocol)
{
  if (domain != AF_INET)
  {
    LOG_ERROR("socket: unsupported domain %d", domain);
    return -EAFNOSUPPORT;
  }

  if (type != SOCK_STREAM && type != SOCK_DGRAM)
  {
    LOG_ERROR("socket: unsupported type %d", type);
    return -EPROTONOSUPPORT;
  }

  int fd = allocate_fd();
  if (fd < 0)
  {
    return -EMFILE;
  }

  Socket sock;
  sock.fd = fd;
  sock.domain = domain;
  sock.type = type;
  sock.protocol = protocol;
  sock.bound = false;
  sock.connected = false;
  sock.listening = false;

  sockets_[fd] = sock;

  LOG_TRACE("socket(%d, %d, %d) -> fd=%d", domain, type, protocol, fd);
  return fd;
}

int SockState::do_bind(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
  Socket *sock = get_socket_mutable(fd);
  if (!sock)
  {
    return -EBADF;
  }

  if (sock->bound)
  {
    return -EINVAL;
  }

  if (addrlen < sizeof(struct sockaddr_in))
  {
    return -EINVAL;
  }

  /* addr is guest pointer, need to read from guest memory */
  struct sockaddr_in sin;
  memcpy_guest2host(&sin, addr, sizeof(struct sockaddr_in));
  memcpy(&sock->local_addr, &sin, sizeof(struct sockaddr_in));
  sock->bound = true;

  LOG_TRACE("bind(fd=%d, %s) = 0", fd, format_sockaddr(sin).c_str());
  return 0;
}

int SockState::do_listen(int fd, int backlog)
{
  Socket *sock = get_socket_mutable(fd);
  if (!sock)
  {
    return -EBADF;
  }

  if (sock->type != SOCK_STREAM)
  {
    return -EOPNOTSUPP;
  }

  if (!sock->bound)
  {
    return -EDESTADDRREQ;
  }

  sock->listening = true;
  sock->backlog = backlog;

  LOG_TRACE("listen(fd=%d, backlog=%d) = 0", fd, backlog);
  return 0;
}

int SockState::do_connect(int fd, const struct sockaddr *addr,
                          socklen_t addrlen)
{
  LOG_WARN("connect() not fully implemented");
  return -ECONNREFUSED;
}

int SockState::do_accept(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
  LOG_WARN("accept() not fully implemented");
  return -EAGAIN;
}

/* ==============================================================================
 * SockState Public API - Data Transfer (UDP)
 * ==============================================================================
 */

ssize_t SockState::do_sendto(int fd, const void *buf, size_t len, int flags,
                             const struct sockaddr *dest_addr,
                             socklen_t addrlen)
{
  Socket *sock = get_socket_mutable(fd);
  if (!sock)
  {
    return -EBADF;
  }

  if (sock->type != SOCK_DGRAM)
  {
    return -EOPNOTSUPP;
  }

  if (!dest_addr && !sock->connected)
  {
    return -EDESTADDRREQ;
  }

  char *data = (char *)malloc(len);
  if (!data)
  {
    return -ENOMEM;
  }
  memcpy_guest2host(data, buf, len);

  struct sockaddr_in dest;
  if (dest_addr)
  {
    /* dest_addr is guest pointer, need to read from guest memory */
    memcpy_guest2host(&dest, dest_addr, sizeof(struct sockaddr_in));
  }
  else
  {
    dest = sock->peer_addr;
  }

  struct sockaddr_in from;
  memset(&from, 0, sizeof(from));
  from.sin_family = AF_INET;
  from.sin_port = sock->bound ? sock->local_addr.sin_port : 0;

  UdpDatagram dg;
  dg.content = std::string(data, len);
  dg.from = from;
  free(data);

  LOG_TRACE("sendto(fd=%d, len=%zu) to %s", fd, len,
            format_sockaddr(dest).c_str());

  return len;
}

ssize_t SockState::do_recvfrom(int fd, void *buf, size_t len, int flags,
                               struct sockaddr *src_addr, socklen_t *addrlen)
{
  // Default: use choice 0 (receive first message)
  return do_recvfrom_with_choice(fd, buf, len, flags, src_addr, addrlen, 0);
}

ssize_t SockState::do_recvfrom_with_choice(int fd, void *buf, size_t len,
                                           int flags, struct sockaddr *src_addr,
                                           socklen_t *addrlen, int choice)
{
  Socket *sock = get_socket_mutable(fd);
  if (!sock)
  {
    return -EBADF;
  }

  if (sock->type != SOCK_DGRAM)
  {
    return -EOPNOTSUPP;
  }

  auto it = udp_recv_buffers_.find(fd);
  if (it == udp_recv_buffers_.end() || it->second.empty())
  {
    return -EAGAIN;
  }

  // Validate choice
  if (choice < 0 || choice >= static_cast<int>(it->second.size()))
  {
    return -EAGAIN;
  }

  // Have only one datagram but postpone
  if (choice == 1 && static_cast<int>(it->second.size()) == 1)
  {
    return -EAGAIN;
  }

  // Get iterator to chosen datagram
  auto dg_it = it->second.begin();
  if (choice == 1 && it->second.size() >= 2)
  {
    // Choice 1: receive second message (for message reordering)
    ++dg_it;
  }

  UdpDatagram &dg = *dg_it;

  /* Copy data to guest buffer */
  size_t copy_len = std::min(len, dg.content.size());
  memcpy_host2guest(buf, dg.content.c_str(), copy_len);

  /* Handle output parameters - src_addr and addrlen are guest pointers */
  if (src_addr && addrlen)
  {
    /* Read input addrlen value from guest */
    socklen_t guest_addrlen = 0;
    memcpy_guest2host(&guest_addrlen, addrlen, sizeof(socklen_t));

    /* Calculate actual address length to write */
    socklen_t addr_len =
        std::min(guest_addrlen, (socklen_t)sizeof(struct sockaddr_in));

    /* Write source address to guest */
    memcpy_host2guest(src_addr, &dg.from, addr_len);

    /* Write actual address length back to guest */
    memcpy_host2guest(addrlen, &addr_len, sizeof(socklen_t));
  }

  // Remove the received datagram from queue
  it->second.erase(dg_it);

  LOG_TRACE("recvfrom(fd=%d, choice=%d) -> len=%zu from %s", fd, choice,
            copy_len, format_sockaddr(dg.from).c_str());

  return copy_len;
}

int SockState::get_recvfrom_choices(int fd) const
{
  auto it = udp_recv_buffers_.find(fd);
  if (it == udp_recv_buffers_.end() || it->second.empty())
  {
    return 0; // No messages available
  }

  if (it->second.size() >= 2)
  {
    return 2; // 2 choices: receive first or second message
  }

  return 2; // Only 1 choice: receive the only message or lag
}

/* ==============================================================================
 * SockState Public API - Lifecycle
 * ==============================================================================
 */

int SockState::do_close(int fd)
{
  auto it = sockets_.find(fd);
  if (it == sockets_.end())
  {
    return -EBADF;
  }

  udp_recv_buffers_.erase(fd);
  tcp_connections_.erase(fd);
  sockets_.erase(it);
  release_fd(fd);

  LOG_TRACE("close(fd=%d) = 0", fd);
  return 0;
}

int SockState::do_getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
  const Socket *sock = get_socket(fd);
  if (!sock)
  {
    return -EBADF;
  }

  if (!sock->bound)
  {
    return -EINVAL;
  }

  if (addr && addrlen)
  {
    /* Read input addrlen from guest */
    socklen_t guest_addrlen = 0;
    memcpy_guest2host(&guest_addrlen, addrlen, sizeof(socklen_t));

    socklen_t len =
        std::min(guest_addrlen, (socklen_t)sizeof(struct sockaddr_in));
    memcpy_host2guest(addr, &sock->local_addr, len);
    memcpy_host2guest(addrlen, &len, sizeof(socklen_t));
  }

  return 0;
}

int SockState::do_getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
  const Socket *sock = get_socket(fd);
  if (!sock)
  {
    return -EBADF;
  }

  if (!sock->connected)
  {
    return -ENOTCONN;
  }

  if (addr && addrlen)
  {
    /* Read input addrlen from guest */
    socklen_t guest_addrlen = 0;
    memcpy_guest2host(&guest_addrlen, addrlen, sizeof(socklen_t));

    socklen_t len =
        std::min(guest_addrlen, (socklen_t)sizeof(struct sockaddr_in));
    memcpy_host2guest(addr, &sock->peer_addr, len);
    memcpy_host2guest(addrlen, &len, sizeof(socklen_t));
  }

  return 0;
}

/* ==============================================================================
 * SockState Public API - Cross-Process Delivery
 * ==============================================================================
 */

void SockState::deliver_udp_datagram(int recv_fd, const UdpDatagram &dg)
{
  udp_recv_buffers_[recv_fd].push_back(dg);

  const size_t MAX_UDP_BUFFER = 100;
  if (udp_recv_buffers_[recv_fd].size() > MAX_UDP_BUFFER)
  {
    udp_recv_buffers_[recv_fd].pop_front();
    LOG_WARN("UDP buffer overflow for fd=%d", recv_fd);
  }
}

/* ==============================================================================
 * SockState Public API - Queries
 * ==============================================================================
 */

bool SockState::is_valid_socket(int fd) const { return sockets_.count(fd) > 0; }

const Socket *SockState::get_socket(int fd) const
{
  auto it = sockets_.find(fd);
  if (it != sockets_.end())
    return &it->second;
  return nullptr;
}

size_t SockState::get_udp_buffer_size(int fd) const
{
  auto it = udp_recv_buffers_.find(fd);
  if (it != udp_recv_buffers_.end())
    return it->second.size();
  return 0;
}

/* ==============================================================================
 * Serialization Implementations
 * ==============================================================================
 */

template <class Archive>
void Socket::serialize(Archive &ar)
{
  sa_family_t local_family = local_addr.sin_family;
  in_port_t local_port = local_addr.sin_port;
  uint32_t local_addr_val = local_addr.sin_addr.s_addr;

  sa_family_t peer_family = peer_addr.sin_family;
  in_port_t peer_port = peer_addr.sin_port;
  uint32_t peer_addr_val = peer_addr.sin_addr.s_addr;

  ar(fd, domain, type, protocol, bound, local_family, local_port,
     local_addr_val, connected, listening, peer_family, peer_port,
     peer_addr_val, backlog, pending_connections);

  local_addr.sin_family = local_family;
  local_addr.sin_port = local_port;
  local_addr.sin_addr.s_addr = local_addr_val;

  peer_addr.sin_family = peer_family;
  peer_addr.sin_port = peer_port;
  peer_addr.sin_addr.s_addr = peer_addr_val;
}

template <class Archive>
void UdpDatagram::serialize(Archive &ar)
{
  sa_family_t family = from.sin_family;
  in_port_t port = from.sin_port;
  uint32_t addr_val = from.sin_addr.s_addr;

  ar(content, family, port, addr_val);

  from.sin_family = family;
  from.sin_port = port;
  from.sin_addr.s_addr = addr_val;
}

template <class Archive>
void TcpConnection::serialize(Archive &ar)
{
  sa_family_t l_family = local_addr.sin_family;
  in_port_t l_port = local_addr.sin_port;
  uint32_t l_addr = local_addr.sin_addr.s_addr;

  sa_family_t p_family = peer_addr.sin_family;
  in_port_t p_port = peer_addr.sin_port;
  uint32_t p_addr = peer_addr.sin_addr.s_addr;

  ar(local_fd, peer_fd, peer_pid, l_family, l_port, l_addr, p_family, p_port,
     p_addr, send_buffer, recv_buffer);

  local_addr.sin_family = l_family;
  local_addr.sin_port = l_port;
  local_addr.sin_addr.s_addr = l_addr;

  peer_addr.sin_family = p_family;
  peer_addr.sin_port = p_port;
  peer_addr.sin_addr.s_addr = p_addr;
}

template <class Archive>
void SockState::serialize(Archive &ar)
{
  ar(sockets_, udp_recv_buffers_, tcp_connections_);
}

/* Explicit instantiations */
template void
Socket::serialize<cereal::BinaryInputArchive>(cereal::BinaryInputArchive &);
template void
Socket::serialize<cereal::BinaryOutputArchive>(cereal::BinaryOutputArchive &);
template void UdpDatagram::serialize<cereal::BinaryInputArchive>(
    cereal::BinaryInputArchive &);
template void UdpDatagram::serialize<cereal::BinaryOutputArchive>(
    cereal::BinaryOutputArchive &);
template void TcpConnection::serialize<cereal::BinaryInputArchive>(
    cereal::BinaryInputArchive &);
template void TcpConnection::serialize<cereal::BinaryOutputArchive>(
    cereal::BinaryOutputArchive &);
template void
SockState::serialize<cereal::BinaryInputArchive>(cereal::BinaryInputArchive &);
template void SockState::serialize<cereal::BinaryOutputArchive>(
    cereal::BinaryOutputArchive &);

/* ==============================================================================
 * Debug
 * ==============================================================================
 */

void SockState::dump_state() const
{
  detsim::ui::ui_printf("[SockState] %zu sockets, %zu UDP buffers\n",
                        sockets_.size(), udp_recv_buffers_.size());

  for (const auto &kv : sockets_)
  {
    const Socket &sock = kv.second;
    detsim::ui::ui_printf(
        "  fd=%d: domain=%d, type=%s, bound=%s, %s\n", sock.fd, sock.domain,
        sock.type == SOCK_STREAM ? "TCP" : "UDP", sock.bound ? "yes" : "no",
        sock.bound ? format_sockaddr(sock.local_addr).c_str() : "-");

    auto buf_it = udp_recv_buffers_.find(sock.fd);
    if (buf_it != udp_recv_buffers_.end() && !buf_it->second.empty())
    {
      detsim::ui::ui_printf("    UDP buffer: %zu datagrams\n",
                            buf_it->second.size());
    }
  }
}

/* ==============================================================================
 * Legacy C-style API - delegates to SockState
 * ==============================================================================
 */

/* Helper: Convert string address to sockaddr_in */
static void addr_str_to_sockaddr(const std::string &addr_str,
                                 struct sockaddr_in *sin)
{
  if (addr_str.size() >= sizeof(struct sockaddr_in))
  {
    memcpy(sin, addr_str.c_str(), sizeof(struct sockaddr_in));
  }
  else
  {
    memset(sin, 0, sizeof(*sin));
  }
}

/* Helper: Convert sockaddr_in to string address */
static std::string sockaddr_to_addr_str(const struct sockaddr_in *sin)
{
  return std::string((const char *)sin, sizeof(*sin));
}

int emu_socket(int domain, int type, int protocol)
{
  int cur = ptmc_state.cursor;
  return ptmc_state.sock_states[cur].do_socket(domain, type, protocol);
}

int emu_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
  int cur = ptmc_state.cursor;
  return ptmc_state.sock_states[cur].do_bind(sockfd, addr, addrlen);
}

int emu_listen(int sockfd, int backlog)
{
  int cur = ptmc_state.cursor;
  return ptmc_state.sock_states[cur].do_listen(sockfd, backlog);
}

int emu_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
  int cur = ptmc_state.cursor;
  return ptmc_state.sock_states[cur].do_connect(sockfd, addr, addrlen);
}

ssize_t emu_recvfrom(int sockfd, void *buf, size_t len, int flags,
                     struct sockaddr *src_addr, socklen_t *addrlen)
{
  int cur = ptmc_state.cursor;
  // Use choice from ptmc_state if available, otherwise default to 0
  int choice = (ptmc_state.n_choose > 0) ? ptmc_state.choose : 0;
  return ptmc_state.sock_states[cur].do_recvfrom_with_choice(
      sockfd, buf, len, flags, src_addr, addrlen, choice);
}

// Get number of recvfrom choices for current process and fd
int emu_recvfrom_get_choices(int sockfd)
{
  int cur = ptmc_state.cursor;
  return ptmc_state.sock_states[cur].get_recvfrom_choices(sockfd);
}

/* Helper: Find which node owns an address */
static int addr_to_node(const struct sockaddr_in *addr)
{
  if (addr->sin_addr.s_addr == 0xff000001 ||
      addr->sin_addr.s_addr == 0x00000000)
  {
    return ptmc_state.cursor;
  }

  for (int i = 0; i < NP; i++)
  {
    struct in_addr in_addr_i;
    if (inet_pton(AF_INET, ptmc_state.addrs[i].c_str(), &in_addr_i) != 1)
    {
      LOG_ERROR("inet_pton failed");
      continue;
    }
    if (addr->sin_addr.s_addr == in_addr_i.s_addr)
      return i;
  }

  char addr_str[32];
  inet_ntop(AF_INET, &addr->sin_addr, addr_str, sizeof(addr_str));
  LOG_ERROR("Addr %s not found", addr_str);
  return -1;
}

ssize_t emu_sendto(int sockfd, const void *buf, size_t len, int flags,
                   struct sockaddr *dest_addr, socklen_t addrlen)
{
  LOG_TRACE("emu_sendto(%d, %p, %ld, %d, %p, %d)", sockfd, (void *)buf, len,
            flags, (void *)dest_addr, addrlen);

  int cur = ptmc_state.cursor;
  const Socket *sock = ptmc_state.sock_states[cur].get_socket(sockfd);
  if (!sock)
  {
    return -ENOTSOCK;
  }

  if (sock->type != SOCK_DGRAM)
  {
    return -EOPNOTSUPP;
  }

  /* Get destination info */
  struct sockaddr_in dest;
  if (dest_addr && addrlen > 0)
  {
    memcpy_guest2host(&dest, dest_addr, sizeof(struct sockaddr_in));
  }
  else if (sock->connected)
  {
    dest = sock->peer_addr;
  }
  else
  {
    return -EDESTADDRREQ;
  }

  int node = addr_to_node(&dest);
  if (node < 0)
  {
    return -ENETUNREACH;
  }

  uint16_t port = dest.sin_port;
  assert(port > 0);

  /* Find receiving socket by port */
  int recvfd = -1;
  const auto &recv_sockets = ptmc_state.sock_states[node].sockets();
  for (const auto &kv : recv_sockets)
  {
    const Socket &rs = kv.second;
    if (rs.bound && rs.local_addr.sin_port == port)
    {
      recvfd = kv.first;
      break;
    }
  }

  if (recvfd == -1)
  {
    /* No receiver - drop packet but report success */
    return len;
  }

  /* Read data from guest */
  char *data = (char *)malloc(len);
  if (!data)
  {
    return -ENOMEM;
  }
  memcpy_guest2host(data, buf, len);

  /* Create datagram with proper source address */
  UdpDatagram dg;
  dg.content = std::string(data, len);
  free(data);

  /* Set source address */
  dg.from.sin_family = AF_INET;
  dg.from.sin_port = sock->bound ? sock->local_addr.sin_port : 0;
  inet_pton(AF_INET, ptmc_state.addrs[cur].c_str(), &dg.from.sin_addr);

  /* Deliver to receiver */
  ptmc_state.sock_states[node].deliver_udp_datagram(recvfd, dg);

  LOG_TRACE("emu_sendto: delivered %zu bytes to node %d fd %d", len, node,
            recvfd);

  return len;
}
