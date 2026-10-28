/*
 * scheduler.h - Simulation scheduler and execution engine
 * 
 * This module is responsible for:
 * - Process initialization and execution control
 * - System call interception and dispatch
 * - State exploration (single step, continue, load/store)
 * - State tree management
 */

#ifndef __SCHEDULER_H
#define __SCHEDULER_H

#include "types.h"
#include <sys/types.h>
#include <vector>
#include <functional>
#include <cstdint>

/* Forward declarations */
struct syscall_info;
struct sys_state;
enum class TransitionAction;
struct ActorAction;
struct TransitionResult;

/* ========================================================================
 * Initialization
 * ======================================================================== */

/* Initialize tracee processes and get initial state */
void init_state(void);

/* ========================================================================
 * Execution Control
 * ======================================================================== */

/* Execute a single system call and record its effect */
int do_one_syscall(pid_t pid, syscall_info *si);

/* Execute one step (one syscall) of the selected process */
int exec_once(syscall_info *info);

/* Execute and store the resulting state */
int exec_store(void);

/* Load state and continue execution */
int load_exec_store(TransitionAction action);

/* Continue execution in auto mode */
int exec_bfs(void);

/* Perform depth-first search from current state */
int exec_dfs(int depth);

/* Perform random search from current state */
int exec_rand(int depth);

/* Load a saved state by hash */
void load(hash_type hash);

// Streaming single-source expansion for persistent native workers. Every actual
// actor/choice edge is delivered, including duplicate target hashes. The result
// and selected choice remain valid for the callback; retain no live references.
enum class EnumerationKind { Expanded, Terminal, DepthCut, ModelBudget, Violation };
struct EnumerationSummary { EnumerationKind kind; uint64_t emitted; };
EnumerationKind classify_enumeration(const sys_state& source,
    const std::vector<ActorAction>& actors, bool may_expand);
using EdgeCallback = std::function<void(const ActorAction&, int, int, const TransitionResult&)>;
EnumerationSummary enumerate_state(const sys_state& source, bool may_expand,
                                   const EdgeCallback& callback);

/* ========================================================================
 * State Management
 * ======================================================================== */

/* Show syscall execution history */
void show_syscall_history(void);

/* Extract one complete syscall entry+exit pair */
syscall_info extract_one_syscall(pid_t pid);

#endif /* __SCHEDULER_H */
