/*
 * state.h - State management
 */

#ifndef __STATE_H
#define __STATE_H

#include "common.h"
#include <sys/user.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <list>
#include "sockstate.h"
#include "fsstate.h"

/* ======================================================================
 * Syscall Info Structure
 * ====================================================================== */

struct syscall_info
{
  int nr;
  /* only modified parameters will have value */
  uintptr_t rval;
  uintptr_t args[6];

  template <class Archive>
  void serialize(Archive &ar);
};

/* ======================================================================
 * Tracee State (Single Process State)
 * ====================================================================== */

typedef struct tracee_state
{
  hash_type ts_hash;
  /* about syscall_info: here the syscall indicates the last DONE syscall */
  syscall_info si;

  int pid;
  uintptr_t brk;
  struct timeval tv;
  FileSystemState fs_state;
  std::list<ptmc_sock> sock_list;

  std::unordered_map<int, tcp_buffer> tcp_buffer_list;
  std::unordered_map<int, udp_buffer> udp_buffer_list;

  /* ---------------------------------------------------------
   * Constructors
   * --------------------------------------------------------- */

  /* From running process */
  tracee_state(int which, struct syscall_info *info);

  /* From saved state (hash) */
  tracee_state(hash_type hash);

  /* Default constructor */
  tracee_state()
  {
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    pid = 0;
  }
  ~tracee_state() {}

  /* ---------------------------------------------------------
   * State Capture (running -> struct -> disk)
   * --------------------------------------------------------- */

  /* Full state save: memory, mappings, serialization */
  void save_full_state();

  /* Capture memory and registers */
  void capture_memory_state();

  /* Save memory mappings */
  void save_mappings();

  /* Serialize to stream (for memory dump) */
  void serialize_to_stream(FILE *fp);

  /* Serialize to disk file */
  void serialize_to_disk();

  /* Save process files */
  void save_proc_files();

  /* Get file descriptors */
  void get_file_descriptors();

  /* ---------------------------------------------------------
   * State Recovery (disk -> struct -> running)
   * --------------------------------------------------------- */

  /* Main recovery entry */
  void recover_running_state(int index);

  /* Restore memory mappings for brk */
  void restore_memory_mappings(std::vector<maps_item> &maps_out);

  /* Recover file descriptors */
  void recover_file_descriptors(int index);

  /* Recover memory and registers */
  void recover_mem_reg_snapshot(std::vector<maps_item> &maps);

  /* Recover process files */
  void recover_proc_files();

  /* ---------------------------------------------------------
   * Memory Access
   * --------------------------------------------------------- */

  /* Read memory from snapshot */
  void *read_snapshot_mem(uint64_t addr, long size);

  /* ---------------------------------------------------------
   * Display
   * --------------------------------------------------------- */

  void show_syscall(syscall_info *info);

  /* ---------------------------------------------------------
   * Serialization
   * --------------------------------------------------------- */
  template <class Archive>
  void serialize(Archive &ar);

} tracee_state;

/* ======================================================================
 * System State (Global State for All Processes)
 * ====================================================================== */

typedef struct sys_state
{
  hash_type ss_hash;
  hash_type ts_hash[NP];
  tracee_state child[NP];
  int exited[NP];

  sys_state(){}; // dummy

  /* From running processes */
  sys_state(struct syscall_info *info);

  /* From saved state (hash) */
  sys_state(hash_type hash);

  ~sys_state(){};

  /* ---------------------------------------------------------
   * Persistence
   * --------------------------------------------------------- */

  int save_metadata();
  void save_shared_files();

  /* ---------------------------------------------------------
   * Recovery
   * --------------------------------------------------------- */

  void recover_running_state();
  void recover_shared_files();

  /* ---------------------------------------------------------
   * Serialization
   * --------------------------------------------------------- */
  template <class Archive>
  void serialize(Archive &ar);

} sys_state;

/* ======================================================================
 * State Collections
 * ====================================================================== */

/* State tree: child -> (parent, which, choose) */
typedef std::unordered_map<hash_type, std::tuple<hash_type, int, int>> TSS;

/* State queue (BFS exploration) */
typedef std::deque<hash_type> LSS;

/* State set (deduplication) */
typedef std::unordered_set<hash_type> SSS;

/* ======================================================================
 * Queue Management
 * ====================================================================== */

void state_queue_append(sys_state *s);
void state_queue_append_front(sys_state *s);
sys_state *state_queue_extract();

/* ======================================================================
 * Tree Management
 * ====================================================================== */

void state_tree_add(sys_state *s, sys_state *t, int which, int choose);

/* ======================================================================
 * History Display
 * ====================================================================== */

void show_syscall_history();

/* ======================================================================
 * Memory Operations
 * ====================================================================== */

void *read_mem(hash_type ts_hash, int pid, uint64_t addr, long size);

/* ======================================================================
 * Helper Functions
 * ====================================================================== */

void get_maps_item(std::vector<maps_item> &items, FILE *maps);
bool mapping_exists(maps_item &a, std::vector<maps_item> array);

/* ======================================================================
 * Statistics
 * ====================================================================== */

void state_stats_print();

#endif /* __STATE_H */
