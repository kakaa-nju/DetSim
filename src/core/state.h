/*
 * state.h - State management
 */

#ifndef __STATE_H
#define __STATE_H

#include "fsstate.h"
#include "sockstate.h"
#include <sys/user.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

/* ======================================================================
 * State Data Format (shared with StateStore)
 * ====================================================================== */

struct StateDataHeader
{
  uint32_t magic;         // 'DSTM' = 0x4453544D
  uint32_t version;       // 3 (updated for integrated mappings)
  uint32_t num_regions;   // number of memory regions
  uint64_t tstate_offset; // offset to serialized tracee_state
  uint64_t tstate_size;   // size of serialized tracee_state
  uint64_t maps_offset;   // offset to mappings data (text format)
  uint64_t maps_size;     // size of mappings data
  uint64_t regs_offset;   // offset to register data
};

struct RegionInfo
{
  uint64_t start;
  uint64_t end;
  uint64_t offset; // offset in data buffer
  uint32_t prot;   // protection flags (PROT_READ/WRITE/EXEC)
  char flags[5];   // original flags string from /proc/pid/maps (e.g., "rwxp")
};

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

/* Raft-specific state stored per tracee for safety checking */
struct raft_check_state
{
  long current_term = 0;
  int is_leader = 0;
  long last_log_term = 0;

  /* Extended state for comprehensive bug detection */
  long commit_idx = 0;
  long applied_idx = 0;
  long last_idx = 0;

  /* Vote integrity checking */
  long voted_for = -1;

  /* Leader state tracking */
  long match_idx[3] = {0, 0, 0};
  long next_idx[3] = {0, 0, 0};

  template <class Archive>
  void serialize(Archive &ar)
  {
    ar(current_term, is_leader, last_log_term, commit_idx, applied_idx,
       last_idx, voted_for, match_idx[0], match_idx[1], match_idx[2],
       next_idx[0], next_idx[1], next_idx[2]);
  }
};

typedef struct tracee_state
{
  hash_type ts_hash;
  /* about syscall_info: here the syscall indicates the last DONE syscall */
  syscall_info si;

  int pid;
  uintptr_t brk;
  struct timeval tv;
  FileSystemState fs_state;
  SockState sock_state;

  /* Raft consensus checking state - stored per state for BFS correctness */
  raft_check_state raft_state;

  /* FdManager state - saved to preserve fd allocation state */
  FdManager fd_manager_state;

  /* ---------------------------------------------------------
   * Constructors
   * --------------------------------------------------------- */

  /* From running process */
  tracee_state(int which, struct syscall_info *info);

  /* From saved state (hash) */
  tracee_state(hash_type hash);

  // Default constructor - value-initialize all members
  tracee_state()
      : ts_hash(0), si{}, pid(0), brk(0), tv{0, 0}, fs_state(), sock_state(),
        raft_state()
  {
  }

  ~tracee_state() = default;

  // Copy control
  tracee_state(const tracee_state &other) = default;
  tracee_state &operator=(const tracee_state &other) = default;

  // Move control - explicitly default
  tracee_state(tracee_state &&other) noexcept = default;
  tracee_state &operator=(tracee_state &&other) noexcept = default;

  // Explicit memory cleanup - call after copying to ptmc_state to free source
  void clear();

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

  /* Memory Access
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
  int status[NP];

  // Default constructor - value-initialize all members
  sys_state() : ss_hash(0), ts_hash{}, child{}, status{} {}

  // Copy control
  sys_state(const sys_state &other) = default;
  sys_state &operator=(const sys_state &other) = default;

  // Move control - explicitly default
  sys_state(sys_state &&other) noexcept = default;
  sys_state &operator=(sys_state &&other) noexcept = default;

  /* From running processes */
  sys_state(struct syscall_info *info);

  /* From saved state (hash) */
  sys_state(hash_type hash);

  ~sys_state(){};

  // Explicit memory cleanup - call after copying to ptmc_state to free source
  void clear();

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
bool mapping_exists(maps_item &a, std::vector<maps_item> &array);

/* ======================================================================
 * Statistics
 * ====================================================================== */

void state_stats_print();

/* ======================================================================
 * Global Cleanup
 * ====================================================================== */

void cleanup_all();

#endif /* __STATE_H */
