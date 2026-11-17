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
 *   TRANSITION_PROFILE_DETAIL {"capture_total_us":…,"capture_layout_us":…, …}
 *
 * `capture` has no clock of its own: the pre-existing TransitionResult.save_time
 * (capture + checker + identity commit, reported as SEARCH_WORK) already
 * measures it, so capture = save_us - checkpoint_us - check_us. The DETAIL line
 * splits that window into the segments named by `Detail` below; it is the
 * instrument that decides where throughput work is worth doing, because the
 * broad phases leave most of save_time unattributed.
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

/* Segments of the save window. The broad phases above attribute only the
 * plugin check and the identity commit, so every other microsecond of
 * save_time -- capture, UserState capture, the search tree insert and the
 * running-state handoff -- needs its own clock. */
enum Detail
{
  kCaptureTotal,    /* sys_state(...) construction: the whole capture */
  kCaptureLayout,   /* /proc/pid/maps parse + capture layout */
  kCaptureRegs,     /* PTRACE_GETREGS + capture_xstate per thread */
  kCaptureSerialize,/* tracee_state cereal encoding */
  kCaptureSnapshot, /* page/flat snapshot build + StateStore save */
  kCheckAssertions, /* check_assertions(): assertions + plugin check */
  kUserStateCapture,/* capture_user_state() blob re-read */
  kCommitMetadata,  /* identity hash + metadata persist */
  kTreeAdd,         /* state_tree_add() */
  kRunningCopy,     /* ptmc_state.running_state = result.new_state */
  kIdentityEncode,  /* cereal identity encoding + XXHash64 (inside commit) */
  kMetadataPersist, /* metadata store exists/load/compare/save (inside commit) */
  kStateLoad,       /* sys_state(hash): reopen a saved state for expansion */
  kEnableActions,   /* enabled_actor_actions() + classify */
  kPrepareCapture,  /* tracee_state ctor + prepare_capture (inside capture) */
  kValidateIo,      /* validate_io_state() graph check (inside capture) */
  kTransitionCall,  /* the whole state_transition() call, caller-side */
  kRefreshPending,  /* refresh_pending_syscalls(): readiness propagation */
  kPrepareBarrier,  /* tracee_state::prepare_capture(): stop barrier + checks */
  kRestoreCopy,     /* recover_running_state(): state copy */
  kRestoreBind,     /* recover_running_state(): thread binding resolution */
  kRestoreMasks,    /* recover_running_state(): signal masks + pool rollback */
  kRestoreImage,    /* recover_running_state(): memory/register restore */
  kRestoreResources,/* recover_running_state(): kernel-object restore */
  kBarrierVerify,   /* prepare_thread_pool(): owned-set + pending verification */
  kBarrierBoundary, /* prepare_thread_pool(): per-carrier boundary + mask probe */
  kBarrierSlots     /* prepare_thread_pool(): bound-slot validation */
};
constexpr int kDetailCount = kBarrierSlots + 1;

inline const char *detail_name(int detail)
{
  static const char *const names[kDetailCount] = {
      "capture_total_us", "capture_layout_us", "capture_regs_us",
      "capture_serialize_us", "capture_snapshot_us", "check_assertions_us",
      "user_state_capture_us", "commit_metadata_us", "tree_add_us",
      "running_copy_us", "identity_encode_us", "metadata_persist_us",
      "state_load_us", "enable_actions_us", "prepare_capture_us",
      "validate_io_us", "transition_call_us", "refresh_pending_us",
      "prepare_barrier_us", "restore_copy_us", "restore_bind_us",
      "restore_masks_us", "restore_image_us", "restore_resources_us",
      "barrier_verify_us", "barrier_boundary_us", "barrier_slots_us"};
  return detail >= 0 && detail < kDetailCount ? names[detail] : "unknown";
}

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
  double details[kDetailCount] = {};
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

/* Same RAII shape for one segment of the save window. Kept separate from Scope
 * so a detail clock can never be mistaken for a top-level phase. */
class DetailScope
{
  public:
  explicit DetailScope(int detail) : detail_(detail), start_()
  {
    if (enabled())
    {
      totals();
      start_ = Clock::now();
    }
  }
  ~DetailScope()
  {
    if (enabled())
      totals().details[detail_] += Clock::us(start_, Clock::now());
  }
  DetailScope(const DetailScope &) = delete;
  DetailScope &operator=(const DetailScope &) = delete;

  private:
  int detail_;
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
  std::fprintf(stderr, "%s_DETAIL {\"transitions\":%llu", tag, t.transitions);
  for (int detail = 0; detail < kDetailCount; ++detail)
    std::fprintf(stderr, ",\"%s\":%.0f", detail_name(detail), t.details[detail]);
  std::fprintf(stderr, "}\n");
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

/* The detail segments are named in instrumented builds only, but the
 * instrumentation sites still mention them, so the identifiers must exist. */
enum Detail
{
  kCaptureTotal,
  kCaptureLayout,
  kCaptureRegs,
  kCaptureSerialize,
  kCaptureSnapshot,
  kCheckAssertions,
  kUserStateCapture,
  kCommitMetadata,
  kTreeAdd,
  kRunningCopy,
  kIdentityEncode,
  kMetadataPersist,
  kStateLoad,
  kEnableActions,
  kPrepareCapture,
  kValidateIo,
  kTransitionCall,
  kRefreshPending,
  kPrepareBarrier,
  kRestoreCopy,
  kRestoreBind,
  kRestoreMasks,
  kRestoreImage,
  kRestoreResources,
  kBarrierVerify,
  kBarrierBoundary,
  kBarrierSlots
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

class DetailScope
{
  public:
  explicit DetailScope(int) {}
};

} // namespace profile
} // namespace detsim

#endif /* DETSIM_ENABLE_TRANSITION_PROFILE */

#endif
