#include "scheduler.h"
#include "common.h"
#include "debug.h"
#include "emu.h"
#include "fsstate.h"
#include "guest.h"
#include "monitor.h"
#include "sockstate.h"
#include "state.h"
#include "state_store.h"
#include "sysstate_store.h"
#include "utils.h"
#include "expr_eval.hpp"

/* NCursesUI integration */
#include "ui/ncurses_ui.h"
#include "ui/log_wrapper.h"

// 外部声明：get_ncurses_ui 定义在 main.cpp
extern "C" detsim::ui::NCursesUI* get_ncurses_ui();

#include <assert.h>
#include <cstddef>
#include <cstdlib>
#include <cmath>        // for log, exp
#include <random>
#include <fcntl.h>
#include <readline/readline.h>
#include <signal.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <sys/ioctl.h>

#define pids ptmc_state.pids
#define PTRACE_TRAP_SIG (SIGTRAP | 0x80)
extern FILE *log_fp;

/* Apply user choice modifications to syscall arguments */
void apply_choose(const syscall_info &info, choose_out *out)
{
  for (int i = 0; i < 6; i++)
  {
    if (out->len[i])
      memcpy_host2guest((void *)info.args[i], out->args[i], out->len[i]);
  }
}

/* prepare tracee for ptrace, and execve */
static void start_tracee(int id)
{
  signal(SIGINT, SIG_IGN);
  ptrace_right(PTRACE_TRACEME, 0, NULL, NULL);
  // detsim::ui::ui_printf("Will start executable %s\n", ptmc_state.tracee[id].executable);
  if (log_fp != stdout)
    fclose(log_fp);
  raise(SIGSTOP);

  execv(ptmc_state.tracee[id].executable, ptmc_state.tracee[id].argv);

  perror("exec");
}

// static pid_t pgid = 114514;

static void exit_all(int _)
{
  /* Shutdown StateStore to flush pending writes */
  StateStore::instance().shutdown();
  kill(0, SIGTERM);
  exit(EXIT_SUCCESS);
}

static void exit_all(void)
{
  /* Shutdown StateStore to flush pending writes */
  StateStore::instance().shutdown();
  kill(0, SIGTERM);
  exit(EXIT_SUCCESS);
}

extern std::unordered_map<int, void *> rseq_struct;
extern std::unordered_map<int, int> rseq_len;

/* Extract one entire syscall, preserve its original effect */
syscall_info extract_one_syscall(pid_t pid)
{
  /* ptrace(2): When delivering system call traps, set bit 7 in the signal     *
   * number (i.e., deliver SIGTRAP|0x80). This makes it easy for the tracer to *
   * distinguish normal traps from those caused by a system call. */
  int wstatus = 0;
  struct ptrace_syscall_info info;
  syscall_info ret = {};  /* Zero-initialize all fields */

  /* entry */
  ptrace_right(PTRACE_SYSCALL, pid, NULL, NULL);
  waitpid(pid, &wstatus, 0);
  assert(WIFSTOPPED(wstatus) && WSTOPSIG(wstatus) == PTRACE_TRAP_SIG);

  ptrace_right(PTRACE_GET_SYSCALL_INFO, pid, (void *)sizeof(info), &info);
  assert(info.op == PTRACE_SYSCALL_INFO_ENTRY);
  ret.nr = info.entry.nr;
  for (int i = 0; i < 6; i++)
    ret.args[i] = info.entry.args[i];

  /* exit */
  ptrace_right(PTRACE_SYSCALL, pid, NULL, NULL);
  waitpid(pid, &wstatus, 0);
  assert(WIFSTOPPED(wstatus) && WSTOPSIG(wstatus) == PTRACE_TRAP_SIG);

  ptrace_right(PTRACE_GET_SYSCALL_INFO, pid, (void *)sizeof(info), &info);
  assert(info.op == PTRACE_SYSCALL_INFO_EXIT);
  ret.rval = info.exit.rval;

  /* eliminate indeterministic syscall result */
  /* set_tid_address */
  if (ret.nr == SYS_set_tid_address)
  {
    ret.rval = 114514;
    tracee_set_rax(pid, 114514);
  }
  /* getrandom */
  if (ret.nr == SYS_getrandom)
  {
    memcpy_host2guest((void *)ret.args[0], "\0\0\0\0\0\0\0\0", 8);
  }
  /* rseq mask */
  if (ret.nr == SYS_rseq)
  {
    rseq_struct[pid] = (void *)ret.args[0];
    rseq_len[pid] = ret.args[1];
    LOG_TRACE("Record rseq(%p, %d)", (void *)ret.args[0], ret.args[1]);
  }
  return ret;
}

static void do_nosys(int pid)
{
  tracee_set_orig_rax(pid, -1);
  tracee_set_rax(pid, -1);
}

/* Check if syscall need to be done */
static void on_syscall_enter(pid_t pid, int nr)
{
  switch (nr)
  {
    /* emulated. so do nothing */
    case SYS_gettimeofday:
    case SYS_sendto:
    case SYS_recvfrom:
    case SYS_listen:
    case SYS_bind:
    case SYS_nanosleep:
    case SYS_clock_nanosleep:
    case SYS_socket:
    case SYS_connect:
    /* VFS emulated */
    case SYS_open:
    case SYS_openat:
    case SYS_chdir:
    case SYS_read:
    case SYS_write:
    case SYS_close:
    case SYS_lseek:
    case SYS_stat:
    case SYS_fstat:
      do_nosys(pid);
      break;

    /* Do not allow process quit. While still allow thread quit(TODO). */
    case SYS_exit_group:
    case SYS_exit:
      /* After yield, rip += 2; mark tracee as EXITED and never get scheduled */
      tracee_switch_syscall(pid, SYS_sched_yield, 0, 0, 0, 0, 0, 0);
      break;

    /* act normally. SYS_sched_yield is preserved for state check */
    case SYS_sched_yield:
    default:
      break;
  }
}

#define CKPT_NO 0
#define CKPT_YES 1
#define CKPT_DISCARD 2
#define CKPT_EXIT 3

int analyze_choose(struct syscall_info *info) { return choose_many[info->nr]; }

/* parse syscall_info */
static int on_syscall_exit(pid_t pid, struct syscall_info *info)
{
  /* calculate choose_n from context */
  ptmc_state.n_choose = analyze_choose(info);

  if (!is_auto_mode() && ptmc_state.n_choose != 0) /* ask for choose */
  {
    detsim::ui::NCursesUI* ui = get_ncurses_ui();
    if (ui) {
      /* Use NCursesUI prompt */
      char prompt_buf[256];
      snprintf(prompt_buf, sizeof(prompt_buf), 
               "Choose from %d options", ptmc_state.n_choose);
      ptmc_state.choose = ui->prompt_int(prompt_buf, 0, 0, ptmc_state.n_choose - 1);
    } else {
      /* Fallback to readline */
      static char *line_read;
      UI_LOG_INFO("Here comes with %d choices.", ptmc_state.n_choose);
      line_read = readline("You choose? [0, N): ");
      while (sscanf(line_read, "%d", &ptmc_state.choose) != 1 ||
             ptmc_state.choose < 0 || ptmc_state.choose >= ptmc_state.n_choose)
      {
        free(line_read);
        line_read = readline("Please make legal choice: ");
      }
      free(line_read);
    }
  } else if (is_auto_mode() && ptmc_state.n_choose != 0 && ptmc_state.mode == PTMC_STATE::MODE_RAND) {
    ptmc_state.choose = rand() % ptmc_state.n_choose; 
  }

  /* Check and dump for each generated next state */
  long ret;
  switch (info->nr)
  {
      /* syscalls need scheduled */
    case SYS_recvfrom:
      ret = emu_recvfrom(info->args[0], (void *)info->args[1], info->args[2],
                         info->args[3], (struct sockaddr *)info->args[4],
                         (socklen_t *)info->args[5]);
      tracee_set_rax(pid, ret);
      info->rval = ret;
      if (ret < 0) /* for willemt/raft, a `poll_msg` cycle will recv() until
                       nothing can be received */
      {
        return CKPT_NO;
      }
      return CKPT_YES;
      break;
    case SYS_sched_yield:
      break;
    case SYS_sendto:
      ret = emu_sendto(info->args[0], (void *)info->args[1], info->args[2],
                       info->args[3], (struct sockaddr *)info->args[4],
                       info->args[5]);
      tracee_set_rax(pid, ret);
      info->rval = ret;
      return CKPT_NO;
    case SYS_gettimeofday:
      ret = emu_gettimeofday((struct timeval *)info->args[0],
                             (struct timezone *)info->args[1]);
      tracee_set_rax(pid, ret);
      info->rval = ret;
      return CKPT_YES;
    case SYS_socket:
      ret = emu_socket(info->args[0], info->args[1], info->args[2]);
      tracee_set_rax(pid, ret);
      info->rval = ret;
      return CKPT_NO;
    case SYS_listen:
      ret = emu_listen(info->args[0], info->args[1]);
      tracee_set_rax(pid, ret);
      info->rval = ret;
      return CKPT_NO;
    case SYS_bind:
      ret = emu_bind(info->args[0], (const struct sockaddr *)info->args[1],
                     info->args[2]);
      tracee_set_rax(pid, ret);
      info->rval = ret;
      return CKPT_YES;
    case SYS_connect:
      ret = emu_connect(info->args[0], (const struct sockaddr *)info->args[1],
                        info->args[2]);
      tracee_set_rax(pid, ret);
      info->rval = ret;
      return CKPT_NO;

    case SYS_chdir: {
      // Read the path string from the tracee memory and update cwd on success
      uintptr_t guest_ptr = info->args[0];
      // helper: read null-terminated string from guest using memcpy_guest2host
      std::string path;
      char ch;
      size_t idx = 0;
      do {
        memcpy_guest2host(&ch, (const void *)(guest_ptr + idx), 1);
        if (ch != '\0') path.push_back(ch);
        idx++;
      } while (ch != '\0');

      if ((long)info->rval >= 0) {
        // success: update cwd of current fs_state
        ptmc_state.fs_states[ptmc_state.cursor].set_cwd(path);
      }
      // reflect syscall return
      tracee_set_rax(pid, info->rval);
      return CKPT_YES;
    }

    case SYS_exit_group:
    case SYS_exit:
      return CKPT_EXIT;

    /* VFS Handlers */
    case SYS_open:
        ret = emu_vfs_openat(AT_FDCWD, (const char *)info->args[0], info->args[1], info->args[2]);
        tracee_set_rax(pid, ret);
        info->rval = ret;
        return CKPT_NO;
    case SYS_openat:
      ret = emu_vfs_openat(info->args[0], (const char *)info->args[1], info->args[2], info->args[3]);
      tracee_set_rax(pid, ret);
      info->rval = ret;
      return CKPT_NO;
    case SYS_read:
      ret = emu_vfs_read(info->args[0], (void *)info->args[1], info->args[2]);
      tracee_set_rax(pid, ret);
      info->rval = ret;
      return CKPT_NO;
    case SYS_write:
      ret = emu_vfs_write(info->args[0], (const void *)info->args[1], info->args[2]);
      tracee_set_rax(pid, ret);
      info->rval = ret;
      return CKPT_YES;
    case SYS_close:
        ret = emu_vfs_close(info->args[0]);
        tracee_set_rax(pid, ret);
        info->rval = ret;
        return CKPT_NO;
    case SYS_lseek:
        ret = emu_vfs_lseek(info->args[0], info->args[1], info->args[2]);
        tracee_set_rax(pid, ret);
        info->rval = ret;
        return CKPT_NO;
    case SYS_stat:
        ret = emu_vfs_stat((const char *)info->args[0], (struct stat *)info->args[1]);
        tracee_set_rax(pid, ret);
        info->rval = ret;
        return CKPT_NO;
    case SYS_fstat:
        ret = emu_vfs_fstat(info->args[0], (struct stat *)info->args[1]);
        tracee_set_rax(pid, ret);
        info->rval = ret;
        return CKPT_NO;

    case SYS_nanosleep:
    case SYS_brk:
      return CKPT_NO;

    default:
      return CKPT_NO;
  }
  return CKPT_YES;
}

void show_syscall_history();
/* Do one syscall, with its effects altered */
int do_one_syscall(pid_t pid, syscall_info *si)
{
  /* ptrace(2): When delivering system call traps, set bit 7 in the signal     *
   * number (i.e., deliver SIGTRAP|0x80). This makes it easy for the tracer to *
   * distinguish normal traps from those caused by a system call. */
  int wstatus = 0;
  struct ptrace_syscall_info info;
  LOG_TRACE("Do one syscall");
  // tracee_show_regs(pid);

  /* entry */
  ptrace_right(PTRACE_SYSCALL, pid, NULL, NULL);

  waitpid(pid, &wstatus, 0);
  if (WIFSTOPPED(wstatus) && (WSTOPSIG(wstatus) != PTRACE_TRAP_SIG))
  {
    LOG_INFO(
        "ptrace_syscall has wrong stop status. WIFSTOPPED=%s and WSTOPSIG=%s.",
        WIFSTOPPED(wstatus) ? "true" : "false", strsignal(WSTOPSIG(wstatus)));
    show_syscall_history();
    ptrace_right(PTRACE_SYSCALL, pid, NULL, NULL);
    waitpid(pid, &wstatus, 0);
    assert(WIFSTOPPED(wstatus) && (WSTOPSIG(wstatus) == PTRACE_TRAP_SIG));
  }

  ptrace_right(PTRACE_GET_SYSCALL_INFO, pid, (void *)sizeof(info), &info);
  assert(info.op == PTRACE_SYSCALL_INFO_ENTRY);

  on_syscall_enter(pid, info.entry.nr);
  si->nr = info.entry.nr;
  for (int i = 0; i < 6; i++)
    si->args[i] = info.entry.args[i];

  /* exit */
  ptrace_right(PTRACE_SYSCALL, pid, NULL, NULL);
  waitpid(pid, &wstatus, 0);
  assert(WIFSTOPPED(wstatus) && (WSTOPSIG(wstatus) == PTRACE_TRAP_SIG));

  ptrace_right(PTRACE_GET_SYSCALL_INFO, pid, (void *)sizeof(info), &info);
  assert(info.op == PTRACE_SYSCALL_INFO_EXIT);
  si->rval = info.exit.rval;
  return on_syscall_exit(pid, si);
}

static void init_tracee_state(int index)
{
  int pid = pids[index];
  int wstatus = 0;
  int stop_nr = is_dynamically_linked(ptmc_state.tracee[index].executable)
                    ? SYS_munmap
                    : SYS_mprotect;
  Assert(waitpid(pid, &wstatus, 0) == pid, "%s", strerror(errno));
  assert(WIFSTOPPED(wstatus) && WSTOPSIG(wstatus) == SIGSTOP);
  /* tracee: stop at raise(SIGSTOP) */

  /* ptrace_right(PTRACE_SEIZE, pid, 0, 0); */
  ptrace_right(PTRACE_SETOPTIONS, pid, 0, PTRACE_O_TRACESYSGOOD);

  /* skip execve */
  while (extract_one_syscall(pid).nr != SYS_execve)
    ;

  /* execve(2): after a successful execve, a SIGTRAP *
   * (instead of SIGTRAP | 0x80) will be sent to it  */
  ptrace_right(PTRACE_SYSCALL, pid, 0, 0);
  waitpid(pid, &wstatus, 0);
  assert(WIFSTOPPED(wstatus) && WSTOPSIG(wstatus) == SIGTRAP);

  remove_vdso(pid);

  /* skip non-user code: *
   * Libc initialization ends with an `munmap` call */
  while (extract_one_syscall(pid).nr != stop_nr)
    ;

  /* Preserve memory for parameters. Need recover registers *
   * for the original first syscall. */
  tracee_reserve_temp_page(pid);
}

/* loaded, exec 1 STEP, not stored. Save Last syscall into `info` *
 * 1 STEP = critical syscall step */
int exec_once(syscall_info *info)
{
  sys_state &s = ptmc_state.source_state;
  int index = ptmc_state.cursor;
  pid_t pid = pids[index];

  if (s.exited[index] == 1)
    return -1;
  LOG_DEBUG("exec_once from state " HASH_FORMAT ":" HASH_FORMAT, s.ss_hash,
            s.child[index].ts_hash);

  while (true)
  {
    int result = do_one_syscall(pid, info);

    switch (result)
    {
      case CKPT_NO:
        continue;
      case CKPT_DISCARD:
      case CKPT_YES:
        return result;
      case CKPT_EXIT:
        ptmc_state.exited[index] = 1;
        LOG_DEBUG("Tracee %d set exited", index);
        return CKPT_YES;
    }
  }
  return 0;
}

long expr(const char *e, bool *success);
/* Judge state assertions. Return 0 if legal */
static int check_state()
{
  for (auto &assertion : ptmc_state.assertions)
  {
    bool success;
    long val = expr(assertion.c_str(), &success);
    LOG_DEBUG("Assertion \"%s\" = %ld (success=%d)", 
              assertion.c_str(), val, success);
    if (!success)
      LOG_ERROR("Failed to evaluate expression \"%s\"", assertion.c_str());
    if (val == false)
    {
      detsim::ui::ui_printf("Assertion \"%s\" fail at sys_state " HASH_FORMAT "!\n",
             assertion.c_str(), ptmc_state.dest_state.ss_hash);
      return 1;
    }
  }
  for (auto f : ptmc_state.user_checks)
    if (f())
      return 1;
  return 0;
}

/* For single step use. Should get hash from ptmc_state.sysstate_hash. *
 * So, after every run, include cmd_cont, should set *
 * ptmc_state.sysstate_hash as the ending state. */

void load(hash_type hash)
{
  /* load only if state hash is set */
  assert(ptmc_state.state == PTMC_PRELOAD);

  ptmc_state.state = PTMC_LOADED;
  
  /* Use provided hash if non-zero, otherwise fall back to global */
  if (hash != 0) {
    ptmc_state.sysstate_hash = hash;
  }
  
  ptmc_state.source_state = sys_state(ptmc_state.sysstate_hash);
  for (int i = 0; i < NP; i++)
  {
    ptmc_state.exited[i] = ptmc_state.source_state.exited[i];
    if (ptmc_state.source_state.ts_hash[i] == 0)
    {
      LOG_CRIT("Child state hash is 0 for tracee %d. This should not happen if the state is properly saved.", i);
    }
  }

  ptmc_state.source_state.recover_running_state();
}

/* Forward declarations for status monitor */
static void stop_status_monitor();

int exec_store()
{
  ptmc_state.state = PTMC_RUNNING;
  ptmc_state.choose = -1;
  int index = ptmc_state.cursor;
  
  /* Initialize syscall_info array to ensure deterministic state creation.
   * Only the current process's info will be updated by exec_once,
   * others should be copied from source_state to maintain consistency. */
  syscall_info info[NP];
  for (int i = 0; i < NP; i++)
  {
    info[i] = ptmc_state.source_state.child[i].si;
  }

  int ret = exec_once(info + index);
  if (ret == -1)
  {
    detsim::ui::ui_printf("Tracee #%d has exited. Please do something else.\n", index);
    return 0;
  }
  LOG_TRACE("Tracee #%d do %s", index, syscalls[info[index].nr]);

  /* always store */
  ptmc_state.dest_state = sys_state(info);
  ptmc_state.dest_state.save_metadata();
  state_tree_add(&ptmc_state.source_state, &ptmc_state.dest_state, index,
                 ptmc_state.choose);
  ptmc_state.state = PTMC_STOP;

  if (check_state() != 0)
  {
    stop_status_monitor();
    detsim::ui::ui_printf("Reach illegal state.\n");
    show_syscall_history();
  }
  stop_status_monitor();
  return 0;
}

int load_exec_store()
{
  load(0); /* load from ptmc_state.sysstate_hash */
  return exec_store();
}

extern LSS state_queue;
extern SSS state_set;
extern TSS state_tree;
extern int sigint_received;

#include <sys/time.h>
static double gettime()
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec + tv.tv_usec / 1000000.0;
}

/* ======================================================================
 * Status Monitor - Real-time statistics display
 * ====================================================================== */

/* Global statistics for status monitor */
static std::atomic<size_t> g_states_searched{0};
static std::atomic<size_t> g_states_new{0};
static std::atomic<size_t> g_queue_size{0};
static std::atomic<double> g_start_time{0.0};
static std::atomic<bool> g_monitor_running{false};

/* Depth tracking for depth vs states analysis */
static std::atomic<size_t> g_max_depth_recorded{0};  // Maximum depth seen so far
static FILE* g_depth_stat_file = nullptr;            // File for depth-statistics output
static std::mutex g_depth_stat_mutex;                // Mutex for file access

/* Data points for exponential fitting: (depth, total_states) */
static std::vector<std::pair<double, double>> g_depth_data_points;
static std::mutex g_depth_data_mutex;

/* Exponential fit parameters: states = a * exp(b * depth) */
static double g_fit_a = 0.0;  // coefficient
static double g_fit_b = 0.0;  // exponent
static std::mutex g_fit_mutex;

/* Perform exponential fit: y = a * exp(b * x)
 * Returns true if fit succeeds, false otherwise */
static bool exponential_fit(const std::vector<std::pair<double, double>>& points, 
                            double& out_a, double& out_b) {
  if (points.size() < 3) return false;  // Need at least 3 points
  
  size_t n = points.size();
  double sum_x = 0.0, sum_y = 0.0, sum_xy = 0.0, sum_x2 = 0.0;
  
  // Use log-linear fit: ln(y) = ln(a) + b*x
  for (const auto& p : points) {
    double x = p.first;
    double y_log = log(p.second);
    sum_x += x;
    sum_y += y_log;
    sum_xy += x * y_log;
    sum_x2 += x * x;
  }
  
  double denom = n * sum_x2 - sum_x * sum_x;
  if (fabs(denom) < 1e-10) return false;  // Avoid division by zero
  
  out_b = (n * sum_xy - sum_x * sum_y) / denom;
  double a_log = (sum_y - out_b * sum_x) / n;
  out_a = exp(a_log);
  
  return true;
}

/* Update fit with new data point */
static void update_depth_fit(size_t depth, size_t total_states) {
  std::lock_guard<std::mutex> lock(g_depth_data_mutex);
  g_depth_data_points.push_back({static_cast<double>(depth), static_cast<double>(total_states)});
  
  // Keep only last 50 points for local fitting
  if (g_depth_data_points.size() > 50) {
    g_depth_data_points.erase(g_depth_data_points.begin());
  }
  
  double a, b;
  if (exponential_fit(g_depth_data_points, a, b)) {
    std::lock_guard<std::mutex> fit_lock(g_fit_mutex);
    g_fit_a = a;
    g_fit_b = b;
  }
}

/* Performance diagnostics */
static std::atomic<uint64_t> g_save_time_us{0};      // Total state save time
static std::atomic<uint64_t> g_restore_time_us{0};   // Total state restore time
static std::atomic<uint64_t> g_syscall_time_us{0};   // Total syscall execution time
static std::atomic<size_t> g_save_count{0};          // Number of saves
static std::atomic<size_t> g_restore_count{0};       // Number of restores
static std::chrono::high_resolution_clock::time_point g_last_stat_time;
static std::thread g_monitor_thread;

/* Calculate real depth from state_tree by backtracking to root
 * This is the actual path length from current state to initial state */
static size_t calculate_state_depth(hash_type current_hash) {
  if (current_hash == 0) return 0;
  
  size_t depth = 0;
  hash_type hash = current_hash;
  
  /* Use a simple visited set to prevent infinite loops (shouldn't happen in tree) */
  std::unordered_set<hash_type> visited;
  
  while (state_tree.count(hash) && visited.insert(hash).second) {
    auto &parent_info = state_tree[hash];
    hash = std::get<0>(parent_info);
    if (hash == 0) break;  // Reached root
    depth++;
  }
  
  return depth;
}

/* Get terminal size */
static void get_terminal_size(int &rows, int &cols) {
  struct winsize w;
  rows = 24; cols = 80; /* defaults */
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
    rows = w.ws_row;
    cols = w.ws_col;
  }
}

/* Format bytes to human readable */
static std::string format_bytes(size_t bytes) {
  const char *units[] = {"B", "KB", "MB", "GB"};
  int unit = 0;
  double size = bytes;
  while (size >= 1024 && unit < 3) {
    size /= 1024;
    unit++;
  }
  char buf[32];
  snprintf(buf, sizeof(buf), "%.1f%s", size, units[unit]);
  return std::string(buf);
}

/* ANSI escape codes */
#define ANSI_HIDE_CURSOR "\033[?25l"
#define ANSI_SHOW_CURSOR "\033[?25h"
#define ANSI_CLEAR_LINE "\033[2K"
#define ANSI_MOVE_UP "\033[A"
#define ANSI_MOVE_DOWN "\033[B"
#define ANSI_SAVE_CURSOR "\033[s"
#define ANSI_RESTORE_CURSOR "\033[u"
#define ANSI_MOVE_TO_ROW(r) "\033[" #r ";1H"

/* Status monitor thread - updates display every second */
static void status_monitor_thread() {
  /* Wait a bit for initial states to be processed */
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  
  /* Get NCursesUI instance */
  detsim::ui::NCursesUI* ui = get_ncurses_ui();
  
  int rows, cols;
  get_terminal_size(rows, cols);
  
  /* Hide cursor (only if not using UI) */
  if (!ui) {
    detsim::ui::ui_printf(ANSI_HIDE_CURSOR);
    fflush(stdout);
  }
  
  while (g_monitor_running.load()) {
    auto &store = StateStore::instance();
    auto stats = store.get_stats();
    
    size_t l1_usage = store.get_l1_usage();
    size_t l1_cap = store.get_l1_capacity();
    size_t l2_usage = store.get_l2_usage();
    size_t l2_cap = store.get_l2_capacity();
    size_t l1_entries = store.get_l1_entry_count();
    size_t l2_entries = store.get_l2_entry_count();
    size_t io_queue_size = store.get_io_queue_size();
    size_t io_queue_cap = store.get_io_queue_capacity();
    size_t prefetch_queue_size = store.get_prefetch_queue_size();
    
    double hit_rate = stats.hit_rate();
    double elapsed = gettime() - g_start_time.load();
    double states_per_sec = elapsed > 0 ? g_states_searched.load() / elapsed : 0;
    
    size_t total_sysstates = state_set.size();
    size_t unique_states = g_states_new.load();
    size_t queue_size = g_queue_size.load();
    /* Calculate real depth from state tree */
    size_t depth = calculate_state_depth(ptmc_state.dest_state.ss_hash);
    
    /* NEW: Track depth vs states relationship - log when we see a new max depth */
    size_t current_max_depth = g_max_depth_recorded.load();
    size_t total_searched = g_states_searched.load();
    if (depth > current_max_depth && total_searched > 0) {
      // Update max depth (may fail if another thread updated, that's OK)
      if (g_max_depth_recorded.compare_exchange_weak(current_max_depth, depth)) {
        std::lock_guard<std::mutex> lock(g_depth_stat_mutex);
        if (g_depth_stat_file) {
          fprintf(g_depth_stat_file, "%zu %zu\n", depth, total_searched);
          fflush(g_depth_stat_file);
        }
        // Update exponential fit with new data point
        update_depth_fit(depth, total_searched);
      }
    }
    
    /* NEW: Track global container sizes (potential memory hogs) */
    size_t state_tree_size = state_tree.size();
    size_t state_queue_internal = StateStore::instance().queue_size();
    size_t sock_state_total = 0;
    size_t fs_state_total = 0;
    for (int i = 0; i < NP; i++) {
        sock_state_total += ptmc_state.sock_states[i].sockets().size();
        sock_state_total += ptmc_state.sock_states[i].udp_recv_buffers().size();
    }
    
    /* Update status display */
    if (ui) {
      /* Use NCursesUI status window */
      char buf[256];
      
      /* Line 1: Basic stats */
      snprintf(buf, sizeof(buf), "States: %zu | Unique: %zu | Queued: %zu | Depth: %zu | %.1f/s", 
               g_states_searched.load(), unique_states, queue_size, depth, states_per_sec);
      ui->set_status_line(1, buf);
      
      /* Line 2: Queues */
      snprintf(buf, sizeof(buf), "IO: %zu/%zu | Prefetch: %zu", 
               io_queue_size, io_queue_cap, prefetch_queue_size);
      ui->set_status_line(2, buf);
      
      /* Line 3: Cache L1 */
      snprintf(buf, sizeof(buf), "L1: %s/%s (%zu entries)", 
               format_bytes(l1_usage).c_str(), format_bytes(l1_cap).c_str(), l1_entries);
      ui->set_status_line(3, buf);
      
      /* Line 4: Cache L2 */
      snprintf(buf, sizeof(buf), "L2: %s/%s (%zu entries)", 
               format_bytes(l2_usage).c_str(), format_bytes(l2_cap).c_str(), l2_entries);
      ui->set_status_line(4, buf);
      
      /* Line 5: Time */
      int hours = (int)(elapsed / 3600);
      int minutes = ((int)elapsed % 3600) / 60;
      int seconds = (int)elapsed % 60;
      snprintf(buf, sizeof(buf), "Time: %02d:%02d:%02d | Total: %zu", 
               hours, minutes, seconds, total_sysstates);
      ui->set_status_line(5, buf);
      
      /* Line 6: Mode */
      const char *mode_str = "DFS";
      if (ptmc_state.mode == PTMC_STATE::MODE_RAND) mode_str = "RAND";
      else if (ptmc_state.mode == PTMC_STATE::MODE_BFS) mode_str = "BFS";
      snprintf(buf, sizeof(buf), "Mode: %s | Auto: %s | SIGINT: %s", 
               mode_str,
               is_auto_mode() ? "ON" : "OFF",
               sigint_received ? "YES" : "NO");
      ui->set_status_line(6, buf);
      
      /* Line 7: Process status */
      int running = 0, exited = 0;
      for (int i = 0; i < NP; i++) {
        if (ptmc_state.exited[i]) exited++;
        else running++;
      }
      snprintf(buf, sizeof(buf), "Procs: %d run | %d exit | Cursor: %d", 
               running, exited, ptmc_state.cursor);
      ui->set_status_line(7, buf);
      
      /* Line 8: Current hash */
      snprintf(buf, sizeof(buf), "Hash: %016lx", ptmc_state.dest_state.ss_hash);
      ui->set_status_line(8, buf);
      
    } else {
      /* Use ANSI escape sequences (original code) */
      /* Save cursor and move to status area (bottom 9 lines) */
      detsim::ui::ui_printf(ANSI_SAVE_CURSOR);
      
      /* Calculate status area starting row (reserve bottom 11 lines) */
      int status_start = rows - 11;
      if (status_start < 1) status_start = 1;
      
      /* Print separator line */
      detsim::ui::ui_printf("\033[%d;1H" ANSI_CLEAR_LINE "═══════════════════════════════════════════════════════════════════════════════", status_start - 1);
      
      /* Line 1: Basic stats */
      detsim::ui::ui_printf("\033[%d;1H" ANSI_CLEAR_LINE " States: %zu searched | %zu unique | %zu queued | Depth: %zu | %.1f states/s", 
             status_start, 
             g_states_searched.load(), 
             unique_states, 
             queue_size,
             depth,
             states_per_sec);
      
      /* Line 2: Worker queues */
      detsim::ui::ui_printf("\033[%d;1H" ANSI_CLEAR_LINE " Queues: IO=[%zu/%zu] | Prefetch=[%zu]", 
             status_start + 1, io_queue_size, io_queue_cap, prefetch_queue_size);
      
      /* Line 3: Cache L1 */
      double l1_pct = l1_cap > 0 ? 100.0 * l1_usage / l1_cap : 0;
      detsim::ui::ui_printf("\033[%d;1H" ANSI_CLEAR_LINE " L1 Hot: %s / %s (%.1f%%) | %zu entries", 
             status_start + 2, format_bytes(l1_usage).c_str(), format_bytes(l1_cap).c_str(), l1_pct, l1_entries);
      
      /* Line 4: Cache L2 */
      double l2_pct = l2_cap > 0 ? 100.0 * l2_usage / l2_cap : 0;
      detsim::ui::ui_printf("\033[%d;1H" ANSI_CLEAR_LINE " L2 Warm: %s / %s (%.1f%%) | %zu entries", 
             status_start + 3, format_bytes(l2_usage).c_str(), format_bytes(l2_cap).c_str(), l2_pct, l2_entries);
      
      /* Line 5: Time */
      int hours = (int)(elapsed / 3600);
      int minutes = ((int)elapsed % 3600) / 60;
      int seconds = (int)elapsed % 60;
      detsim::ui::ui_printf("\033[%d;1H" ANSI_CLEAR_LINE " Elapsed: %02d:%02d:%02d | Total SysStates: %zu", 
             status_start + 4, hours, minutes, seconds, total_sysstates);
      
      /* Line 6: Mode */
      const char *mode_str = "DFS";
      if (ptmc_state.mode == PTMC_STATE::MODE_RAND) mode_str = "RAND";
      else if (ptmc_state.mode == PTMC_STATE::MODE_DFS) mode_str = "DFS";
      else if (ptmc_state.mode == PTMC_STATE::MODE_BFS) mode_str = "BFS";
      detsim::ui::ui_printf("\033[%d;1H" ANSI_CLEAR_LINE " Mode: %s | Auto: %s | SIGINT: %s", 
             status_start + 5, mode_str,
             is_auto_mode() ? "ON" : "OFF",
             sigint_received ? "YES" : "NO");
      
      /* Line 7: Process status */
      int running = 0, exited = 0;
      for (int i = 0; i < NP; i++) {
        if (ptmc_state.exited[i]) exited++;
        else running++;
      }
      detsim::ui::ui_printf("\033[%d;1H" ANSI_CLEAR_LINE " Processes: %d running | %d exited | Current: %d", 
             status_start + 6, running, exited, ptmc_state.cursor);
      
      /* Line 8: Current hash */
      detsim::ui::ui_printf("\033[%d;1H" ANSI_CLEAR_LINE " Current: %016lx", 
             status_start + 7, ptmc_state.dest_state.ss_hash);
      
      /* Restore cursor */
      detsim::ui::ui_printf(ANSI_RESTORE_CURSOR);
      fflush(stdout);
    }
    
    /* Sleep 1 second */
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  
  /* Show cursor again (only if not using UI) */
  if (!ui) {
    detsim::ui::ui_printf(ANSI_SHOW_CURSOR);
    fflush(stdout);
  }
}

static void start_status_monitor() {
  g_monitor_running = true;
  g_start_time = gettime();
  g_last_stat_time = std::chrono::high_resolution_clock::now();
  g_max_depth_recorded = 0;
  
  /* Open depth statistics file for append */
  std::lock_guard<std::mutex> lock(g_depth_stat_mutex);
  if (g_depth_stat_file) {
    fclose(g_depth_stat_file);
  }
  g_depth_stat_file = fopen("depth_stats.txt", "a");
  if (g_depth_stat_file) {
    fprintf(g_depth_stat_file, "# depth total_states_searched\n");
    fflush(g_depth_stat_file);
  }
  
  g_monitor_thread = std::thread(status_monitor_thread);
}

static void stop_status_monitor() {
  g_monitor_running = false;
  if (g_monitor_thread.joinable()) {
    g_monitor_thread.join();
  }
  
  /* Close depth statistics file */
  {
    std::lock_guard<std::mutex> lock(g_depth_stat_mutex);
    if (g_depth_stat_file) {
      fclose(g_depth_stat_file);
      g_depth_stat_file = nullptr;
    }
  }
  
  /* Clear depth data points for next run */
  {
    std::lock_guard<std::mutex> data_lock(g_depth_data_mutex);
    g_depth_data_points.clear();
  }
  {
    std::lock_guard<std::mutex> fit_lock(g_fit_mutex);
    g_fit_a = 0.0;
    g_fit_b = 0.0;
  }
  g_max_depth_recorded = 0;
  
  /* If not using UI, clear status area and show cursor */
  detsim::ui::NCursesUI* ui = get_ncurses_ui();
  if (!ui) {
    int rows, cols;
    get_terminal_size(rows, cols);
    detsim::ui::ui_printf(ANSI_SAVE_CURSOR);
    for (int i = rows - 10; i <= rows; i++) {
      detsim::ui::ui_printf("\033[%d;1H" ANSI_CLEAR_LINE, i);
    }
    detsim::ui::ui_printf(ANSI_RESTORE_CURSOR);
    fflush(stdout);
    
    /* Show cursor again */
    detsim::ui::ui_printf(ANSI_SHOW_CURSOR);
    fflush(stdout);
  }
}

bool state_to_be_discarded(int index, syscall_info *infos) {
  if (index == 0) 
  {
    uintptr_t raft = eval<uintptr_t>("((raft_server_private_t *)raft)", 0);
    if (raft == 0) return false; // If we can't evaluate, don't discard
    int state = eval<int>("((raft_server_private_t *)raft)->state", 0);
    if (state == 2 || state == 3) {
      return true;
    }
  }

  uintptr_t raft = eval<uintptr_t>("((raft_server_private_t *)raft)", index);
  if (raft == 0) return false; // If we can't evaluate, don't discard
  int state = eval<int>("((raft_server_private_t *)raft)->state", index);
  if (state == 3 && infos[index].nr == SYS_gettimeofday && ptmc_state.choose == 1)
  {
    return true;
  }

  return false;
}

/* auto mode: */
int exec_cont()
{
  double start_time = gettime();
  // srand(time(NULL));
  sys_state *state_fetched = NULL;
  syscall_info syscall_info[NP];
  
  /* Statistics for this execution only */
  size_t states_searched_this_run = 0;
  size_t states_new_this_run = 0;
  
  /* Reset StateStore statistics for this run */
  StateStore::instance().reset_stats();
  
  /* Initialize global statistics */
  g_states_searched = 0;
  g_states_new = 0;
  g_queue_size = 0;

  StateStore::instance().queue_clear();
  state_queue_append(&ptmc_state.dest_state);
  
  /* Check if this is a new state or was already in state_set */
  if (!state_set.count(ptmc_state.dest_state.ss_hash))
  {
    states_new_this_run++;
  }
  state_set.emplace(ptmc_state.dest_state.ss_hash);
  states_searched_this_run++;
  
  /* Update global stats */
  g_states_searched = states_searched_this_run;
  g_states_new = states_new_this_run;

  ptmc_state.n_choose = 0;
  ptmc_state.choose = 0;
  
  /* Start status monitor in auto mode */
  start_status_monitor();

  /* until no state in queue */
  while ((state_fetched = state_queue_extract()) != NULL)
  {
    std::vector<int> indexes;
    switch (ptmc_state.mode)
    {
    case PTMC_STATE::MODE_RAND:
      indexes.push_back(rand() % NP);
      break;
    default:
      for (int k = 0; k < NP; k++)
      {
        if (!ptmc_state.source_state.exited[k])
          indexes.push_back(k);
      }
      shuffle(indexes.begin(), indexes.end(), std::default_random_engine(rand()));
    }
    
    for (auto &i : indexes)
    {
    again:
      ptmc_state.source_state = *state_fetched;
      auto &s = ptmc_state.source_state;

      for (int j = 0; j < NP; j++)
        ptmc_state.exited[j] = s.exited[j];

      if (ptmc_state.exited[i])
        continue;

      for (int j = 0; j < NP; j++)
        if (s.ts_hash[j] == 0)
          LOG_CRIT("Child state hash is 0 for tracee %d. This should not happen if the state is properly saved.", j);

      /* Time state restore */
      auto t_restore_start = std::chrono::high_resolution_clock::now();
      s.recover_running_state();
      auto t_restore_end = std::chrono::high_resolution_clock::now();
      g_restore_time_us += std::chrono::duration_cast<std::chrono::microseconds>(
          t_restore_end - t_restore_start).count();
      g_restore_count++;
      
      ptmc_state.cursor = i;

      /* Initialize syscall_info from source_state for all processes
       * to ensure deterministic state creation */
      for (int j = 0; j < NP; j++)
        syscall_info[j] = s.child[j].si;

      int ckpt = exec_once(syscall_info + i);

      ptmc_state.dest_state = sys_state(syscall_info);

      /* Time state save */
      auto t_save_start = std::chrono::high_resolution_clock::now();
      ptmc_state.dest_state.save_metadata();
      auto t_save_end = std::chrono::high_resolution_clock::now();
      g_save_time_us += std::chrono::duration_cast<std::chrono::microseconds>(
          t_save_end - t_save_start).count();
      g_save_count++;
      if (ckpt != CKPT_DISCARD && !state_to_be_discarded(i, syscall_info))
      {
        bool is_new = !state_set.count(ptmc_state.dest_state.ss_hash);
        if (is_new || ptmc_state.mode == PTMC_STATE::MODE_RAND)
        {
          state_queue_append(&ptmc_state.dest_state);
          state_set.emplace(ptmc_state.dest_state.ss_hash);
          states_new_this_run++;
        }
      }
      states_searched_this_run++;
      state_tree_add(&s, &ptmc_state.dest_state, i,
                     ptmc_state.n_choose ? ptmc_state.choose : -1);
      if (check_state() != 0)
      {
        stop_status_monitor();
        detsim::ui::ui_printf("Stopped for illegal state. Searched for %zu sys_states (new: %zu)\n",
               states_searched_this_run, states_new_this_run);
        show_syscall_history();
        double time_used = gettime() - start_time;
        detsim::ui::ui_printf("Time elapsed: %lfs, speed = %lf states/s\n", time_used,
               states_searched_this_run / time_used);
        StateStore::instance().print_stats();
        state_fetched->clear();
        delete state_fetched;
        return 0;
      }

      if (ptmc_state.n_choose)
      {
        if (ptmc_state.mode == PTMC_STATE::MODE_BFS)
        {
          ptmc_state.choose++;
          if (ptmc_state.choose < ptmc_state.n_choose)
            goto again;
          ptmc_state.choose = 0;
        }
        else if (ptmc_state.mode == PTMC_STATE::MODE_RAND)
        {
        }
      }
    }
    // Clear state_fetched before delete to free memory early
    state_fetched->clear();
    delete state_fetched;
    
    /* Update global statistics for monitor */
    g_states_searched = states_searched_this_run;
    g_states_new = states_new_this_run;
    g_queue_size = StateStore::instance().queue_size();

    if (sigint_received)
    {
      stop_status_monitor();
      detsim::ui::ui_printf("Program received signal SIGINT, Interrupt.\n");
      /* Wait for pending StateStore writes to complete to ensure data consistency */
      detsim::ui::ui_printf("Flushing pending state writes...\n");
      StateStore::instance().wait_for_completion();
      /* Flush SysStateStore index to merge incremental entries */
      SysStateStore::instance().flush_index();
      detsim::ui::ui_printf("Searched for %zu sys_states (new: %zu), total unique: %zu\n", 
             states_searched_this_run, states_new_this_run, state_set.size());
      if (ptmc_state.dest_state.ss_hash == 0)
        LOG_CRIT("Current state hash is 0. This should not happen if states are properly saved.");
      show_syscall_history();
      stop_status_monitor();
      double time_used = gettime() - start_time;
      detsim::ui::ui_printf("Time elapsed: %lfs, speed = %lf states/s\n", time_used,
             states_searched_this_run / time_used);
      StateStore::instance().print_stats();
      sigint_received = 0;
      state_set.clear();
      return 0;
    }
  }
  stop_status_monitor();
  detsim::ui::ui_printf("Complete explore all %zu sys_states (new: %zu), total unique: %zu\n", 
         states_searched_this_run, states_new_this_run, state_set.size());
  show_syscall_history();
  double time_used = gettime() - start_time;
  detsim::ui::ui_printf("Time elapsed: %lfs, speed = %lf states/s\n", time_used,
         states_searched_this_run / time_used);
  StateStore::instance().print_stats();
  return 0;
}

/* start tracees, and execute to a FIRST state */
void init_state()
{
  /* initialization for multiple process */
  // setpgid(0, pgid);
  setbuf(stdout, NULL);
  atexit(exit_all);
  signal(SIGTERM, exit_all);
  // signal(SIGINT, exit_all);

  pids[0] = fork();
  for (int i = 0; i < NP - 1; i++)
  {
    if (pids[i])
      pids[i + 1] = fork();
    else
    {
      // setpgid(0, pgid);
      start_tracee(i);
    }
  }

  if (!pids[NP - 1])
    start_tracee(NP - 1);

  detsim::ui::ui_printf("Instance #pid = %d\n", getpid());

  /* Initialize FdManagers and link to states */
  for (int i = 0; i < NP; i++)
  {
    ptmc_state.fd_managers[i] = std::make_shared<FdManager>();
    ptmc_state.fs_states[i].set_fd_manager(ptmc_state.fd_managers[i]);
    ptmc_state.sock_states[i].set_fd_manager(ptmc_state.fd_managers[i]);
  }

  /* here goes to first state */
  /* initialize tracees' state */
  for (int i = 0; i < NP; i++)
  {
    ptmc_state.cursor = i;
    init_tracee_state(i);
  }

  /* Record first sys_state. Notice that the syscall_info need no recording */
  struct syscall_info syscall_info[NP];
  memset(syscall_info, 0, sizeof(syscall_info));
  ptmc_state.cursor = -1;
  ptmc_state.dest_state = sys_state(syscall_info);
  ptmc_state.dest_state.save_metadata();

  /* add to link and tree */
  state_queue_append(&ptmc_state.dest_state);

  ptmc_state.state = PTMC_STOP;
}
