#ifndef __STATE_H
#define __STATE_H
#include "common.h"
#include "fsstate.h"
#include "guest.h"
#include "sockstate.h"
#include <deque>
#include <list>
#include <stdbool.h>
#include <stdint.h>
#include <string>
#include <sys/user.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

typedef uint32_t hash_type;
#define HASH_FORMAT "%08x"

/* exec state */

struct syscall_info
{
  int nr;
  /* only modified parameters will have value */
  uintptr_t rval;
  uintptr_t args[6];

  template <class Archive>
  void serialize(Archive &ar);
};

typedef struct tracee_state
{
  hash_type ts_hash;
  /* about syscall_info: *
   * here the syscall indicates the last DONE syscall */
  syscall_info si;

  int pid;
  uintptr_t brk;
  struct timeval tv;
  std::list<ptmc_filedesc> fd_list;
  std::list<ptmc_sock> sock_list;

  std::unordered_map<int, tcp_buffer> tcp_buffer_list;
  std::unordered_map<int, udp_buffer> udp_buffer_list;
  /* --------------------------------------------------------- */
  /* running -> struct (Constructor) *
   * Analyze running state, and record important information *
   * (mainly system resources) into structure. No disk write *
   * included. */
  tracee_state(int which, struct syscall_info *info);

  void get_file_descriptors();
  /* --------------------------------------------------------- */
  /* struct -> fs *
   * 1. Dump memory & register. *
   * 2. Calculate MD5, set ts_hash. *
   * 3. Dump structure data, filesystem, mappings and manage *
   *    data file */
  void save_proc_full_data();
  /* May need functions below: */

  /* set MD5 as ts_hash */
  void create_mem_reg_snapshot();

  /* Save structure */
  void save_structure_data(FILE *fp);
  void save_structure_data();

  /* Save mappings */
  void save_mappings();

  /* Save files */
  void save_proc_files();

  /* --------------------------------------------------------- */

  /* fs -> struct (Constructor) *
   * Read struct from tstate/#tshash.ts, then construct the *
   * structure. */
  tracee_state(hash_type hash);

  /* --------------------------------------------------------- */
  /* struct -> running *
   * 1. Copy memory & register dump to process. *
   * 2. Apply data in structure. */
  void recover_running_state(int index);

  std::vector<maps_item> recover_brk_mappings();

  void recover_proc_files();

  void recover_file_descriptors(int index);

  void recover_mem_reg_snapshot(std::vector<maps_item> &maps);

  /* --------------------------------------------------------- */
  template <class Archive>
  void serialize(Archive &ar);

  /* read from tracee snapshot */
  void *read_snapshot_mem(uint64_t addr, long size);

  void show_syscall(syscall_info *info);

  /* default is enough */
  tracee_state()
  {
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    pid = 0;
  }
  ~tracee_state() {}
} tracee_state;

typedef struct sys_state
{
  hash_type ss_hash; // different sys_state may share same traceeState
  hash_type ts_hash[NP];
  tracee_state child[NP];
  int exited[NP];

  sys_state(){}; // dummy

  /* running -> struct *
   * Construct sys_state struct from halt processes. *
   * Will call tracee_state::tracee_state(). *
   * Also calculates ss_hash, which needs ts_hash. *
   * So here tracee_state should already be dumped *
   * by calling tracee_state::save_proc_full_data */
  sys_state(struct syscall_info *info);

  /* fs -> struct *
   * Construct sys_state from dump file indicated by hash. *
   * Just read metadata. */
  sys_state(hash_type hash);

  template <class Archive>
  void serialize(Archive &ar);

  /* struct -> fs *
   * Store sys_state information into file. Some metadata *
   * containing ts_hash'es, and if exited. */
  int save_metadata();
  void save_shared_files();

  /* struct -> running *
   * Recover running state from metadata in struct, *
   * which will call tracee_state::recover_running_state */
  void recover_running_state();
  void recover_shared_files();

  /* default is enough */
  ~sys_state(){};
} sys_state;

/* f: child state -> (parent state, which, choose) */
typedef std::unordered_map<hash_type, std::tuple<hash_type, int, int>> TSS;
typedef std::deque<hash_type> LSS;
typedef std::unordered_set<hash_type> SSS;

void state_queue_append(sys_state *s);
void state_queue_append_front(sys_state *s);

sys_state *state_queue_extract();

void state_tree_add(sys_state *s, sys_state *t, int which, int choose);

#endif /* __STATE_H */
