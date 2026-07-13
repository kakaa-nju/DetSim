/*
 * thread_manager.h - Thread lifecycle and scheduling management
 *
 * This module provides centralized thread management for the simulation engine:
 * - Thread lifecycle (creation, exit, cleanup)
 * - Thread state tracking (running, blocked, exited)
 * - Thread scheduling (round-robin selection)
 * - Thread recovery (for state restore)
 */

#ifndef __THREAD_MANAGER_H
#define __THREAD_MANAGER_H

#include "types.h"
#include <memory>
#include <sys/types.h>
#include <vector>

// Maximum threads per process
constexpr int MAX_THREADS_PER_PROCESS = 64;

// Forward declarations
struct tracee_state;
struct PTMC_STATE;

/* ======================================================================
 * Thread State Enumeration
 * ====================================================================== */

enum class ThreadState
{
  RUNNING, // Thread is ready to execute
  BLOCKED, // Thread is blocked (e.g., on futex)
  EXITED,  // Thread has exited
  ZOMBIE   // Thread exited but not yet reaped
};

/* ======================================================================
 * Per-Thread Scheduling Information
 * ====================================================================== */

struct ThreadSchedInfo
{
  int thread_idx;      // Index in thread array (0-based)
  pid_t virtual_tid;   // Virtual TID (index + 1, stable across saves)
  pid_t physical_tid;  // Actual kernel TID (may change on restore)
  ThreadState state;   // Current state
  uint64_t blocked_on; // If BLOCKED, what address (for futex)

  ThreadSchedInfo()
      : thread_idx(0), virtual_tid(0), physical_tid(0),
        state(ThreadState::RUNNING), blocked_on(0)
  {
  }
};

/* ======================================================================
 * Thread Manager - Per-Process Thread Management
 * ====================================================================== */

class ThreadManager
{
  public:
  ThreadManager();
  ~ThreadManager() = default;

  // Non-copyable (each process has its own manager)
  ThreadManager(const ThreadManager &) = delete;
  ThreadManager &operator=(const ThreadManager &) = delete;

  // Movable
  ThreadManager(ThreadManager &&other) noexcept;
  ThreadManager &operator=(ThreadManager &&other) noexcept;

  /* ------------------------------------------------------------------
   * Thread Lifecycle
   * ------------------------------------------------------------------ */

  // Add a new thread (called on clone/clone3)
  // Returns the new thread's index, or -1 on failure
  int add_thread(pid_t physical_tid, uint64_t clone_flags, uint64_t stack_addr,
                 pid_t parent_tid, bool is_main = false);

  // Mark a thread as exited (called on SYS_exit)
  // Returns true if the thread was found and marked, false otherwise
  bool mark_exited(pid_t physical_tid);

  // Remove a thread completely (used during cleanup)
  void remove_thread(pid_t virtual_tid);

  // Clear all threads (used during state reset)
  void clear();

  /* ------------------------------------------------------------------
   * Thread State Queries
   * ------------------------------------------------------------------ */

  // Get the number of threads
  size_t thread_count() const { return threads_.size(); }

  // Get number of non-exited threads
  size_t live_thread_count() const;

  // Get number of runnable (not blocked, not exited) threads
  size_t runnable_thread_count() const;

  // Check if any threads are runnable
  bool has_runnable_threads() const;

  // Check if all threads have exited
  bool all_threads_exited() const;

  // Get thread info by index
  const ThreadSchedInfo *get_thread_info(int thread_idx) const;

  // Find thread index by physical TID
  int find_thread_index(pid_t physical_tid) const;

  // Find thread index by virtual TID
  int find_thread_index_by_vtid(pid_t virtual_tid) const;

  /* ------------------------------------------------------------------
   * Thread State Management
   * ------------------------------------------------------------------ */

  // Mark a thread as blocked (e.g., waiting on futex)
  void mark_blocked(int thread_idx, uint64_t blocked_on = 0);

  // Mark a thread as unblocked (e.g., futex wake)
  void mark_unblocked(int thread_idx);

  // Update physical TID (used during state restore)
  void update_physical_tid(int thread_idx, pid_t new_physical_tid);

  /* ------------------------------------------------------------------
   * Thread Scheduling
   * ------------------------------------------------------------------ */

  // Get current thread index
  int current_thread() const { return current_thread_idx_; }

  // Set current thread index
  void set_current_thread(int thread_idx);

  // Get physical TID of current thread
  pid_t get_current_physical_tid() const;

  // Get virtual TID of current thread
  pid_t get_current_virtual_tid() const;

  // Advance to next runnable thread (round-robin)
  // Returns the index of the next runnable thread, or -1 if none
  int next_runnable_thread();

  // Find next runnable thread without changing current
  int peek_next_runnable_thread() const;

  // Get list of all runnable thread indices (for multi-threaded BFS)
  std::vector<int> get_runnable_threads() const;

  /* ------------------------------------------------------------------
   * Integration with tracee_state
   * ------------------------------------------------------------------ */

  // Synchronize from tracee_state (used during state restore)
  void sync_from_tracee_state(const tracee_state &state);

  // Synchronize to tracee_state (used during state save)
  void sync_to_tracee_state(tracee_state &state) const;

  /* ------------------------------------------------------------------
   * Debug/Info
   * ------------------------------------------------------------------ */

  void dump_state() const;

  // Get a summary string for display
  std::string get_summary() const;

  private:
  std::vector<ThreadSchedInfo> threads_;
  int current_thread_idx_;

  // Helper: Check if thread index is valid and runnable
  bool is_runnable(int thread_idx) const;

  // Helper: Find next runnable thread starting from given index
  int find_next_runnable(int start_idx) const;
};

/* ======================================================================
 * Global Thread Management Helpers
 * ====================================================================== */

namespace thread
{

// Initialize thread managers for all processes
void init_all(PTMC_STATE &state);

// Get thread manager for a specific process
ThreadManager *get_manager(int tracee_idx);

// Set thread manager for a specific process
void set_manager(int tracee_idx, std::unique_ptr<ThreadManager> manager);

// Clean up all thread managers
void cleanup_all();

} // namespace thread

#endif /* __THREAD_MANAGER_H */
