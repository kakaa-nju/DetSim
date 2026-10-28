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
