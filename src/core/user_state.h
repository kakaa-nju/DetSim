/*
 * user_state.h - User-defined state plugin interface
 *
 * DetSim already checkpoints and restores the state of the traced programs
 * (memory, registers, file descriptors, sockets, ...).  Some checks, however,
 * need extra "shadow" state that is derived from the guest but is not part of
 * DetSim's own bookkeeping.  The Raft checker, for example, remembers the
 * previous term / commit index / vote of every node so that it can verify
 * monotonicity across a state transition.
 *
 * This header defines an optional plugin (loaded from a shared object just
 * like "UserCheck" and "ChooseFunc") whose custom state is captured together
 * with the program state, serialized into the checkpoint, restored on load,
 * and can be inspected from the regular check() function.
 *
 * Configuration (JSON):
 *
 *   "UserState": "./plugins/check.so"
 *
 * The plugin may export any subset of the hooks below.  A missing hook is
 * treated as "no state" / no-op.  DetSim resolves the hooks with dlsym() when
 * the plugin is loaded; the plugin does not need to link against DetSim.
 *
 * Hook contract
 * -------------
 *   user_state_size(index):
 *       Return the number of bytes required to represent the current custom
 *       state of tracee `index`.  Return <= 0 when there is nothing to save.
 *       Called for every tracee whenever a checkpoint is written.
 *
 *   user_state_save(index, buf):
 *       Serialize the custom state of tracee `index` into `buf`.  `buf` is at
 *       least user_state_size(index) bytes and is owned by DetSim.
 *
 *   user_state_load(index, buf, size):
 *       Restore the custom state of tracee `index` from `buf`.  Called
 *       whenever a checkpoint is restored.  `size` is the size that was
 *       returned by user_state_size() when the checkpoint was written.
 *
 * All three hooks are invoked while DetSim is single-threaded (during a state
 * transition), so they do not need to be re-entrant.
 */

#ifndef __USER_STATE_H
#define __USER_STATE_H

#include <stdint.h>
#include <vector>

#ifdef __cplusplus
extern "C"
{
#endif

  /* Optional hooks exported by a UserState plugin. */
  int user_state_size(int index);
  void user_state_save(int index, void *buf);
  void user_state_load(int index, const void *buf, int size);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

namespace detsim
{
namespace user_state
{

/* Install the resolved hooks (nullptr for hooks the plugin does not export). */
void install(int (*size_fn)(int), void (*save_fn)(int, void *),
             void (*load_fn)(int, const void *, int));

/* True when at least the size+save hooks are available. */
bool enabled();

/* Capture the custom state of tracee `index` into `out` (cleared on no-op). */
void capture(int index, std::vector<uint8_t> &out);

/* Restore the custom state of tracee `index` from `blob`. */
void restore(int index, const std::vector<uint8_t> &blob);

} // namespace user_state
} // namespace detsim

#endif /* __cplusplus */

#endif /* __USER_STATE_H */
