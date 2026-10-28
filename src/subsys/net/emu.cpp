#include "emu.h"
#include "guest.h"
#include "monitor.h"
#include "state/state.h"
#include "user_plugin.h"
#include <cerrno>
#include <stdexcept>
#include <sys/syscall.h>

int choose_many[450] = {};
choose_func choose_syswhat[450] = {};

/* Default time advance per enumerated outcome when a ChoosePoint enables a
 * gettimeofday choice but no ChooseFunc plugin supplies values. */
static constexpr int64_t kDefaultTimeStepUs = 100;

/* Copy a candidate clock value to the guest; a rejected guest pointer must not
 * consume the virtual clock. */
static int emu_gettimeofday_write(int node, timeval result, struct timeval *tv,
                                  struct timezone *tz)
{
  if (tv && !guest_write_exact(ptmc_state.pids[node], tv, &result, sizeof(result)))
    return -EFAULT;
  const struct timezone zero{};
  if (tz && !guest_write_exact(ptmc_state.pids[node], tz, &zero, sizeof(zero)))
    return -EFAULT;
  ptmc_state.time[node] = result;
  return 0;
}

/* Advance the virtual clock by `microseconds`. */
static timeval advance_clock(timeval current, int64_t microseconds)
{
  int64_t sec = current.tv_sec + microseconds / 1000000;
  int64_t usec = current.tv_usec + microseconds % 1000000;
  if (usec >= 1000000) { sec += 1; usec -= 1000000; }
  return {static_cast<time_t>(sec), static_cast<suseconds_t>(usec)};
}

int emu_gettimeofday(struct timeval *tv, struct timezone *tz)
{
  const int node = ptmc_state.cursor;
  const timeval current = ptmc_state.time[node];

  /* A configured ChooseFunc plugin owns the value of each enumerated outcome. */
  if (ptmc_state.n_choose != 0 && choose_syswhat[SYS_gettimeofday])
  {
    choose_in in(current);
    choose_out *out = choose_syswhat[SYS_gettimeofday](ptmc_state.pids[node],
                                                       ptmc_state.choose, in);
    if (!out)
      throw std::runtime_error("choose_gettimeofday returned no outcome");
    timeval result = current;
    if (out->args[0] && out->len[0] >= static_cast<int>(sizeof(result)))
      result = *static_cast<const timeval *>(out->args[0]);
    else if (ptmc_state.choose > 0)
      result = advance_clock(current, kDefaultTimeStepUs * ptmc_state.choose);
    const int rval = out->rval;
    delete out;
    return emu_gettimeofday_write(node, result, tv, tz) ?: rval;
  }

  /* No plugin: each enumerated outcome advances by one default step so the
   * branches observe different instants. */
  if (ptmc_state.n_choose != 0)
  {
    const int choice = ptmc_state.choose < 0 ? 0 : ptmc_state.choose;
    return emu_gettimeofday_write(node, advance_clock(current,
        kDefaultTimeStepUs * (choice + 1)), tv, tz);
  }

  return emu_gettimeofday_write(node, current, tv, tz);
}
