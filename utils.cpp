#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/ptrace.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include "debug.h"
#include "common.h"

/* ptrace with error check */
long ptrace_right(enum __ptrace_request op, pid_t pid, void *addr, void *data) {
  /* ptrace(2): On success, the PTRACE_PEEK* operations return the requested *
   * data ... and other operations return zero. On error, all operations     *
   * return -1, and errno is set to indicate the error. Since the value      *
   * returned by a successful PTRACE_PEEK* operation may be -1, the caller   *
   * must clear errno before the call, and then check it ...                 */
  errno = 0;
  int result = ptrace(op, pid, addr, data);
  if (errno)
    panic("ptrace: %s", strerror(errno));
  return result;
}
