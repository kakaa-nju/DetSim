/* ipv4 by default */

#include "monitor.h"
#include "sockstate.h"
#include "debug.h"
#include "guest.h"
#include <cerrno>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/socket.h>

ptmc_sock *get_socket(int sockfd) {
  int cur = ptmc_state.cursor;
  for (auto &sock: ptmc_state.sock_lists[cur]) 
    if (sock.fd == sockfd) return &sock;
  return NULL;
}


int emu_socket(int sockfd, int domain, int type, int protocol) {
  ptmc_sock sock = (ptmc_sock) { 
    .fd = sockfd, 
    .domain = domain,
    .type = type,
    .protocol = protocol
  };
  int cur = ptmc_state.cursor;
  ptmc_state.sock_lists[cur].push_back(sock);
  return sockfd;
}

int emu_listen(int sockfd, int backlog) {
  ptmc_sock *sock = get_socket(sockfd);
  if (!sock)
    return -ENOTSOCK;

  if (sock->type != SOCK_STREAM)
    return -EOPNOTSUPP;
  sock->backlog = backlog;
  return 0;
}

int emu_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
  ptmc_sock *sock = get_socket(sockfd);
  if (!sock)
    return -ENOTSOCK;

  if (sock->type == SOCK_DGRAM) 
    sock->dest = std::string((char *)addr, (char *)addr + addrlen);
  else if (sock->type == SOCK_STREAM)
    panic("not implemented");
  return 0;
}

int emu_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
  LOG_TRACE("emu_bind(%d, %p, %d)", sockfd, (void *)addr, addrlen);
  ptmc_sock *sock = get_socket(sockfd);
  if (!sock) 
  {
    LOG_TRACE("emu_bind: ENOTSOCK");
    return -ENOTSOCK;
  }
  char *tmp = (char *)malloc(addrlen);
  memcpy_guest2host(tmp, addr, addrlen);
  sock->addr = std::string(tmp, tmp + addrlen);
  free(tmp);
  return 0;
}
  
ssize_t emu_recvfrom(
    int sockfd, 
    void *buf,
    size_t len,
    int flags,
    struct sockaddr *src_addr,
    socklen_t *addrlen
    ) {
  ptmc_sock *sock = get_socket(sockfd);
  if (!sock) 
    return -ENOTSOCK;
  if (sock->type & SOCK_STREAM)
    panic("not implemented");
  else if (sock->type == SOCK_DGRAM)
  {
    auto &chan = ptmc_state.udp_buffer_lists[ptmc_state.cursor][sockfd];
    if (chan.empty())
      return -EAGAIN;
    auto msg = chan.front();
    chan.pop_front();

    size_t msg_len = std::min(msg.content.length(), len);
    memcpy_host2guest(buf, msg.content.c_str(), msg_len);

    socklen_t msg_addrlen = msg.from.size();
    if (src_addr != NULL) 
      memcpy_host2guest(src_addr, msg.from.c_str(), msg_addrlen);
    if (addrlen != NULL)
      memcpy_host2guest(addrlen, &msg_addrlen, sizeof(msg_addrlen));

    return msg_len;
  }
  panic("NOT any socket");
  return 0;
}

static int addr_to_node(const struct sockaddr_in *addr, socklen_t len) {
  if (addr->sin_addr.s_addr == 0xff000001
   || addr->sin_addr.s_addr == 0x00000000)
    return ptmc_state.cursor;

  for (int i = 0; i < NP; i++)
  {
    auto addr_i_str = ptmc_state.addrs[i];
    struct in_addr in_addr_i; 
    if (inet_pton(AF_INET, addr_i_str.c_str(), &in_addr_i) != 1)
    {
      LOG_ERROR("inet_pton failed");
    }
    if (addr->sin_addr.s_addr == in_addr_i.s_addr)
      return i;
  }
  char addr_str[32];
  inet_ntop(AF_INET, &addr->sin_addr, addr_str, len);
  LOG_ERROR("Addr %s not found", addr_str);
  return -1;
}

ssize_t emu_sendto(
    int sockfd,
    const void *buf, 
    size_t len, 
    int flags, 
    struct sockaddr *dest_addr, 
    socklen_t addrlen
    ) {
  LOG_TRACE("emu_sendto(%d, %p, %ld, %d, %p, %d)",
      sockfd, (void *)buf, len, flags, (void *)dest_addr, addrlen);
  ptmc_sock *sock = get_socket(sockfd);
  if (!sock)
    return -ENOTSOCK;

  int node = -1;
  int port = -1;

  if (sock->type == SOCK_STREAM)
    panic("not implemented");
  else if (sock->type == SOCK_DGRAM)
  {
    /* get channel on dest with dest addr */
    if (dest_addr != NULL && addrlen != 0) 
    {
      struct sockaddr_in dest_addr_copy;
      memcpy_guest2host(&dest_addr_copy, dest_addr, addrlen);
      node = addr_to_node(&dest_addr_copy, addrlen);
      port = dest_addr_copy.sin_port;
    }
    else if (!sock->dest.empty()) 
    {
      socklen_t addrlen = sock->dest.length();
      node = addr_to_node((struct sockaddr_in *)sock->dest.c_str(), addrlen);
      port = ((struct sockaddr_in *)sock->dest.c_str())->sin_port;
    }
    else 
      return -EDESTADDRREQ;

    assert(port > 0 && port < 65536);

    int recvfd = -1;
    for (auto &recvsock: ptmc_state.sock_lists[node]) 
    {
      if (((struct sockaddr_in *)recvsock.addr.c_str())->sin_port == port)
        recvfd = recvsock.fd;
    }
    if (recvfd == -1) return 0; /* always success */

    auto &chan = ptmc_state.udp_buffer_lists[node][recvfd];

    /* construct dg */
    struct sockaddr_in from_raw;
    memcpy(&from_raw, sock->addr.c_str(), sock->addr.length());

    struct sockaddr_in from;
    memset(&from, 0, sizeof(from));
    from.sin_family = AF_INET;
    from.sin_addr.s_addr = inet_addr(ptmc_state.addrs[ptmc_state.cursor].c_str());
    from.sin_port = from_raw.sin_port;

    char *dup = (char *)malloc(len);
    memcpy_guest2host(dup, buf, len);
    ptmc_datagram dg = (ptmc_datagram) {
      .content = std::string((char *)dup, (char *)dup + len),
      .from = std::string((char *)&from, (char *)&from + addrlen),
    };
    free(dup);

    /* lose packet */
    if (chan.size() == 3) 
      return len;

    /* add dg to channel */
    chan.push_back(dg);
    return len;
  }

  panic("NOT any socket");
  return 0;
}
