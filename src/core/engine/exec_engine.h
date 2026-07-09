/*
 * exec_engine.h - Integrated execution engine
 *
 * This module integrates ThreadManager, SignalHandler, and SyscallDispatcher
 * to provide a clean execution interface for the scheduler.
 */

#ifndef __EXEC_ENGINE_H
#define __EXEC_ENGINE_H

#include "types.h"
#include <sys/types.h>

// Forward declarations
struct syscall_info;
struct sys_state;

/* ======================================================================
 * Execution Result
 * ====================================================================== */

enum class ExecStatus {
    SUCCESS,        // Syscall executed, state saved
    NO_CHECKPOINT,  // Syscall executed, no checkpoint needed
    EXIT,           // Process exited
    CRASH,          // Process crashed
    DISCARD,        // Execution path discarded (blocked thread)
    ERROR           // Unexpected error
};

struct ExecResult {
    ExecStatus status;
    int checkpoint_decision;  // CKPT_YES, CKPT_NO, etc.
    const char* message;

    ExecResult(ExecStatus s = ExecStatus::SUCCESS, int ckpt = 0, const char* msg = "")
        : status(s), checkpoint_decision(ckpt), message(msg) {}
};

/* ======================================================================
 * Execution Engine
 * ====================================================================== */

class ExecutionEngine {
public:
    ExecutionEngine();
    ~ExecutionEngine() = default;

    // Initialize for a tracee
    void init_tracee(int tracee_idx);

    // Execute one syscall on the current thread
    // Returns checkpoint decision
    int execute_syscall(pid_t pid, syscall_info& info);

    // Execute one step with thread scheduling
    // This is the main entry point for exec_once
    ExecResult execute_step(const sys_state& state, syscall_info& info);

    // Handle thread creation (clone/clone3)
    void on_thread_created(pid_t parent_pid, pid_t new_tid,
                          uint64_t clone_flags, uint64_t stack_addr);

    // Handle thread exit
    bool on_thread_exit(pid_t pid, pid_t wait_result, int wstatus, syscall_info& si);

    // Get current thread info
    pid_t get_current_tid(int tracee_idx) const;
    int get_current_thread_idx(int tracee_idx) const;

    // Check if any threads are runnable
    bool has_runnable_threads(int tracee_idx) const;

    // Check if all threads have exited
    bool all_threads_exited(int tracee_idx) const;

    // Advance to next runnable thread
    bool advance_to_next_thread(int tracee_idx);

private:
    // Internal: execute single syscall without thread switching
    int do_single_syscall(pid_t pid, syscall_info& info);

    // Internal: handle signals during syscall execution
    bool handle_signal(pid_t pid, int wstatus);
};

/* ======================================================================
 * Global Execution Engine
 * ====================================================================== */

namespace exec {

// Initialize execution engine for all tracees
void init_all();

// Get engine instance
ExecutionEngine* get_engine();

// Cleanup
void cleanup();

} // namespace exec

#endif /* __EXEC_ENGINE_H */
