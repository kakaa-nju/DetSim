#ifndef __MONITOR_H
#define __MONITOR_H

#include "common.h"
#include "sockstate.h"
#include "state.h"
#include "engine.h"
#include <string>

enum {
  PTMC_PRELOAD, PTMC_LOADED, PTMC_STOP, PTMC_RUNNING, PTMC_END, PTMC_ABORT, PTMC_QUIT 
};

typedef struct {
  int state;
  int cursor;
  int n_choose;
  int choose;
  sys_state source_state;
  sys_state dest_state;
  hash_type sysstate_hash; /* state in operating system */
  hash_type toload;

  int exited[NP];
  std::list<ptmc_sock> sock_lists[NP];
  std::unordered_map<int, tcp_buffer> tcp_buffer_lists[NP];
  std::unordered_map<int, udp_buffer> udp_buffer_lists[NP];

  int pids[NP];

  struct {
    char *executable;
    int argc;
    char *argv[5];
  } tracee[NP];

  struct timeval time[NP];
  std::string addrs[NP];
  std::unordered_set<std::string> shared_files;
  std::unordered_set<std::string> proc_files[NP];

  std::unordered_set<std::string> assertions;
  std::vector<int (*)()> user_checks;
} PTMC_STATE;

int is_auto_mode();

extern PTMC_STATE ptmc_state;

#endif /* __MONITOR_H */
