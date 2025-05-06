#include "guest.h"
#include "emu.h"
#include "monitor.h"
#include "tests/raft/include/raft.h"
#include "tests/raft/include/raft_private.h"

static struct timeval tv_addmsec(struct timeval tv, int msec) {
  long sec = tv.tv_sec;
  long usec = tv.tv_usec;
  usec += msec * 1000;
  sec += usec / 1000000;
  usec %= 1000000;
  return (struct timeval){ sec, usec };
}

static struct timeval choose(struct timeval tv) {
  int pid = ptmc_state.pids[ptmc_state.cursor];
  long praft = tracee_read_word(pid, (void *)get_var_addr("raft"));
  raft_server_private_t raft;
  tracee_read_mem(pid, (void *)praft, &raft, sizeof(raft));

  assert(ptmc_state.choose >= 0 && ptmc_state.choose < ptmc_state.n_choose);

  if (raft.state == RAFT_STATE_LEADER)
  {
    if (ptmc_state.choose == 0) 
      return tv_addmsec(tv, raft.request_timeout + 1);
    else 
      return tv_addmsec(tv, raft.request_timeout - 1);
  }
  else 
  {
    if (ptmc_state.choose == 0) 
      return tv_addmsec(tv, raft.election_timeout_rand + 1);
    else 
      return tv_addmsec(tv, raft.election_timeout_rand - 1);
  }
}

int emu_gettimeofday(struct timeval *tv, 
                     struct timezone *tz) {
  /* Ignore tz */
  int index = ptmc_state.cursor;
  struct timeval tracee_tv = ptmc_state.time[index];

  if (ptmc_state.n_choose == 0) {
    assert(0);
    memcpy_host2guest(tv, &tracee_tv, sizeof(struct timeval));
    return 0;
  }
  struct timeval ret = choose(tracee_tv);
  memcpy_host2guest(tv, &ret, sizeof(struct timeval));
  ptmc_state.time[index] = ret;

  return 0;
}
