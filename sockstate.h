#ifndef __SOCKSTATE_H
#define __SOCKSTATE_H

#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sstream>
#include <deque>
// #include "utils.h"
#include <string>
#include "cereal/archives/binary.hpp"
#include "cereal/types/string.hpp"
#include "cereal/types/deque.hpp"

/* System global. Always in tracer so can be very big */
#define MAXADDR 128 
#define MAXCHAN 128

/* design goal: support unix socket, udp and tcp:
 * AF_UNIX; AF_INET/SOCK_DGRAM & SOCK_STREAM 

 * Valid socket types in the UNIX domain are: SOCK_STREAM, for a
 * stream-oriented socket; SOCK_DGRAM, for a datagram-oriented
 * socket that preserves message boundaries (as on most UNIX
 * implementations, UNIX domain datagram sockets are always reliable
 * and don't reorder datagrams) */


/* For AF_UNIX, sockaddr_un may easily exceed tens of bytes.
 * It's so susceptible.
 * Maybe need to manage all sockaddr in one place.
 * That comes with another problem: is this socket new created? */

/* length variable */
typedef std::string ptmc_addr;

enum { TCP_TYPE = 1, UDP_TYPE };


struct tcp_buffer {
  std::stringstream ss;

  template <class Archive>
    void save(Archive &ar) const
    {
      ar(ss.str());
    }

  template <class Archive>
    void load(Archive &ar)
    {
      std::string s;
      ar(s);
      ss << s;
    }

  tcp_buffer() { }

  tcp_buffer(const tcp_buffer& b) {
    ss << b.ss.str();
  }
  tcp_buffer& operator=(const tcp_buffer& b) {
    ss << b.ss.str();
    return *this;
  }
};

struct ptmc_datagram {
  std::string content;
  ptmc_addr from;

  template <class Archive>
    void serialize(Archive &ar)
    { 
      ar(content, from);
    }
}; 

typedef std::deque<ptmc_datagram> udp_buffer;

/* TODO: udp not sequential */  

/* A connection is created, accompanied with two sockets */
typedef struct Conn {
  ptmc_addr addr[2];
} Conn; /* Equivalent */

typedef struct ptmc_sock {
  int fd;
  int domain;
  int type;
  int protocol;
  
  /* If LISTENED */
  int backlog;
  /* TODO: pending connect queue */

  ptmc_addr addr;
  ptmc_addr dest;

  template <class Archive>
    void serialize(Archive &ar) {
      ar(fd, domain, type, protocol, backlog, addr, dest);
    }
} ptmc_sock;

int emu_socket(int sockfd, int domain, int type, int protocol);

/* connetion based */
int emu_listen(int sockfd, int backlog);

int emu_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);

int emu_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);

ssize_t emu_recvfrom(
    int sockfd, 
    void *buf,
    size_t len, 
    int flags, 
    struct sockaddr *src_addr, 
    socklen_t *addrlen
    );

ssize_t emu_sendto(
    int sockfd, 
    const void *buf,
    size_t len, 
    int flags, 
    struct sockaddr *dest_addr, 
    socklen_t addrlen
    );

/* connection based, return a socket file descriptor */
int emu_accept4(
    int sockfd, 
    struct sockaddr *src_addr, 
    socklen_t *addrlen,
    int flags
    );

#endif /* __SOCKSTATE_H */
