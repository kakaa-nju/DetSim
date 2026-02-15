#include "engine.h"
#include "common.h"
#include "debug.h"
#include "emu.h"
#include "fsstate.h"
#include "guest.h"
#include "monitor.h"
#include "sockstate.h"
#include "state.h"
#include "utils.h"

#include <assert.h>
#include <cstddef>
#include <cstdlib>
#include <fcntl.h>
#include <readline/readline.h>
#include <signal.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define pids ptmc_state.pids
#define PTRACE_TRAP_SIG (SIGTRAP | 0x80)
extern FILE *log_fp;

/* prepare tracee for ptrace, and execve */
static void start_tracee(int id)
{
  signal(SIGINT, SIG_IGN);
  ptrace_right(PTRACE_TRACEME, 0, NULL, NULL);
  // printf("Will start executable %s\n", ptmc_state.tracee[id].executable);
  if (log_fp != stdout)
    fclose(log_fp);
  raise(SIGSTOP);

  execv(ptmc_state.tracee[id].executable, ptmc_state.tracee[id].argv);

  perror("exec");
}

// static pid_t pgid = 114514;

static void exit_all(int _)
{
  kill(0, SIGTERM);
  exit(EXIT_SUCCESS);
}

static void exit_all(void)
{
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
  syscall_info ret;

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

  static char *line_read;
  if (!is_auto_mode() && ptmc_state.n_choose != 0) /* ask for choose */
  {
    printf("Here comes with %d choices.\n", ptmc_state.n_choose);
    line_read = readline("You choose? [0, N): ");
    while (sscanf(line_read, "%d", &ptmc_state.choose) != 1 ||
           ptmc_state.choose < 0 || ptmc_state.choose >= ptmc_state.n_choose)
    {
      free(line_read);
      line_read = readline("Please make legal choice: ");
    }
    free(line_read);
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
      if (ret >= 0) /* for willemt/raft, a `poll_msg` cycle will recv() until
                       nothing can be received */
      {
        return CKPT_NO;
      }
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
      ret = tracee_do_open(pid, "/dev/null", O_RDWR);
      tracee_set_rax(pid, ret);
      tracee_set_orig_rax(pid, ret);
      info->rval = ret;
      emu_socket(ret, info->args[0], info->args[1], info->args[2]);
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
      return CKPT_NO;
    case SYS_connect:
      ret = emu_connect(info->args[0], (const struct sockaddr *)info->args[1],
                        info->args[2]);
      tracee_do_open(pid, "/dev/null", O_RDWR);
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
        ret = guest_do_vfs_openat(AT_FDCWD, (const char *)info->args[0], info->args[1], info->args[2]);
        tracee_set_rax(pid, ret);
        info->rval = ret;
        return CKPT_YES;
    case SYS_openat:
      ret = guest_do_vfs_openat(info->args[0], (const char *)info->args[1], info->args[2], info->args[3]);
      tracee_set_rax(pid, ret);
      info->rval = ret;
      return CKPT_YES;
    case SYS_read:
      ret = guest_do_vfs_read(info->args[0], (void *)info->args[1], info->args[2]);
      tracee_set_rax(pid, ret);
      info->rval = ret;
      return CKPT_YES;
    case SYS_write:
      ret = guest_do_vfs_write(info->args[0], (const void *)info->args[1], info->args[2]);
      tracee_set_rax(pid, ret);
      info->rval = ret;
      return CKPT_YES;
    case SYS_close:
        ret = guest_do_vfs_close(info->args[0]);
        tracee_set_rax(pid, ret);
        info->rval = ret;
        return CKPT_YES;
    case SYS_lseek:
        ret = guest_do_vfs_lseek(info->args[0], info->args[1], info->args[2]);
        tracee_set_rax(pid, ret);
        info->rval = ret;
        return CKPT_YES;
    case SYS_stat:
        ret = guest_do_vfs_stat((const char *)info->args[0], (struct stat *)info->args[1]);
        tracee_set_rax(pid, ret);
        info->rval = ret;
        return CKPT_YES;
    case SYS_fstat:
        ret = guest_do_vfs_fstat(info->args[0], (struct stat *)info->args[1]);
        tracee_set_rax(pid, ret);
        info->rval = ret;
        return CKPT_YES;

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
  tracee_get_freepage(pid);
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
    if (!success)
      LOG_ERROR("Failed to evaluate expression \"%s\"", assertion.c_str());
    if (val == false)
    {
      printf("Assertion \"%s\" fail at sys_state " HASH_FORMAT "!\n",
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

static void load()
{
  /* load only if state hash is set */
  assert(ptmc_state.state == PTMC_PRELOAD);

  ptmc_state.state = PTMC_LOADED;
  ptmc_state.source_state = sys_state(ptmc_state.sysstate_hash);
  for (int i = 0; i < NP; i++)
    ptmc_state.exited[i] = ptmc_state.source_state.exited[i];

  ptmc_state.source_state.recover_running_state();
}

int exec_store()
{
  ptmc_state.state = PTMC_RUNNING;
  ptmc_state.choose = -1;
  int index = ptmc_state.cursor;
  syscall_info info[NP];

  int ret = exec_once(info + index);
  if (ret == -1)
  {
    printf("Tracee #%d has exited. Please do something else.\n", index);
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
    printf("Reach illegal state.\n");
    show_syscall_history();
  }
  return 0;
}

int load_exec_store()
{
  load();
  return exec_store();
}

extern LSS state_queue;
extern SSS state_set;
extern int sigint_received;

#include <sys/time.h>
static double gettime()
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec + tv.tv_usec / 1000000.0;
}

/* auto mode: */
int exec_cont()
{
  double start_time = gettime();
  srand(time(NULL));
  sys_state *state_fetched = NULL;
  syscall_info syscall_info[NP];

  state_queue.clear();
  state_queue_append(&ptmc_state.dest_state);
  state_set.emplace(ptmc_state.dest_state.ss_hash);

  ptmc_state.n_choose = 0;
  ptmc_state.choose = 0;

  /* until no state in queue */
  while ((state_fetched = state_queue_extract()) != NULL)
  {
    // int i = rand() % NP;
    for (int i = 0; i < NP; i++)
    {
    again:
      ptmc_state.source_state = *state_fetched;
      auto &s = ptmc_state.source_state;

      for (int j = 0; j < NP; j++)
        ptmc_state.exited[j] = s.exited[j];

      if (ptmc_state.exited[i])
        continue;

      s.recover_running_state();
      ptmc_state.cursor = i;

      for (int j = 0; j < NP; j++)
        if (j != i && ptmc_state.exited[j] != true)
          syscall_info[j] = s.child[j].si;

      int ckpt = exec_once(syscall_info + i);

      ptmc_state.dest_state = sys_state(syscall_info);

      if (ckpt != CKPT_DISCARD)
      {
        ptmc_state.dest_state.save_metadata();
        if (!state_set.count(ptmc_state.dest_state.ss_hash))
        {
          state_queue_append(&ptmc_state.dest_state);
          state_set.emplace(ptmc_state.dest_state.ss_hash);
        }
        state_tree_add(&s, &ptmc_state.dest_state, i,
                       ptmc_state.n_choose ? ptmc_state.choose : -1);
      }
      if (check_state() != 0)
      {
        printf("Stopped for illegal state. Searched for %ld sys_states\n",
               state_set.size());
        show_syscall_history();
        double time_used = gettime() - start_time;
        printf("Time elapsed: %lfs, speed = %lf states/s\n", time_used,
               state_set.size() / time_used);
        delete state_fetched;
        return 0;
      }

      if (ptmc_state.n_choose)
      {
        ptmc_state.choose++;
        if (ptmc_state.choose < ptmc_state.n_choose)
          goto again;
        ptmc_state.choose = 0;
      }
    }
    delete state_fetched;

    if (sigint_received)
    {
      printf("Program received signal SIGINT, Interrupt.\n");
      printf("Searched for %ld sys_states\n", state_set.size());
      show_syscall_history();
      double time_used = gettime() - start_time;
      printf("Time elapsed: %lfs, speed = %lf states/s\n", time_used,
             state_set.size() / time_used);
      sigint_received = 0;
      return 0;
    }
  }
  printf("Complete explore all %ld sys_states\n", state_set.size());
  show_syscall_history();
  double time_used = gettime() - start_time;
  printf("Time elapsed: %lfs, speed = %lf states/s\n", time_used,
         state_set.size() / time_used);
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

  printf("Instance #pid = %d\n", getpid());

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
