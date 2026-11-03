/*
 * transition_profile.h - opt-in phase accounting for one state transition.
 *
 * The exploration loop runs one transition at a time, so a transition's phase
 * breakdown is the per-state cost breakdown. Phases are timed with
 * steady_clock in state_transition.cpp.
 *
 * The instrumentation is compiled in only when the build defines
 * DETSIM_ENABLE_TRANSITION_PROFILE=1 (`make NP=N PROFILER=1`, or add the
 * define to CXXFLAGS). The default build contains no clocks, no counters and
 * no reporting code: every entry point below collapses to an empty inline
 * function, so a release binary is byte-for-byte what it was without this
 * header. See perf/raft/ for the benchmark that uses it.
 *
 * Even in a profiled build nothing is measured unless the process also sets
 * DETSIM_TRANSITION_PROFILE=1, so one instrumented binary can be used for both
 * timing and normal exploration runs.
 *
 * At the end of every search the accumulated totals are printed on stderr:
 *   TRANSITION_PROFILE {"transitions":N,"restore_us":…,"syscall_us":…, …}
 *
 * `capture` has no clock of its own: the pre-existing TransitionResult.save_time
 * (capture + checker + identity commit, reported as SEARCH_WORK) already
 * measures it, so capture = save_us - checkpoint_us - check_us.
 */
#ifndef DETSIM_TRANSITION_PROFILE_H
#define DETSIM_TRANSITION_PROFILE_H

#ifndef DETSIM_ENABLE_TRANSITION_PROFILE
#define DETSIM_ENABLE_TRANSITION_PROFILE 0
#endif

#if DETSIM_ENABLE_TRANSITION_PROFILE

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace detsim
{
namespace profile
{

enum Phase
{
  kRestore,   /* recover_running_state(): tracee images back to the source */
  kCheck,     /* one check() evaluation of the user plugin */
  kSyscall,   /* execution of one system call on a tracee */
  kCheckpoint /* identity hash + metadata commit */
};
constexpr int kPhaseCount = 4;

struct Clock
{
  using point = std::chrono::steady_clock::time_point;
  static point now() { return std::chrono::steady_clock::now(); }
  static double us(point from, point to)
  {
    return std::chrono::duration<double, std::micro>(to - from).count();
  }
};

inline bool enabled()
{
  static const bool on = [] {
    const char *value = std::getenv("DETSIM_TRANSITION_PROFILE");
    return value && *value && std::strcmp(value, "0") != 0;
  }();
  return on;
}

struct Totals
{
  double phases[kPhaseCount] = {};
  unsigned long long transitions = 0;
};

inline Totals &totals()
{
  static thread_local Totals value;
  return value;
}

inline void add(int phase, Clock::point from, Clock::point to)
{
  if (enabled())
    totals().phases[phase] += Clock::us(from, to);
}

/* RAII phase timer. When the process has profiling off the clock read still
 * costs a vDSO call, which is why this whole header is opt-in at build time. */
class Scope
{
  public:
  explicit Scope(int phase) : phase_(phase), start_()
  {
    if (enabled())
    {
      totals(); /* touch the thread-local once */
      start_ = Clock::now();
    }
  }
  ~Scope()
  {
    if (enabled())
      add(phase_, start_, Clock::now());
  }
  Scope(const Scope &) = delete;
  Scope &operator=(const Scope &) = delete;

  private:
  int phase_;
  Clock::point start_;
};

inline void report(const char *tag)
{
  if (!enabled())
    return;
  const Totals &t = totals();
  std::fprintf(stderr,
               "%s {\"transitions\":%llu,\"restore_us\":%.0f,\"syscall_us\":%.0f,"
               "\"checkpoint_us\":%.0f,\"check_us\":%.0f}\n",
               tag, t.transitions, t.phases[kRestore], t.phases[kSyscall],
               t.phases[kCheckpoint], t.phases[kCheck]);
}

inline void reset() { totals() = Totals{}; }

} // namespace profile
} // namespace detsim

#else /* !DETSIM_ENABLE_TRANSITION_PROFILE: the whole profiler compiles away */

namespace detsim
{
namespace profile
{

enum Phase
{
  kRestore,
  kCheck,
  kSyscall,
  kCheckpoint
};

inline bool enabled() { return false; }

inline void add(int, const void *, const void *) {}
inline void report(const char *) {}
inline void reset() {}

class Scope
{
  public:
  explicit Scope(int) {}
};

} // namespace profile
} // namespace detsim

#endif /* DETSIM_ENABLE_TRANSITION_PROFILE */

#endif
