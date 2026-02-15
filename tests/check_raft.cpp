#include "common.h"
#include "guest.h"
#include "monitor.h"
#include <stdio.h>

extern "C"
{
#include "tests/raft/include/raft.h"
#include "tests/raft/include/raft_private.h"

  int check()
  {
    uint64_t addr = get_var_addr("raft");
    raft_server_private_t raft[NP];
    int leader_term = -1;
    int leader = -1;
    for (int i = 0; i < NP; i++)
    {
      long praft = tracee_read_word(ptmc_state.pids[i], (void *)addr);
      if (praft == 0)
        continue;
      tracee_read_mem(ptmc_state.pids[i], (const void *)praft, raft + i,
                      sizeof(raft_server_private_t));
      if (raft[i].state == RAFT_STATE_LEADER)
      {
        printf("leader = %d, leader_term = %ld\n", i, raft[i].current_term);
        if (leader == -1 || leader_term < raft[i].current_term)
        {
          leader = i;
          leader_term = raft[i].current_term;
        }
        else if (leader_term == raft[i].current_term)
        {
          printf("node %d and %d are both leader of term %d\n", leader, i,
                 leader_term);
          return 1;
        }
      }
    }
    return 0;
  }
}
