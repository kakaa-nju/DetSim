/*
 * syscall_fmt.cpp - System call string formatting
 *
 * This module formats system call information into human-readable strings
 * for logging, debugging, and display purposes.
 */

#include "state.h"
#include "guest.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/time.h>

/* External syscall name table */
extern const char *syscalls[450];

namespace syscall_fmt {

/* ======================================================================
 * Helper Functions
 * ====================================================================== */

/* Format memory content with printable characters escaped */
static int format_mem_content(char *dest, const char *src, size_t len)
{
  int pos = 0;
  for (size_t i = 0; i < len; i++)
  {
    uint8_t val = src[i];
    if (isprint(src[i]))
      pos += sprintf(dest + pos, "%c", src[i]);
    else
      pos += sprintf(dest + pos, "\\%03u", val);
  }
  return pos;
}

/* Format sockaddr_in structure */
static int format_sockaddr(char *dest, struct sockaddr_in *addr)
{
  char ipbuf[INET_ADDRSTRLEN];
  const char *ipstr = inet_ntop(AF_INET, &(addr->sin_addr), ipbuf, sizeof(ipbuf));
  if (!ipstr)
    ipstr = "invalid";

  return sprintf(dest,
                 "{sin_family=AF_INET, sin_port=htons(%d), sin_addr=inet_addr(\"%s\")}",
                 ntohs(addr->sin_port), ipstr);
}

/* ======================================================================
 * Individual Syscall Formatters
 * ====================================================================== */

/* Format: int fd, void *buf, size_t count (read/write style) */
void format_fd_buf_count(char *buf, tracee_state *t, syscall_info *info)
{
  char *mem = (char *)t->read_snapshot_mem(info->args[1], info->args[2]);
  assert(mem);

  int pos = sprintf(buf, "%s(%ld, %p(\"", syscalls[info->nr], info->args[0],
                    (void *)info->args[1]);

  pos += format_mem_content(buf + pos, mem, info->args[2]);

  pos += sprintf(buf + pos, "\"), %ld) = %ld", info->args[2], info->rval);
  free(mem);
}

/* Format: int fd only (close style) */
void format_fd_only(char *buf, syscall_info *info)
{
  sprintf(buf, "%s(%ld) = %ld", syscalls[info->nr], info->args[0], info->rval);
}

/* Format: sendto */
void format_sendto(char *buf, tracee_state *t, syscall_info *info)
{
  /* Limit buffer size to prevent overflow - cap at 4KB for display */
  size_t data_len = info->args[2];
  if (data_len > 4096)
    data_len = 4096;
  
  char *mem = (char *)t->read_snapshot_mem(info->args[1], data_len);
  if (!mem) {
    sprintf(buf, "%s(%ld, <error reading buffer>, %ld, %ld, <addr>, %ld) = %ld",
            syscalls[info->nr], info->args[0], info->args[2], info->args[3],
            info->args[5], info->rval);
    return;
  }

  int pos = sprintf(buf, "%s(%ld, \"", syscalls[info->nr], info->args[0]);

  pos += format_mem_content(buf + pos, mem, data_len);

  if (info->args[2] > 4096)
    pos += sprintf(buf + pos, "...(%ld more bytes)", info->args[2] - 4096);

  pos += sprintf(buf + pos, "\", %ld, %ld, ", info->args[2], info->args[3]);
  free(mem);

  struct sockaddr_in *addr =
      (struct sockaddr_in *)t->read_snapshot_mem(info->args[4], info->args[5]);
  if (addr) {
    pos += format_sockaddr(buf + pos, addr);
    free(addr);
  } else {
    pos += sprintf(buf + pos, "<invalid addr>");
  }

  sprintf(buf + pos, ", %ld) = %ld", info->args[5], info->rval);
}

/* Format: recvfrom */
void format_recvfrom(char *buf, tracee_state *t, syscall_info *info)
{
  /* Limit displayed bytes to prevent buffer overflow - use return value if valid */
  size_t display_len = info->rval;
  if (display_len <= 0 || display_len > 4096)
    display_len = (info->args[2] > 4096) ? 4096 : info->args[2];
  
  char *mem = (char *)t->read_snapshot_mem(info->args[1], display_len);
  if (!mem) {
    sprintf(buf, "%s(%ld, <error reading buffer>, %ld, %ld, <addr>, [%d]) = %ld",
            syscalls[info->nr], info->args[0], info->args[2], info->args[3],
            sizeof(socklen_t), info->rval);
    return;
  }

  int pos = sprintf(buf, "%s(%ld, \"", syscalls[info->nr], info->args[0]);

  pos += format_mem_content(buf + pos, mem, display_len);

  if (info->rval > 4096)
    pos += sprintf(buf + pos, "...(%ld more bytes)", info->rval - 4096);

  pos += sprintf(buf + pos, "\", %ld, %ld, ", info->args[2], info->args[3]);
  free(mem);

  socklen_t *plen = (socklen_t *)t->read_snapshot_mem(info->args[5], sizeof(socklen_t));
  if (!plen) {
    pos += sprintf(buf + pos, "<invalid addrlen>, [?]) = %ld", info->rval);
    return;
  }
  
  socklen_t addrlen = *plen;

  struct sockaddr_in *addr =
      (struct sockaddr_in *)t->read_snapshot_mem(info->args[4], addrlen);

  if (addr) {
    pos += format_sockaddr(buf + pos, addr);
    free(addr);
  } else {
    pos += sprintf(buf + pos, "<invalid addr>");
  }

  sprintf(buf + pos, ", [%d]) = %ld", addrlen, info->rval);
  free(plen);
}

/* Format: void *addr (brk/mmap style) */
void format_addr(char *buf, syscall_info *info)
{
  sprintf(buf, "%s(0x%lx) = 0x%lx", syscalls[info->nr],
          info->args[0], info->rval);
}

/* Format: int status (exit style) */
void format_exit(char *buf, syscall_info *info)
{
  sprintf(buf, "%s(%ld) = ?", syscalls[info->nr], info->args[0]);
}

/* Format: openat */
void format_openat(char *buf, tracee_state *t, syscall_info *info)
{
  /* Use a reasonable size for path display - protect against huge paths */
  size_t path_len = 256;
  char *path = (char *)t->read_snapshot_mem(info->args[1], path_len);
  if (!path) {
    sprintf(buf, "openat(%ld, <error reading path>, %ld, %ld) = %ld",
            info->args[0], info->args[2], info->args[3], info->rval);
    return;
  }
  
  /* Ensure null termination within our buffer */
  path[path_len - 1] = '\0';
  /* Find actual string length if shorter */
  size_t actual_len = strnlen(path, path_len);

  const char *flag_str = "";
  int flags = info->args[2];
  if ((flags & O_RDWR) == O_RDWR)
    flag_str = "O_RDWR";
  else if (flags & O_WRONLY)
    flag_str = "O_WRONLY";
  else
    flag_str = "O_RDONLY";

  sprintf(buf, "openat(%ld, \"%s\", %s|0x%x, 0%03lo) = %ld",
          info->args[0], path, flag_str, flags & ~O_ACCMODE, info->args[3], info->rval);
  free(path);
}

/* Format: gettimeofday */
void format_gettimeofday(char *buf, tracee_state *t, syscall_info *info)
{
  struct timeval *tv = (struct timeval *)t->read_snapshot_mem(
      info->args[0], sizeof(struct timeval));
  if (!tv) {
    sprintf(buf, "%s(%p, %p) = %ld <error reading timeval>",
            syscalls[info->nr], (void *)info->args[0],
            (void *)info->args[1], info->rval);
    return;
  }

  sprintf(buf, "%s({tv_sec=%ld, tv_usec=%ld}, %p) = %ld",
          syscalls[info->nr], tv->tv_sec, tv->tv_usec,
          (void *)info->args[1], info->rval);
  free(tv);
}

/* Format: clock_nanosleep */
void format_clock_nanosleep(char *buf, tracee_state *t, syscall_info *info)
{
  struct timespec *ts = (struct timespec *)t->read_snapshot_mem(
      info->args[2], sizeof(struct timespec));
  if (!ts) {
    sprintf(buf, "clock_nanosleep(%ld, %ld, %p, %p) = %ld <error reading timespec>",
            info->args[0], info->args[1], (void *)info->args[2],
            (void *)info->args[3], info->rval);
    return;
  }

  sprintf(buf, "clock_nanosleep(%ld, %ld, {tv_sec=%ld, tv_nsec=%ld}, %p) = %ld",
          info->args[0], info->args[1], ts->tv_sec, ts->tv_nsec,
          (void *)info->args[3], info->rval);
  free(ts);
}

/* Format: socket */
void format_socket(char *buf, syscall_info *info)
{
  const char *domain_str = "AF_???";
  if (info->args[0] == AF_INET)
    domain_str = "AF_INET";
  else if (info->args[0] == AF_UNIX)
    domain_str = "AF_UNIX";

  const char *type_str = "SOCK_???";
  if (info->args[1] == SOCK_STREAM)
    type_str = "SOCK_STREAM";
  else if (info->args[1] == SOCK_DGRAM)
    type_str = "SOCK_DGRAM";

  sprintf(buf, "socket(%s, %s, %ld) = %ld",
          domain_str, type_str, info->args[2], info->rval);
}

/* Format: bind/listen/accept/connect */
void format_socket_fd(char *buf, syscall_info *info)
{
  sprintf(buf, "%s(%ld, ...) = %ld", syscalls[info->nr], info->args[0], info->rval);
}

/* Format: default (6 arguments hex) */
void format_default(char *buf, syscall_info *info)
{
  sprintf(buf, "%s(0x%lx, 0x%lx, 0x%lx, 0x%lx, 0x%lx, 0x%lx) = %ld",
          syscalls[info->nr], info->args[0], info->args[1], info->args[2],
          info->args[3], info->args[4], info->args[5], info->rval);
}

/* ======================================================================
 * Main Formatting Function
 * ====================================================================== */

void format(char *buf, tracee_state *t, syscall_info *info)
{
  switch (info->nr)
  {
    /* Network */
    case SYS_sendto:
      format_sendto(buf, t, info);
      break;
    case SYS_recvfrom:
      format_recvfrom(buf, t, info);
      break;
    case SYS_socket:
      format_socket(buf, info);
      break;
    case SYS_bind:
    case SYS_listen:
    case SYS_accept:
    case SYS_accept4:
    case SYS_connect:
      format_socket_fd(buf, info);
      break;

    /* File I/O */
    case SYS_read:
    case SYS_write:
      format_fd_buf_count(buf, t, info);
      break;
    case SYS_openat:
      format_openat(buf, t, info);
      break;
    case SYS_close:
      format_fd_only(buf, info);
      break;

    /* Memory */
    case SYS_brk:
    case SYS_mmap:
      format_addr(buf, info);
      break;

    /* Time */
    case SYS_gettimeofday:
      format_gettimeofday(buf, t, info);
      break;
    case SYS_clock_nanosleep:
      format_clock_nanosleep(buf, t, info);
      break;

    /* Process */
    case SYS_exit:
    case SYS_exit_group:
      format_exit(buf, info);
      break;

    /* Simple fd-only */
    case SYS_sched_yield:
    case SYS_set_tid_address:
      format_fd_only(buf, info);
      break;

    /* Default */
    default:
      format_default(buf, info);
      break;
  }
}

} /* namespace syscall_fmt */
