/*
 * user_plugin.h - User-facing check/choose plugin interface (target contract)
 *
 * The tracer accepts three optional plugin configuration keys:
 *
 *   "UserCheck":  "plugins/check.c"     - a C/C++ source or a prebuilt .so
 *   "UserState":  "plugins/check.c"     - same file, optional state hooks
 *   "ChooseFunc": "plugins/choose.c"    - a C/C++ source or a prebuilt .so
 *
 * Source files are compiled on demand with g++ into "<stem>.so" next to the
 * source (the historical behaviour of this tracer), then dlopen()'d.
 *
 * Hook contract
 * -------------
 *   int check(void)
 *       Called after every state transition (and once for the initial state).
 *       A non-zero result reports a property violation of the resulting state.
 *
 *   int user_state_size(int index)
 *   void user_state_save(int index, void *buf)
 *   void user_state_load(int index, const void *buf, int size)
 *       Optional shadow state of tracee `index`; captured into every checkpoint
 *       and restored on load. A missing hook is a no-op.
 *
 *   choose_func choose_<syscall>(struct syscall_info *info)
 *       Resolved for every syscall mnemonic. It decides how many outcomes are
 *       enumerated for that syscall (0 = no choice point).
 */

#ifndef __USER_PLUGIN_H
#define __USER_PLUGIN_H

#include <sys/types.h>
#include <string>
#include <vector>
#include "emu.h"

struct syscall_info;
struct sys_state;

#ifdef __cplusplus
extern "C"
{
#endif

/* Optional UserState hooks exported by the check plugin. */
int user_state_size(int index);
void user_state_save(int index, void *buf);
void user_state_load(int index, const void *buf, int size);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

namespace detsim
{
namespace user_plugin
{

/* Mirrors of the resolved hooks, exposed so the header's inline queries below
 * compile in every translation unit without a function call. */
extern int (*g_check_fn)(void);
extern int (*g_state_size_fn)(int);
extern void (*g_state_save_fn)(int, void *);
extern void (*g_state_load_fn)(int, const void *, int);

/* Load the plugins named by the configuration. Replaces any previous state. */
void configure(const char *check_source, const char *state_source,
               const char *choose_source, const char *configuration_directory);

/* Drop every loaded plugin and resolved hook. */
void reset();

/* True when a check plugin is loaded. */
bool has_check();

/* Run check(); returns the violation flag (false when no plugin is loaded). */
bool check_state(const sys_state &state, int process);

/* Description of loaded modules for the CLI. */
std::string describe();

/* Inline hot-path queries. These return the same values as has_check() and
 * user_state_enabled() but read a global instead of calling into this
 * translation unit: the capture path asks once per node and the restore path
 * once more per node, so the call itself was measurable at 200+ states/s. */
inline bool has_check_inline() { return g_check_fn != nullptr; }
inline bool user_state_enabled_inline()
{
  return g_state_size_fn != nullptr && g_state_save_fn != nullptr;
}

/* True when the UserState plugin supplies capture hooks. */
bool user_state_enabled();

/* Capture the shadow state of tracee `index` (empty when unavailable). */
void user_state_capture(int index, std::vector<unsigned char> &out);

/* Restore the shadow state of tracee `index` from `blob`. */
void user_state_restore(int index, const std::vector<unsigned char> &blob);

} // namespace user_plugin
} // namespace detsim

#endif /* __cplusplus */

#endif /* __USER_PLUGIN_H */
