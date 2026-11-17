#ifndef DETSIM_THREAD_POOL_H
#define DETSIM_THREAD_POOL_H

#include <cstdint>
#include <sys/types.h>

struct tracee_state;

pid_t logical_process_id(int node);
pid_t logical_thread_id(int node, pid_t slot);

// Supported Linux shared-address-space thread profile. PI robust ownership,
// rseq and asynchronous signal delivery remain unsupported.
struct CloneRequest {
  uint64_t flags = 0, stack = 0, tls = 0, parent_tid = 0, child_tid = 0;
};
pid_t create_logical_thread(int node, pid_t parent, const CloneRequest& request);
// Run after dirtying the image and before the selected child resumes user code.
void complete_thread_start(int node, pid_t physical_tid);
pid_t record_logical_clear_tid(int node, pid_t physical_tid, uint64_t address);
// Bootstrap only, after all nodes are stopped and scratch memory is installed.
// Transfer a physical registration into model state, then disable it in Linux.
void adopt_clear_child_tid(int node, pid_t physical_tid);
void adopt_thread_signal_mask(int node, pid_t physical_tid);
void adopt_thread_robust_list(int node, pid_t physical_tid);
long emulate_set_robust_list(int node, pid_t physical_tid, uint64_t head, uint64_t length);
long emulate_get_robust_list(int node, pid_t caller, int32_t target,
                            uint64_t head_out, uint64_t length_out);
long emulate_thread_sigprocmask(int node, pid_t physical_tid, uint64_t how,
                               uint64_t new_mask, uint64_t old_mask, uint64_t size);
long emulate_thread_sigaltstack(int node, pid_t physical_tid, uint64_t new_stack, uint64_t old_stack);
void restore_thread_signal_masks(int node, const tracee_state& state);
void validate_logical_clone(uint64_t flags);
pid_t create_logical_thread(int node, pid_t parent, uint64_t flags,
                            uint64_t stack, uint64_t tls);
bool exit_logical_thread(int node, pid_t physical_tid, int exit_code);
void exit_logical_process(int node, pid_t caller, int exit_code);

// Pool bookkeeping is runtime-only. Initial registration accepts exactly the
// captured owned slots; subsequent barriers include all parked carriers.
void prepare_thread_pool(int node, const tracee_state& state);
/* Create parked carriers so an adopted saved thread layout can bind. */
void ensure_thread_carriers(int node, size_t count);
void verify_thread_pool(int node);
void require_thread_capture_boundary(pid_t tid);
void normalize_thread_pool_for_restore(int node);
void bind_thread_carriers(int node, tracee_state& state);
void clear_thread_pools();

/* Stop-barrier epochs.
 *
 * A full `verify_thread_pool()` proves one stopped cut: the owned thread set
 * matches the model and every carrier is parked at a syscall boundary with the
 * captured signal mask. That proof stays valid until a task of the node is
 * resumed or its stop state is changed by ptrace, so an exploration cycle that
 * restores every node, runs exactly one syscall on the cursor, and then
 * captures every node does not have to buy the same barrier twice.
 *
 * `note_task_change()` is called from the only places that can resume a task or
 * rewrite its stop state (Ptrace_right, ThreadCarrier control, PTRACE_SETSIGMASK,
 * the rollback resume). An unknown tid voids every node, so a missed ownership
 * lookup can only cost the optimisation, never correctness.
 *
 * Modes: `on` (default) skips a verified-intact barrier, `off` always verifies,
 * `verify` always verifies *and* audits that the intact claim held. A search run
 * in `verify` mode therefore proves, transition by transition, that no skipped
 * barrier would have rejected its cut.
 */
namespace detsim::threadbarrier {
enum class Mode { Off, On, Verify };
Mode mode();
/* Full verification happened and nothing resumed since. */
void commit(int node);
void note_task_change(pid_t tid);
/* True when the last committed barrier for `node` is still valid. */
bool intact(int node);
void clear();
}

#endif
