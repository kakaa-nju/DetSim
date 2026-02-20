#include "common.h"
#include "guest.h"
#include "monitor.h"
#include "utils/expr_eval.hpp"
#include <stdio.h>
#include <stdint.h>

extern "C"
{

  int check()
  {
    void *raft = eval<void *>("tracee1(raft)");
    if (raft == NULL) return 0;
    long term = eval<long>("tracee1(((raft_server_private_t *)raft)->current_term)");
    if (term > 0) return 1;
    return 0;
  }
}
