#include "emu.h"
#include "guest.h"
#include "monitor.h"
#include "state/state.h"
#include "user_plugin.h"
#include <cerrno>
#include <sys/syscall.h>

int choose_many[450] = {};
choose_func choose_syswhat[450] = {};

/* Syscalls whose outcome detsim can materialize today. Candidate-class calls
 * (recvfrom) and the special gettimeofday path are listed separately from the
 * value-class models and read(2), which is modelled by handle_read(). */
bool emu_syscall_choice_supported(int nr)
{
  switch (nr)
  {
    case SYS_gettimeofday:
    case SYS_recvfrom:
    case SYS_read:
    case SYS_clock_gettime:
    case SYS_clock_getres:
    case SYS_getrandom:
    case SYS_sysinfo:
    case SYS_sched_getaffinity:
      return true;
    default:
      return false;
  }
}

/* Value-class syscalls whose default result is produced by their own model
 * (emu_process_syscall(), or handle_read() for read(2)), so a ChooseFunc branch
 * may replace it whole. analyze_choose() uses this to demand a choose_ handler
 * for a choice point. */
bool emu_syscall_uses_generic_choice(int nr)
{
  switch (nr)
  {
    case SYS_read:
    case SYS_clock_gettime:
    case SYS_clock_getres:
    case SYS_getrandom:
    case SYS_sysinfo:
    case SYS_sched_getaffinity:
      return true;
    default:
      return false;
  }
}

long emu_apply_choose_out(int pid, const syscall_info &info, const choose_out &out)
{
  for (int i = 0; i < 6; ++i)
  {
    if (!out.len[i])
      continue;
    if (!out.args[i])
      return -EFAULT;
    if (!guest_write_exact(pid, reinterpret_cast<void *>(info.args[i]),
                           out.args[i], static_cast<size_t>(out.len[i])))
      return -EFAULT;
  }
  return out.rval;
}

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
    uint64_t args[6] = {
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(tv)),
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(tz)), 0, 0, 0, 0};
    choose_in in(SYS_gettimeofday, ptmc_state.pids[node], node, args, current);
    choose_out *out = choose_syswhat[SYS_gettimeofday](ptmc_state.pids[node],
                                                       ptmc_state.choose, in);
    if (out)
    {
      timeval result = current;
      if (out->args[0] && out->len[0] >= static_cast<int>(sizeof(result)))
        result = *static_cast<const timeval *>(out->args[0]);
      else if (ptmc_state.choose > 0)
        result = advance_clock(current, kDefaultTimeStepUs * ptmc_state.choose);
      const int rval = out->rval;
      delete out;
      return emu_gettimeofday_write(node, result, tv, tz) ?: rval;
    }
    /* A null outcome keeps the default branch below. */
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
