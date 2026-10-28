#ifndef __EMU_H
#define __EMU_H

#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/time.h>

/* Inputs a choose_<syscall> plugin may inspect for one enumerated outcome. */
typedef struct choose_in
{
  union
  {
    struct timeval gettimeofday_now;
    struct
    {
      struct pollfd *fds;
      nfds_t nfds;
      int timeout;
    } poll;
  };

  choose_in(struct timeval tv) : gettimeofday_now(tv) {}

  choose_in(struct pollfd *poll_fds, nfds_t poll_nfds, int poll_timeout)
  {
    poll.fds = poll_fds;
    poll.nfds = poll_nfds;
    poll.timeout = poll_timeout;
  }
} choose_in;

/* Values one enumerated outcome writes back; len[i] == 0 means "unmodified". */
typedef struct choose_out
{
  void *args[6];
  int len[6];
  int rval;

  choose_out() = default;

  ~choose_out()
  {
    for (int i = 0; i < 6; i++)
    {
      if (len[i])
        free(args[i]);
    }
  }
} choose_out;

typedef choose_out *(*choose_func)(int pid, int choice, const choose_in &in);

/* Per-syscall outcomes of the configured ChoosePoint entries. */
extern int choose_many[450];
/* Resolved choose_<syscall> symbols of the configured ChooseFunc plugin. */
extern choose_func choose_syswhat[450];

int emu_gettimeofday(struct timeval *tv, struct timezone *tz);

#endif /* __EMU_H */
