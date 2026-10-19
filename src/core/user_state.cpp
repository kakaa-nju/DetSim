/*
 * user_state.cpp - User-defined state plugin runtime
 *
 * Holds the hook pointers resolved from a UserState plugin and provides the
 * capture/restore entry points used by state save/load.
 *
 * See user_state.h for the plugin contract.
 */

#include "user_state.h"
#include "log_wrapper.h"

namespace detsim
{
namespace user_state
{

static int (*g_size_fn)(int) = nullptr;
static void (*g_save_fn)(int, void *) = nullptr;
static void (*g_load_fn)(int, const void *, int) = nullptr;

void install(int (*size_fn)(int), void (*save_fn)(int, void *),
             void (*load_fn)(int, const void *, int))
{
  g_size_fn = size_fn;
  g_save_fn = save_fn;
  g_load_fn = load_fn;

  LOG_INFO("User state hooks: size=%s save=%s load=%s", size_fn ? "yes" : "no",
           save_fn ? "yes" : "no", load_fn ? "yes" : "no");
}

bool enabled() { return g_size_fn != nullptr && g_save_fn != nullptr; }

void capture(int index, std::vector<uint8_t> &out)
{
  out.clear();

  if (!enabled())
    return;

  int size = g_size_fn(index);
  if (size <= 0)
    return;

  out.resize(static_cast<size_t>(size));
  g_save_fn(index, out.data());

  LOG_DEBUG("Captured %d bytes of user state for tracee %d", size, index);
}

void restore(int index, const std::vector<uint8_t> &blob)
{
  if (!g_load_fn)
    return;

  /* An empty blob means the plugin had no state at save time; leave the
   * plugin's current (default) state untouched, which mirrors the behaviour
   * of a freshly started run. */
  if (blob.empty())
    return;

  g_load_fn(index, blob.data(), static_cast<int>(blob.size()));
  LOG_DEBUG("Restored %zu bytes of user state for tracee %d", blob.size(),
            index);
}

} // namespace user_state
} // namespace detsim
