#include "state_transition.h"
#include "log_wrapper.h"
#include "common.h"
#include "guest.h"
#include "emu.h"
#include "utils.h"
#include "proc_status.h"
#include "ncurses_ui.h"
#include "state_store.h"
#include <string.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <readline/readline.h>

#include <monitor.h>

#define pids ptmc_state.pids

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
      do_nosys(pid);
      break;

    /* act normally. SYS_sched_yield is preserved for state check */
    case SYS_sched_yield:
    default:
      break;
  }
}

int analyze_choose(struct syscall_info &info)
{
  // Check if syscall has predefined choose count
  int predefined = choose_many[info.nr];
  if (predefined > 0)
  {
    return predefined;
  }

  // For recvfrom, check number of available messages for message reordering
  if (info.nr == SYS_recvfrom)
  {
    int fd = info.args[0];
    int choices = emu_recvfrom_get_choices(fd);
    // Only provide choice when there are 2+ messages (allow reordering)
    if (choices >= 2)
    {
      return 2; // 0=receive first message, 1=receive second message
    }
    return 0; // No choice when < 2 messages
  }

  return 0;
}

extern "C" detsim::ui::NCursesUI *get_ncurses_ui();
/* parse syscall_info */
static int on_syscall_exit(pid_t pid, struct syscall_info &info)
{
  /* calculate choose_n from context */
  ptmc_state.n_choose = analyze_choose(info);

  if (!is_auto_mode() && ptmc_state.n_choose != 0) /* ask for choose */
  {
    // Check if there's a preset choice from batch file
    if (ptmc_state.batch_choice_preset >= 0 &&
        ptmc_state.batch_choice_preset < ptmc_state.n_choose)
    {
      ptmc_state.choose = ptmc_state.batch_choice_preset;
      ptmc_state.batch_choice_preset = -1; // Clear preset after use
      UI_LOG_INFO("Using batch preset choice: %d", ptmc_state.choose);
    }
    else
    {
      detsim::ui::NCursesUI *ui = get_ncurses_ui();
      if (ui)
      {
        /* Use NCursesUI prompt */
        char prompt_buf[256];
        snprintf(prompt_buf, sizeof(prompt_buf), "Choose from %d options",
                 ptmc_state.n_choose);
        ptmc_state.choose =
            ui->prompt_int(prompt_buf, 0, 0, ptmc_state.n_choose - 1);
      }
      else
      {
        /* Fallback to readline */
        static char *line_read;
        UI_LOG_INFO("Here comes with %d choices.", ptmc_state.n_choose);
        line_read = readline("You choose? [0, N): ");
        while (sscanf(line_read, "%d", &ptmc_state.choose) != 1 ||
               ptmc_state.choose < 0 ||
               ptmc_state.choose >= ptmc_state.n_choose)
        {
          free(line_read);
          line_read = readline("Please make legal choice: ");
        }
        free(line_read);
      }
    }
  }
  else if (is_auto_mode() && ptmc_state.n_choose != 0 &&
           ptmc_state.mode == PTMC_STATE::MODE_RAND)
  {
    ptmc_state.choose = rand() % ptmc_state.n_choose;
  }
  else if (is_auto_mode() && ptmc_state.n_choose != 0 &&
           ptmc_state.mode == PTMC_STATE::MODE_DFS)
  {
    if (ptmc_state.choose < 0)
    {
      ptmc_state.choose = rand() % ptmc_state.n_choose;
    }
  }

  /* Check and dump for each generated next state */
  long ret;
  switch (info.nr)
  {
      /* syscalls need scheduled */
    case SYS_recvfrom:
      ret = emu_recvfrom(info.args[0], (void *)info.args[1], info.args[2],
                         info.args[3], (struct sockaddr *)info.args[4],
                         (socklen_t *)info.args[5]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      if (ret < 0 && ptmc_state.n_choose < 2) /* for willemt/raft, a `poll_msg`
                       cycle will recv() until nothing can be received */
      {
        return CKPT_NO;
      }
      return CKPT_YES;
      break;
    case SYS_sched_yield:
      break;
    case SYS_sendto:
      ret = emu_sendto(info.args[0], (void *)info.args[1], info.args[2],
                       info.args[3], (struct sockaddr *)info.args[4],
                       info.args[5]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_NO;
    case SYS_gettimeofday:
      ret = emu_gettimeofday((struct timeval *)info.args[0],
                             (struct timezone *)info.args[1]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_YES;
    case SYS_socket:
      ret = emu_socket(info.args[0], info.args[1], info.args[2]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_NO;
    case SYS_listen:
      ret = emu_listen(info.args[0], info.args[1]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_NO;
    case SYS_bind:
      ret = emu_bind(info.args[0], (const struct sockaddr *)info.args[1],
                     info.args[2]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_YES;
    case SYS_connect:
      ret = emu_connect(info.args[0], (const struct sockaddr *)info.args[1],
                        info.args[2]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_NO;

    case SYS_chdir:
    {
      // Read the path string from the tracee memory and update cwd on success
      uintptr_t guest_ptr = info.args[0];
      // helper: read null-terminated string from guest using memcpy_guest2host
      std::string path;
      char ch;
      size_t idx = 0;
      do
      {
        memcpy_guest2host(&ch, (const void *)(guest_ptr + idx), 1);
        if (ch != '\0')
          path.push_back(ch);
        idx++;
      } while (ch != '\0');

      if ((long)info.rval >= 0)
      {
        // success: update cwd of current fs_state
        ptmc_state.fs_states[ptmc_state.cursor].set_cwd(path);
      }
      // reflect syscall return
      tracee_set_rax(pid, info.rval);
      return CKPT_YES;
    }

    case SYS_exit_group:
    case SYS_exit:
      return CKPT_EXIT;

    /* VFS Handlers */
    case SYS_open:
      ret = emu_vfs_openat(AT_FDCWD, (const char *)info.args[0], info.args[1],
                           info.args[2]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_NO;
    case SYS_openat:
      ret = emu_vfs_openat(info.args[0], (const char *)info.args[1],
                           info.args[2], info.args[3]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_NO;
    case SYS_read:
      ret = emu_vfs_read(info.args[0], (void *)info.args[1], info.args[2]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_NO;
    case SYS_write:
      ret = emu_vfs_write(info.args[0], (const void *)info.args[1],
                          info.args[2]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_YES;
    case SYS_close:
      ret = emu_vfs_close(info.args[0]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_NO;
    case SYS_lseek:
      ret = emu_vfs_lseek(info.args[0], info.args[1], info.args[2]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_NO;
    case SYS_stat:
      ret = emu_vfs_stat((const char *)info.args[0],
                         (struct stat *)info.args[1]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_NO;
    case SYS_fstat:
      ret = emu_vfs_fstat(info.args[0], (struct stat *)info.args[1]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_NO;

    case SYS_nanosleep:
    case SYS_brk:
      return CKPT_NO;

    default:
      return CKPT_NO;
  }
  return CKPT_YES;
}
/* Do one syscall, with its effects altered */
int do_one_syscall(pid_t pid, syscall_info &si)
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
    switch (WSTOPSIG(wstatus))
    {
      case SIGSEGV:
      case SIGABRT:
        LOG_INFO("ptrace_syscall has wrong stop status. WIFSTOPPED=%s and "
                 "WSTOPSIG=%s.",
                 WIFSTOPPED(wstatus) ? "true" : "false",
                 strsignal(WSTOPSIG(wstatus)));
        show_syscall_history();
        tracee_backtrace(pid);
        ptmc_state.status[ptmc_state.cursor] = dstatus_crash(WSTOPSIG(wstatus));

        return CKPT_STOP;
      default:
        /* non fatal signal. dismiss */
        ptrace_right(PTRACE_SYSCALL, pid, NULL, NULL);
        waitpid(pid, &wstatus, 0);
        assert(WIFSTOPPED(wstatus) && (WSTOPSIG(wstatus) == PTRACE_TRAP_SIG));
    }
  }

  ptrace_right(PTRACE_GET_SYSCALL_INFO, pid, (void *)sizeof(info), &info);
  assert(info.op == PTRACE_SYSCALL_INFO_ENTRY);

  on_syscall_enter(pid, info.entry.nr);
  si.nr = info.entry.nr;
  for (int i = 0; i < 6; i++)
    si.args[i] = info.entry.args[i];

  if (si.nr == SYS_exit || si.nr == SYS_exit_group)
    ptmc_state.status[ptmc_state.cursor] = dstatus_exit(info.entry.args[0]);

  /* exit */
  ptrace_right(PTRACE_SYSCALL, pid, NULL, NULL);
  waitpid(pid, &wstatus, 0);
  assert(WIFSTOPPED(wstatus) && (WSTOPSIG(wstatus) == PTRACE_TRAP_SIG));

  ptrace_right(PTRACE_GET_SYSCALL_INFO, pid, (void *)sizeof(info), &info);
  assert(info.op == PTRACE_SYSCALL_INFO_EXIT);
  si.rval = info.exit.rval;
  return on_syscall_exit(pid, si);
}


/* loaded, exec 1 STEP, not stored. Save Last syscall into `info` *
 * 1 STEP = critical syscall step */
int exec_once(const sys_state &s, syscall_info &info)
{
  int index = ptmc_state.cursor;
  pid_t pid = pids[index];

  if (DISDEAD(s.status[index]))
    return -1;
  LOG_DEBUG("exec_once from state " HASH_FORMAT ":" HASH_FORMAT, s.ss_hash,
            s.ts_hash[index]);

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
        LOG_INFO("Tracee %d exited with status %d", index,
                    DEXITSTATUS(ptmc_state.status[index]));
        return result;
      case CKPT_STOP:
        LOG_INFO("Tracee %d stopped for signal %s", index,
                    strsignal(DSTOPSIG(ptmc_state.status[index])));
        return result;
    }
  }
  return 0;
}

TransitionResult state_transition(
    const sys_state& source_state,
    int process_index)
{
    TransitionResult result;
    ptmc_state.cursor = process_index;
    
    // restore state to tracees with timing
    auto t1 = std::chrono::high_resolution_clock::now();
    source_state.recover_running_state();
    auto t2 = std::chrono::high_resolution_clock::now();
    result.restore_time = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1);
    
    // do syscalls
    syscall_info new_syscalls[NP];
    for (int i = 0; i < NP; i++)
        new_syscalls[i] = source_state.child[i].si;

    int ckpt = exec_once(source_state, new_syscalls[process_index]);
    
    // save state with timing
    auto t3 = std::chrono::high_resolution_clock::now();
    result.new_state = sys_state(new_syscalls);
    auto t4 = std::chrono::high_resolution_clock::now();
    result.save_time = std::chrono::duration_cast<std::chrono::microseconds>(t4 - t3);
    
    // add to state tree for future exploration
    state_tree_add(source_state, result.new_state, process_index, 
                   ptmc_state.n_choose ? ptmc_state.choose : -1);
    ptmc_state.running_state = result.new_state;
    return result;
}

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
  syscall_info ret = {}; /* Zero-initialize all fields */

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
    LOG_INFO("Neutralize set_tid_address, return 114514");
  }
  /* getrandom */
  if (ret.nr == SYS_getrandom)
  {
    memcpy_host2guest((void *)ret.args[0], "\0\0\0\0\0\0\0\0", 8);
    LOG_INFO("Neutralize getrandom(%p, %d, %d)", (void *)ret.args[0], ret.args[1],
             ret.args[2]);
  }
  /* rseq mask */
  if (ret.nr == SYS_rseq)
  {
    rseq_struct[pid] = (void *)ret.args[0];
    rseq_len[pid] = ret.args[1];
    LOG_INFO("Record rseq(%p, %d)", (void *)ret.args[0], ret.args[1]);
  }
  
  if (ret.nr == SYS_getcpu)
  {
    tracee_set_rax(pid, 0);
    tracee_set_orig_rax(pid, 0);
    LOG_INFO("Neutralize getcpu, return 0 and do nothing to arguments");
  }
  return ret;
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
  patch_at_random(pid);

  /* skip non-user code: *
   * Libc initialization ends with an `munmap` call */
  while (extract_one_syscall(pid).nr != stop_nr)
    ;
  patch_cpu_features_elf(pid);

  /* Preserve memory for parameters. Need recover registers *
   * for the original first syscall. */
  tracee_reserve_temp_page(pid);
  tracee_write_mem(pid, (void *)(scratch_page + 0xff0),
                   "\x0f\x05\x0f\x05\x0f\x05\x0f\x05", 8);
}


/* prepare tracee for ptrace, and execve */
static void start_tracee(int id)
{
  signal(SIGINT, SIG_IGN);
  ptrace_right(PTRACE_TRACEME, 0, NULL, NULL);
  // detsim::ui::ui_printf("Will start executable %s\n",
  // ptmc_state.tracee[id].executable);
  if (log_fp != stdout)
    fclose(log_fp);
  raise(SIGSTOP);

  execv(ptmc_state.tracee[id].executable, ptmc_state.tracee[id].argv);

  perror("exec");
}

int state_initialization()
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

  sys_state first_state(syscall_info);
  ptmc_state.running_state = first_state;
  return 0;
}