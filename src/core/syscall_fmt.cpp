/*
 * syscall_fmt.cpp - System call string formatting
 *
 * This module formats system call information into human-readable strings
 * for logging, debugging, and display purposes.
 */

#include "state.h"
#include "guest.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
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

/* Helper: Format return value with errno if negative */
static int format_ret(char *dest, long rval)
{
  if (rval < 0)
    return sprintf(dest, "= %ld (%s)", rval, strerror(-rval));
  else
    return sprintf(dest, "= %ld", rval);
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

/* Format: write - input buffer, print by requested size */
void format_write(char *buf, tracee_state *t, syscall_info *info)
{
  size_t requested_len = info->args[2];
  size_t display_len = requested_len;
  if (display_len > 4096) display_len = 4096;

  char *mem = (char *)t->read_snapshot_mem(info->args[1], display_len);
  
  int pos = sprintf(buf, "write(%ld, ", info->args[0]);
  
  if (mem) {
    pos += sprintf(buf + pos, "%p\"", (void *)info->args[1]);
    pos += format_mem_content(buf + pos, mem, display_len);
    if (requested_len > 4096)
      pos += sprintf(buf + pos, "...(%ld more bytes)", requested_len - 4096);
    pos += sprintf(buf + pos, "\"");
    free(mem);
  } else {
    pos += sprintf(buf + pos, "%p", (void *)info->args[1]);
  }
  
  pos += sprintf(buf + pos, ", %ld) ", info->args[2]);
  format_ret(buf + pos, info->rval);
}

/* Format: read - output buffer, print by return value */
void format_read(char *buf, tracee_state *t, syscall_info *info)
{
  int pos = sprintf(buf, "read(%ld, ", info->args[0]);
  
  ssize_t ret = info->rval;
  if (ret < 0) {
    /* Failed: just show pointer */
    pos += sprintf(buf + pos, "%p", (void *)info->args[1]);
  } else if (ret == 0) {
    /* EOF: show empty string */
    pos += sprintf(buf + pos, "%p\"\"", (void *)info->args[1]);
  } else {
    /* Success: show content by return value */
    size_t display_len = ret;
    if (display_len > 4096) display_len = 4096;
    
    char *mem = (char *)t->read_snapshot_mem(info->args[1], display_len);
    if (mem) {
      pos += sprintf(buf + pos, "%p\"", (void *)info->args[1]);
      pos += format_mem_content(buf + pos, mem, display_len);
      if (ret > 4096)
        pos += sprintf(buf + pos, "...");
      pos += sprintf(buf + pos, "\"");
      free(mem);
    } else {
      pos += sprintf(buf + pos, "%p", (void *)info->args[1]);
    }
  }
  
  pos += sprintf(buf + pos, ", %ld) ", info->args[2]);
  format_ret(buf + pos, info->rval);
}

/* Format: int fd only (close style) */
void format_fd_only(char *buf, syscall_info *info)
{
  int pos = sprintf(buf, "%s(%ld) ", syscalls[info->nr], info->args[0]);
  format_ret(buf + pos, info->rval);
}

/* Format: sendto - input buffer, print by requested size */
void format_sendto(char *buf, tracee_state *t, syscall_info *info)
{
  /* Limit buffer size to prevent overflow - cap at 4KB for display */
  size_t requested_len = info->args[2];
  size_t display_len = requested_len;
  if (display_len > 4096) display_len = 4096;
  
  int pos = sprintf(buf, "sendto(%ld, ", info->args[0]);

  char *mem = (char *)t->read_snapshot_mem(info->args[1], display_len);
  if (mem) {
    pos += sprintf(buf + pos, "%p\"", (void *)info->args[1]);
    pos += format_mem_content(buf + pos, mem, display_len);
    if (requested_len > 4096)
      pos += sprintf(buf + pos, "...(%ld more bytes)", requested_len - 4096);
    pos += sprintf(buf + pos, "\"");
    free(mem);
  } else {
    pos += sprintf(buf + pos, "%p", (void *)info->args[1]);
  }

  pos += sprintf(buf + pos, ", %ld, %ld, ", info->args[2], info->args[3]);

  struct sockaddr_in *addr =
      (struct sockaddr_in *)t->read_snapshot_mem(info->args[4], info->args[5]);
  if (addr) {
    pos += format_sockaddr(buf + pos, addr);
    free(addr);
  } else {
    pos += sprintf(buf + pos, "<invalid addr>");
  }

  pos += sprintf(buf + pos, ", %ld) ", info->args[5]);
  format_ret(buf + pos, info->rval);
}

/* Format: recvfrom - output buffer, print by return value */
void format_recvfrom(char *buf, tracee_state *t, syscall_info *info)
{
  int pos = 0;
  ssize_t ret = info->rval;
  
  /* Start: recvfrom(fd, buf, len, flags, addr, addrlen) */
  pos += sprintf(buf + pos, "recvfrom(%ld, ", info->args[0]);
  
  /* buf argument - output buffer, print by return value */
  if (ret < 0) {
    /* Failed: just show pointer */
    pos += sprintf(buf + pos, "%p", (void *)info->args[1]);
  } else if (ret == 0) {
    /* EOF: show empty string */
    pos += sprintf(buf + pos, "%p\"\"", (void *)info->args[1]);
  } else {
    /* Success: show pointer + content by return value */
    size_t display_len = ret;
    if (display_len > 4096) display_len = 4096;
    
    char *mem = (char *)t->read_snapshot_mem(info->args[1], display_len);
    if (mem) {
      pos += sprintf(buf + pos, "%p\"", (void *)info->args[1]);
      pos += format_mem_content(buf + pos, mem, display_len);
      pos += sprintf(buf + pos, "\"");
      if (ret > 4096)
        pos += sprintf(buf + pos, "...");
      free(mem);
    } else {
      pos += sprintf(buf + pos, "%p", (void *)info->args[1]);
    }
  }
  
  /* len and flags */
  pos += sprintf(buf + pos, ", %ld, %ld, ", info->args[2], info->args[3]);
  
  /* addr and addrlen - output arguments */
  if (info->args[5] != 0) {
    /* Read addrlen from output pointer */
    socklen_t *plen = (socklen_t *)t->read_snapshot_mem(info->args[5], sizeof(socklen_t));
    if (plen && *plen > 0 && ret >= 0) {
      socklen_t addrlen = *plen;
      /* addr pointer */
      pos += sprintf(buf + pos, "%p", (void *)info->args[4]);
      /* addr content */
      if (addrlen >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in *addr = (struct sockaddr_in *)t->read_snapshot_mem(info->args[4], addrlen);
        if (addr) {
          pos += sprintf(buf + pos, " ");
          pos += format_sockaddr(buf + pos, addr);
          free(addr);
        }
      }
      pos += sprintf(buf + pos, ", [%d]", addrlen);
      free(plen);
    } else {
      /* No address returned or failed */
      pos += sprintf(buf + pos, "%p", (void *)info->args[4]);
      if (plen) {
        pos += sprintf(buf + pos, ", [%d]", (int)*plen);
        free(plen);
      } else {
        pos += sprintf(buf + pos, ", [0]");
      }
    }
  } else {
    /* NULL addrlen pointer */
    pos += sprintf(buf + pos, "%p, NULL", (void *)info->args[4]);
  }
  
  pos += sprintf(buf + pos, ") ");
  format_ret(buf + pos, info->rval);
}

void format_poll(char *buf, syscall_info *info)
{
    // 从 info 获取参数（Guest 指针）
    void *guest_fds = (void *)info->args[0];
    unsigned long nfds = info->args[1];
    int timeout = (int)info->args[2];
    int ret = (int)info->rval;
    
    int pos = 0;
    
    // 格式化基本信息
    pos += sprintf(buf + pos, "poll(");
    
    if (guest_fds == NULL) {
        pos += sprintf(buf + pos, "NULL");
    } else {
        pos += sprintf(buf + pos, "%p", guest_fds);
    }
    
    pos += sprintf(buf + pos, ", %lu, ", nfds);
    
    // timeout 人类可读
    if (timeout == -1) {
        pos += sprintf(buf + pos, "INFTIM");
    } else if (timeout == 0) {
        pos += sprintf(buf + pos, "0");
    } else {
        pos += sprintf(buf + pos, "%d", timeout);
    }
    
    pos += sprintf(buf + pos, ") = ");
    
    // 返回值
    if (ret < 0) {
        pos += sprintf(buf + pos, "%d (%s)", ret, strerror(-ret));
    } else {
        pos += sprintf(buf + pos, "%d", ret);
    }
    
    // 关键：从 Guest 读取 pollfd 数组详情
    if (nfds > 0 && nfds <= 1024 && guest_fds != NULL) {
        size_t fds_size = sizeof(struct pollfd) * nfds;
        struct pollfd *host_fds = (struct pollfd *)malloc(fds_size);
        
        if (host_fds != NULL) {
            // 从 Guest 复制到 Host
            memcpy_guest2host(host_fds, guest_fds, fds_size);
            
            pos += sprintf(buf + pos, " {");
            
            nfds_t display_count = (nfds > 4) ? 4 : nfds;
            for (nfds_t i = 0; i < display_count; i++) {
                if (i > 0) pos += sprintf(buf + pos, ", ");
                
                pos += sprintf(buf + pos, "[%zu]", (size_t)i);
                
                int fd = host_fds[i].fd;
                short events = host_fds[i].events;
                short revents = host_fds[i].revents;
                
                // fd
                if (fd < 0) {
                    pos += sprintf(buf + pos, "IGNORED");
                } else {
                    pos += sprintf(buf + pos, "fd=%d", fd);
                }
                
                // events（请求的事件）
                if (events != 0) {
                    pos += sprintf(buf + pos, ",ev=");
                    if (events & POLLIN) pos += sprintf(buf + pos, "IN");
                    if (events & POLLOUT) pos += sprintf(buf + pos, "OUT");
                    if (events & POLLERR) pos += sprintf(buf + pos, "ERR");
                    if (events & POLLHUP) pos += sprintf(buf + pos, "HUP");
                    if (events & POLLNVAL) pos += sprintf(buf + pos, "NVAL");
                }
                
                // revents（实际发生的事件）
                if (revents != 0) {
                    pos += sprintf(buf + pos, ",re=");
                    if (revents & POLLIN) pos += sprintf(buf + pos, "IN");
                    if (revents & POLLOUT) pos += sprintf(buf + pos, "OUT");
                    if (revents & POLLERR) pos += sprintf(buf + pos, "ERR");
                    if (revents & POLLHUP) pos += sprintf(buf + pos, "HUP");
                    if (revents & POLLNVAL) pos += sprintf(buf + pos, "NVAL");
                }
            }
            
            if (nfds > 4) {
                pos += sprintf(buf + pos, ", ...(%lu more)", nfds - 4);
            }
            
            pos += sprintf(buf + pos, "}");
            
            free(host_fds);
        }
    }
}

/* Format: void *addr (brk/mmap style) */
void format_addr(char *buf, syscall_info *info)
{
  int pos = sprintf(buf, "%s(0x%lx) ", syscalls[info->nr], info->args[0]);
  format_ret(buf + pos, info->rval);
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
  
  int pos = sprintf(buf, "openat(%ld, ", info->args[0]);
  
  if (path) {
    /* Ensure null termination within our buffer */
    path[path_len - 1] = '\0';
    
    const char *flag_str = "";
    int flags = info->args[2];
    if ((flags & O_RDWR) == O_RDWR)
      flag_str = "O_RDWR";
    else if (flags & O_WRONLY)
      flag_str = "O_WRONLY";
    else
      flag_str = "O_RDONLY";

    pos += sprintf(buf + pos, "\"%s\", %s|0x%x, 0%03lo) ",
            path, flag_str, flags & ~O_ACCMODE, info->args[3]);
    free(path);
  } else {
    pos += sprintf(buf + pos, "<error reading path>, %ld, %ld) ", 
            info->args[2], info->args[3]);
  }
  
  format_ret(buf + pos, info->rval);
}

/* Format: gettimeofday */
void format_gettimeofday(char *buf, tracee_state *t, syscall_info *info)
{
  int pos = sprintf(buf, "gettimeofday(");
  
  struct timeval *tv = (struct timeval *)t->read_snapshot_mem(
      info->args[0], sizeof(struct timeval));
  if (tv && info->rval >= 0) {
    pos += sprintf(buf + pos, "{tv_sec=%ld, tv_usec=%ld}", tv->tv_sec, tv->tv_usec);
    free(tv);
  } else {
    pos += sprintf(buf + pos, "%p", (void *)info->args[0]);
    if (tv) free(tv);
  }
  
  pos += sprintf(buf + pos, ", %p) ", (void *)info->args[1]);
  format_ret(buf + pos, info->rval);
}

/* Format: clock_nanosleep */
void format_clock_nanosleep(char *buf, tracee_state *t, syscall_info *info)
{
  int pos = sprintf(buf, "clock_nanosleep(%ld, %ld, ", info->args[0], info->args[1]);
  
  struct timespec *ts = (struct timespec *)t->read_snapshot_mem(
      info->args[2], sizeof(struct timespec));
  if (ts && info->rval >= 0) {
    pos += sprintf(buf + pos, "{tv_sec=%ld, tv_nsec=%ld}", ts->tv_sec, ts->tv_nsec);
    free(ts);
  } else {
    pos += sprintf(buf + pos, "%p", (void *)info->args[2]);
    if (ts) free(ts);
  }
  
  pos += sprintf(buf + pos, ", %p) ", (void *)info->args[3]);
  format_ret(buf + pos, info->rval);
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

  int pos = sprintf(buf, "socket(%s, %s, %ld) ", domain_str, type_str, info->args[2]);
  format_ret(buf + pos, info->rval);
}

/* Format: bind with sockaddr details */
void format_bind(char *buf, tracee_state *t, syscall_info *info)
{
  int pos = sprintf(buf, "bind(%ld, ", info->args[0]);
  
  /* Read sockaddr */
  struct sockaddr_in *addr = (struct sockaddr_in *)t->read_snapshot_mem(
      info->args[1], info->args[2]);
  if (addr && info->rval >= 0) {
    pos += format_sockaddr(buf + pos, addr);
    free(addr);
  } else {
    pos += sprintf(buf + pos, "%p", (void *)info->args[1]);
    if (addr) free(addr);
  }
  
  pos += sprintf(buf + pos, ", %ld) ", info->args[2]);
  format_ret(buf + pos, info->rval);
}

/* Format: listen/accept/connect */
void format_socket_fd(char *buf, syscall_info *info)
{
  int pos = sprintf(buf, "%s(%ld, ...) ", syscalls[info->nr], info->args[0]);
  format_ret(buf + pos, info->rval);
}

/* Format: default (6 arguments hex) */
void format_default(char *buf, syscall_info *info)
{
  int pos = sprintf(buf, "%s(0x%lx, 0x%lx, 0x%lx, 0x%lx, 0x%lx, 0x%lx) ",
          syscalls[info->nr], info->args[0], info->args[1], info->args[2],
          info->args[3], info->args[4], info->args[5]);
  format_ret(buf + pos, info->rval);
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
    case SYS_poll:
      format_poll(buf, info);
      break;
    case SYS_socket:
      format_socket(buf, info);
      break;
    case SYS_bind:
      format_bind(buf, t, info);
      break;
    case SYS_listen:
    case SYS_accept:
    case SYS_accept4:
    case SYS_connect:
      format_socket_fd(buf, info);
      break;

    /* File I/O */
    case SYS_read:
      format_read(buf, t, info);
      break;
    case SYS_write:
      format_write(buf, t, info);
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
