#include "state_transition.h"
#include "common.h"
#include "emu.h"
#include "fsstate.h"
#include "futexstate.h"
#include "guest.h"
#include "log_wrapper.h"
#include "ncurses_ui.h"
#include "proc_status.h"
#include "state_store.h"
#include "utils.h"
#include <fcntl.h>
#include <readline/readline.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#include <monitor.h>

#define pids ptmc_state.pids

// Track VFS mmap operations between enter and exit
struct VfsMmapState
{
  bool active;
  int fd;
  off_t offset;
  size_t length;
};
static VfsMmapState g_vfs_mmap = {false, -1, 0, 0};

static void do_nosys(int pid)
{
  tracee_set_orig_rax(pid, -1);
  tracee_set_rax(pid, -1);
}

/* Check if syscall need to be done */
static void on_syscall_enter(pid_t pid, int nr)
{
  if (nr == SYS_mmap)
  {
    // Check if this is a VFS file mmap
    int fd = ptrace(PTRACE_PEEKUSER, pid, offsetof(struct user_regs_struct, r8),
                    NULL);
    auto &fs = ptmc_state.fs_states[ptmc_state.cursor];
    if (fd >= 0 && fs.open_files.find(fd) != fs.open_files.end())
    {
      // Save VFS mmap state
      g_vfs_mmap.active = true;
      g_vfs_mmap.fd = fd;
      g_vfs_mmap.offset = ptrace(PTRACE_PEEKUSER, pid,
                                 offsetof(struct user_regs_struct, r9), NULL);
      g_vfs_mmap.length = ptrace(PTRACE_PEEKUSER, pid,
                                 offsetof(struct user_regs_struct, rsi), NULL);
      int prot = ptrace(PTRACE_PEEKUSER, pid,
                        offsetof(struct user_regs_struct, rdx), NULL);
      int flags = ptrace(PTRACE_PEEKUSER, pid,
                         offsetof(struct user_regs_struct, r10), NULL);
      // Change to anonymous mmap (fd = -1)
      ptrace(PTRACE_POKEUSER, pid, offsetof(struct user_regs_struct, rdx),
             prot | PROT_WRITE);
      ptrace(PTRACE_POKEUSER, pid, offsetof(struct user_regs_struct, r10),
             flags | MAP_ANONYMOUS);
      ptrace(PTRACE_POKEUSER, pid, offsetof(struct user_regs_struct, r8), -1);
    }
  }

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
    case SYS_accept:
    case SYS_accept4:
    case SYS_epoll_create:
    case SYS_epoll_create1:
    case SYS_epoll_ctl:
    case SYS_epoll_wait_old:
    case SYS_epoll_wait:
    case SYS_epoll_pwait:
    case SYS_epoll_pwait2:
    case SYS_setsockopt:
    case SYS_getsockopt:
    case SYS_fsync:
    case SYS_fdatasync:
    case SYS_fcntl:
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
    case SYS_pipe:
    case SYS_pipe2:
      do_nosys(pid);
      break;

    case SYS_clone:;
      tracee_set_rdi(pid, tracee_get_rdi(pid) | CLONE_PTRACE);
      break;

    case SYS_clone3:;
      {
        // clone3 takes a struct clone_args *args and size_t size
        // struct clone_args {
        //     uint64_t flags;
        //     uint64_t pidfd;
        //     uint64_t child_tid;
        //     uint64_t parent_tid;
        //     uint64_t exit_signal;
        //     uint64_t stack;
        //     uint64_t stack_size;
        //     ...
        // };
        uint64_t args_ptr = ptrace(PTRACE_PEEKUSER, pid,
                                   offsetof(struct user_regs_struct, rdi), NULL);
        // Read current flags from clone_args
        uint64_t flags = ptrace(PTRACE_PEEKDATA, pid, args_ptr, NULL);
        // Add CLONE_PTRACE flag
        flags |= CLONE_PTRACE;
        ptrace(PTRACE_POKEDATA, pid, args_ptr, flags);
        LOG_INFO("Modified clone3 flags at %p to add CLONE_PTRACE", (void*)args_ptr);
      }
      break;

    /* Do not allow process exit_group, but allow thread exit (SYS_exit).
     * SYS_exit is handled in do_one_syscall to properly clean up thread state. */
    case SYS_exit_group:
      do_nosys(pid);
      break;
    case SYS_exit:
      /* Allow SYS_exit to execute - thread cleanup happens after syscall completes */
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

/* ======================================================================
 * Choice Handling
 * ====================================================================== */

static void handle_user_choice()
{
  // Check if there's a preset choice from batch file
  if (ptmc_state.batch_choice_preset >= 0 &&
      ptmc_state.batch_choice_preset < ptmc_state.n_choose)
  {
    ptmc_state.choose = ptmc_state.batch_choice_preset;
    ptmc_state.batch_choice_preset = -1; // Clear preset after use
    UI_LOG_INFO("Using batch preset choice: %d", ptmc_state.choose);
    return;
  }

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

static void handle_auto_choice()
{
  if (ptmc_state.n_choose == 0)
    return;

  switch (ptmc_state.mode)
  {
    case PTMC_STATE::MODE_RAND:
      ptmc_state.choose = rand() % ptmc_state.n_choose;
      break;
    case PTMC_STATE::MODE_DFS:
      if (ptmc_state.choose < 0)
      {
        ptmc_state.choose = rand() % ptmc_state.n_choose;
      }
      break;
    default:
      break;
  }
}

/* parse syscall_info */
static int on_syscall_exit(pid_t pid, struct syscall_info &info)
{
  /* calculate choose_n from context */
  ptmc_state.n_choose = analyze_choose(info);

  if (ptmc_state.n_choose != 0)
  {
    if (is_auto_mode())
    {
      handle_auto_choice();
    }
    else
    {
      handle_user_choice();
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
      return CKPT_YES;
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
      return CKPT_YES;
    case SYS_listen:
      ret = emu_listen(info.args[0], info.args[1]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_YES;
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
      return CKPT_YES;

    case SYS_chdir:
    {
      ret = emu_chdir((const char *)info.args[0]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_NO;
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
      return CKPT_YES;
    case SYS_openat:
      ret = emu_vfs_openat(info.args[0], (const char *)info.args[1],
                           info.args[2], info.args[3]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_YES;
    case SYS_read:
      ret = emu_vfs_read(info.args[0], (void *)info.args[1], info.args[2]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_YES;
    case SYS_write:
      ret =
          emu_vfs_write(info.args[0], (const void *)info.args[1], info.args[2]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_YES;
    case SYS_close:
      ret = emu_vfs_close(info.args[0]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_YES;
    case SYS_lseek:
      ret = emu_vfs_lseek(info.args[0], info.args[1], info.args[2]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_NO;
    case SYS_stat:
      ret =
          emu_vfs_stat((const char *)info.args[0], (struct stat *)info.args[1]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_NO;
    case SYS_fstat:
      ret = emu_vfs_fstat(info.args[0], (struct stat *)info.args[1]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_NO;
    case SYS_mmap:
      if (g_vfs_mmap.active)
      {
        // This was a VFS file mmap - tracee did anonymous mmap
        // Get the returned address from tracee
        void *addr = (void *)info.rval;
        if (addr != MAP_FAILED)
        {
          // Get VFS file content and write to tracee memory
          auto &fs = ptmc_state.fs_states[ptmc_state.cursor];
          auto fd_it = fs.open_files.find(g_vfs_mmap.fd);
          if (fd_it != fs.open_files.end())
          {
            auto node_it = fs.filesystem.find(fd_it->second.path);
            if (node_it != fs.filesystem.end())
            {
              VFSNode &node = node_it->second;
              size_t offset = g_vfs_mmap.offset;
              size_t len = g_vfs_mmap.length;

              // Clamp to file size
              if (offset > node.content.size())
                offset = node.content.size();
              if (offset + len > node.content.size())
                len = node.content.size() - offset;

              // Write content to tracee memory
              memcpy_host2guest(addr, node.content.data() + offset, len);
            }
          }
        }
        g_vfs_mmap.active = false;
        return CKPT_YES;
      }
      // Normal mmap - fall through to default
      return CKPT_NO;
    case SYS_munmap:
      ret = emu_munmap((void *)info.args[0], info.args[1]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_NO;
    case SYS_pipe:
    case SYS_pipe2:
    {
      int pipefd[2];
      ret = emu_pipe(pipefd, info.args[1]);
      if (ret == 0)
      {
        memcpy_host2guest((void *)info.args[0], pipefd, sizeof(pipefd));
      }
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_YES;
    }

    case SYS_epoll_create:
      ret = emu_epoll_create(info.args[0]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_YES;
    case SYS_epoll_create1:
      ret = emu_epoll_create1(info.args[0]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_YES;
    case SYS_epoll_ctl:
      ret = emu_epoll_ctl(info.args[0], info.args[1], info.args[2],
                          (struct epoll_event *)info.args[3]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_YES;
    case SYS_epoll_wait_old:
      assert(0);
    case SYS_epoll_wait:
      ret = emu_epoll_wait(info.args[0], (struct epoll_event *)info.args[1],
                           info.args[2], info.args[3]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_YES;
    case SYS_epoll_pwait:
      ret = emu_epoll_pwait(info.args[0], (struct epoll_event *)info.args[1],
                            info.args[2], info.args[3],
                            (const sigset_t *)info.args[4], info.args[5]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_YES;
    case SYS_epoll_pwait2:
      ret = emu_epoll_pwait2(info.args[0], (struct epoll_event *)info.args[1],
                             info.args[2], (const struct timespec *)info.args[3],
                             (const sigset_t *)info.args[4], info.args[5]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_YES;
    case SYS_setsockopt:
      ret = emu_setsockopt(info.args[0], info.args[1], info.args[2],
                           (void *)info.args[3], info.args[4]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_YES;
    case SYS_getsockopt:
      ret = emu_getsockopt(info.args[0], info.args[1], info.args[2],
                           (void *)info.args[3], (socklen_t *)info.args[4]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_YES;
    case SYS_fsync:
      ret = emu_fsync(info.args[0]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_YES;
    case SYS_fdatasync:
      ret = emu_fdatasync(info.args[0]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_YES;
    case SYS_fcntl:
      ret = emu_fcntl(info.args[0], info.args[1], info.args[2]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_YES;

    case SYS_accept:
    case SYS_accept4:
      ret = emu_accept(info.args[0], (struct sockaddr *)info.args[1],
                       (socklen_t *)info.args[2]);
      tracee_set_rax(pid, ret);
      info.rval = ret;
      return CKPT_YES;

    case SYS_clone:
      return CKPT_YES;

    case SYS_gettid:
    {
      // Return virtual TID (thread index within the tracee)
      int tracee_idx = ptmc_state.cursor;
      int thread_idx = ptmc_state.current_thread_idx[tracee_idx];
      int virtual_tid = thread_idx + 1;  // +1 to avoid 0
      tracee_set_rax(pid, virtual_tid);
      info.rval = virtual_tid;
      LOG_DEBUG("gettid: returning virtual tid %d for thread %d in process %d",
                virtual_tid, thread_idx, tracee_idx);
      return CKPT_NO;
    }

    case SYS_futex:
    {
      // Emulate futex operations
      int tracee_idx = ptmc_state.cursor;
      auto& child_state = ptmc_state.running_state.child[tracee_idx];

      // Initialize FutexState if needed
      if (!child_state.futex_state) {
        child_state.futex_state = new FutexState();
      }

      uint64_t uaddr = info.args[0];
      int futex_op = (int)info.args[1];
      int val = (int)info.args[2];
      uint64_t timeout = info.args[3];
      uint64_t uaddr2 = info.args[4];
      int val3 = (int)info.args[5];

      // For FUTEX_WAIT, we need to read the current value at uaddr
      uint64_t stackval = 0;
      if ((futex_op & 0x7F) == FUTEX_WAIT || (futex_op & 0x7F) == FUTEX_WAIT_BITSET) {
        tracee_read_mem(pid, (void*)uaddr, &stackval, sizeof(int));
      }

      // Use virtual TID (thread index + 1) instead of physical PID for futex state
      // This ensures consistency across save/restore cycles
      int thread_idx = ptmc_state.current_thread_idx[tracee_idx];
      int virtual_tid = thread_idx + 1;  // +1 to avoid 0

      int result = child_state.futex_state->handle_futex(
          virtual_tid, uaddr, futex_op, val, timeout, uaddr2, val3,
          (futex_op & 0x7F) == FUTEX_WAIT || (futex_op & 0x7F) == FUTEX_WAIT_BITSET ? &stackval : nullptr);

      if (result < 0) {
        // Error
        tracee_set_rax(pid, (uint64_t)result);
        info.rval = (uint64_t)result;
      } else {
        // Success or wake count
        tracee_set_rax(pid, result);
        info.rval = result;
      }

      // Check if current thread is now waiting on futex
      int op_type = futex_op & 0x7F;
      if (op_type == FUTEX_WAIT || op_type == FUTEX_WAIT_BITSET) {
        if (child_state.futex_state->is_thread_waiting(virtual_tid)) {
          LOG_INFO("Thread %d (vTID %d) is now waiting on futex at %p - DISCARDING this execution", pid, virtual_tid, (void*)uaddr);
          // Mark thread as blocked - scheduler should skip it
          ptmc_state.thread_blocked[tracee_idx][thread_idx] = true;
          // Return CKPT_DISCARD to cancel this execution and roll back state
          return CKPT_DISCARD;
        }
      } else if (op_type == FUTEX_WAKE || op_type == FUTEX_WAKE_BITSET) {
        // Threads were woken up - clear their blocked flags
        // result contains the number of threads woken
        if (result > 0) {
          LOG_DEBUG("FUTEX_WAKE waked %d threads, unblocking them", result);
          // Get the list of waiters that were at this address
          auto waiters = child_state.futex_state->get_waiters(uaddr);
          for (pid_t waiter_vtid : waiters) {
            int waiter_idx = waiter_vtid - 1;  // Convert back to 0-based index
            if (waiter_idx >= 0 && waiter_idx < 64) {
              ptmc_state.thread_blocked[tracee_idx][waiter_idx] = false;
            }
          }
        }
      }

      return CKPT_YES;
    }

    case SYS_nanosleep:
    case SYS_brk:
      return CKPT_NO;

    default:
      return CKPT_YES;
  }
  return CKPT_YES;
}
/* Forward declaration: Handle thread exit for SYS_exit syscall */
bool handle_thread_exit(pid_t pid, pid_t wait_result, int wstatus, syscall_info &si);

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
      case SIGURG:
        LOG_INFO("Received SIGURG from tracee %d, likely due to Go runtime "
                 "preemption signal. Ignoring and continuing.",
                 ptmc_state.cursor);
        ptrace_right(PTRACE_SYSCALL, pid, NULL, NULL);
        waitpid(pid, &wstatus, 0);
        assert(WIFSTOPPED(wstatus) && (WSTOPSIG(wstatus) == PTRACE_TRAP_SIG));
        break;
      
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

  /* Mark process as exited only for exit_group, not for single thread exit */
  if (si.nr == SYS_exit_group)
    ptmc_state.status[ptmc_state.cursor] = dstatus_exit(info.entry.args[0]);

  /* exit */
  ptrace_right(PTRACE_SYSCALL, pid, NULL, NULL);

  /* Special handling for SYS_exit: the thread will actually exit, not stop with SIGTRAP */
  if (si.nr == SYS_exit) {
    pid_t wait_result = waitpid(pid, &wstatus, 0);

    if (handle_thread_exit(pid, wait_result, wstatus, si)) {
      return CKPT_NO;
    }

    if (wait_result == pid && WIFSTOPPED(wstatus) && WSTOPSIG(wstatus) == PTRACE_TRAP_SIG) {
      /* Thread didn't actually exit (maybe exit was prevented) - continue normally */
      ptrace_right(PTRACE_GET_SYSCALL_INFO, pid, (void *)sizeof(info), &info);
      if (info.op == PTRACE_SYSCALL_INFO_EXIT) {
        si.rval = info.exit.rval;
        return on_syscall_exit(pid, si);
      }
    }
    /* Unexpected state - fall through to error handling */
    LOG_ERROR("Unexpected waitpid result for SYS_exit: pid=%d, wstatus=%d", wait_result, wstatus);
    return CKPT_STOP;
  }

  /* Normal syscall exit handling for non-SYS_exit syscalls */
  waitpid(pid, &wstatus, 0);
  assert(WIFSTOPPED(wstatus) && (WSTOPSIG(wstatus) == PTRACE_TRAP_SIG));

  ptrace_right(PTRACE_GET_SYSCALL_INFO, pid, (void *)sizeof(info), &info);
  assert(info.op == PTRACE_SYSCALL_INFO_EXIT);
  si.rval = info.exit.rval;
  return on_syscall_exit(pid, si);
}

/* Handle thread exit for SYS_exit syscall.
 * Returns true if thread exited, false if it should continue normally. */
bool handle_thread_exit(pid_t pid, pid_t wait_result, int wstatus, syscall_info &si)
{
  if (!(wait_result == pid && (WIFEXITED(wstatus) || WIFSIGNALED(wstatus)))) {
    return false;  // Thread did not exit
  }

  /* Thread has exited - mark thread as exited but keep process running */
  int tracee_idx = ptmc_state.cursor;
  auto &child_state = ptmc_state.running_state.child[tracee_idx];

  /* Find the exited thread index */
  int exited_thread_idx = -1;
  for (int i = 0; i < (int)child_state.threads.size(); i++) {
    pid_t thread_tid = ptmc_state.get_thread_tid(tracee_idx, i);
    if (thread_tid == pid) {
      exited_thread_idx = i;
      break;
    }
  }

  if (exited_thread_idx >= 0) {
    /* Mark thread as exited in ptmc_state */
    ptmc_state.thread_exited[tracee_idx][exited_thread_idx] = true;
    /* Also mark in child_state */
    int virtual_tid = exited_thread_idx + 1;
    child_state.remove_thread(virtual_tid);
    LOG_INFO("Thread %d (PID %d) exited, marked as exited", virtual_tid, pid);
  } else {
    LOG_WARN("Exited thread PID %d not found in state", pid);
  }

  /* Return CKPT_NO to continue with other threads */
  si.rval = 0;
  return true;
}

/**
 * @brief Execute one critical syscall step for the current tracee
 *
 * This function implements the multi-threaded scheduling logic:
 * 1. Initialize thread state arrays (blocked/exited) from saved state
 * 2. Handle edge case: process marked exited but has live threads (load scenario)
 * 3. Try each thread in round-robin until one makes progress
 * 4. Handle case where all threads are blocked or exited
 *
 * @param s Current system state (for status checking)
 * @param info Output: syscall information for the step that was executed
 * @return CKPT_YES/CKPT_NO/CKPT_EXIT/CKPT_STOP based on result
 */
int exec_once(const sys_state &s, syscall_info &info)
{
  int index = ptmc_state.cursor;

  // Get number of threads for this tracee
  const auto& threads = ptmc_state.running_state.child[index].threads;
  int num_threads = threads.size();
  if (num_threads == 0) num_threads = 1;  // At least main thread

  /*
   * Step 1: Initialize thread state arrays from saved state
   *
   * thread_blocked[i]: true if thread i is waiting on a futex
   * thread_exited[i]: true if thread i has exited (physical_tid == 0)
   */
  auto futex_state = ptmc_state.running_state.child[index].futex_state;
  if (futex_state) {
    for (int i = 0; i < num_threads; i++) {
      int vtid = i + 1;  // Virtual TID is 1-based
      ptmc_state.thread_blocked[index][i] = futex_state->is_thread_waiting(vtid);
    }
  } else {
    memset(ptmc_state.thread_blocked[index], 0, sizeof(ptmc_state.thread_blocked[index]));
  }

  // Initialize thread_exited from threads vector (physical_tid == 0 means exited)
  for (int i = 0; i < num_threads; i++) {
    if (i < (int)threads.size()) {
      ptmc_state.thread_exited[index][i] = (threads[i].physical_tid == 0);
    } else {
      ptmc_state.thread_exited[index][i] = false;
    }
  }

  /*
   * Step 2: Handle edge case - process was marked exited but we're loading
   * a state with live threads. This can happen when using the 'load' command
   * to go back to a previous state.
   */
  if (DISDEAD(ptmc_state.status[index])) {
    bool has_live_threads = false;
    for (int i = 0; i < num_threads; i++) {
      if (i < (int)threads.size() && threads[i].physical_tid != 0) {
        has_live_threads = true;
        break;
      }
    }
    if (has_live_threads) {
      LOG_INFO("Process %d was marked exited but loaded state has live threads, resetting status", index);
      ptmc_state.status[index] = dstatus_normal();  // Reset to running state
    }
  }

  /*
   * Step 3: Try each thread in round-robin fashion until one makes progress
   *
   * We skip threads that are:
   * - Blocked on a futex (waiting for wakeup)
   * - Already exited
   *
   * If a thread makes progress without needing a checkpoint (CKPT_NO),
   * we continue executing the same thread (round-robin within thread).
   */
  for (int attempt = 0; attempt < num_threads; attempt++) {
    int thread_idx = ptmc_state.current_thread_idx[index];

    // Check if current thread is blocked on futex
    bool thread_blocked = ptmc_state.thread_blocked[index][thread_idx];

    // Check if current thread has exited
    bool thread_exited = ptmc_state.thread_exited[index][thread_idx];

    if (thread_blocked || thread_exited) {
      // Current thread is blocked or exited, try next thread
      int next_idx = (thread_idx + 1) % num_threads;
      ptmc_state.set_current_thread(index, next_idx);
      if (thread_blocked) {
        LOG_DEBUG("Thread %d blocked, switching to thread %d", thread_idx, next_idx);
      } else {
        LOG_DEBUG("Thread %d exited, switching to thread %d", thread_idx, next_idx);
      }
      continue;
    }

    // Get current thread TID
    pid_t pid = ptmc_state.get_current_tid(index);
    if (pid < 0) {
      pid = pids[index];  // Fallback to main thread
    }

    if (DISDEAD(s.status[index]))
      return -1;
    LOG_DEBUG("exec_once from state " HASH_FORMAT ":" HASH_FORMAT " on TID %d (thread %d/%d)",
              s.ss_hash, s.ts_hash[index], pid, thread_idx, num_threads);

    int result = do_one_syscall(pid, info);

    switch (result)
    {
      case CKPT_NO:
        // Thread made progress but no checkpoint needed
        // Continue executing same thread
        attempt--;  // Don't count this as an attempt
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

  /*
   * Step 4: Handle case where no thread could make progress
   *
   * This happens when all threads are either:
   * - Exited (all_exited = true) -> return CKPT_EXIT
   * - Blocked on futex -> possible deadlock, save state anyway
   */

  // Check if all threads have exited
  bool all_exited = true;
  for (int i = 0; i < num_threads; i++) {
    if (!ptmc_state.thread_exited[index][i]) {
      all_exited = false;
      break;
    }
  }

  if (all_exited) {
    LOG_INFO("All %d threads have exited", num_threads);
    ptmc_state.status[index] = dstatus_exit(0);
    return CKPT_EXIT;
  }

  // All threads are blocked on futex - this may be a deadlock
  // Save state anyway so the user can investigate
  LOG_WARN("All %d threads are blocked, possible deadlock", num_threads);
  return CKPT_YES;
}

TransitionResult state_transition(const sys_state &source_state,
                                  int process_index)
{
  TransitionResult result;
  ptmc_state.cursor = process_index;

  // restore state to tracees with timing
  auto t1 = std::chrono::high_resolution_clock::now();
  source_state.recover_running_state();
  auto t2 = std::chrono::high_resolution_clock::now();
  result.restore_time =
      std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1);

  // do syscalls
  syscall_info new_syscalls[NP];
  for (int i = 0; i < NP; i++)
    new_syscalls[i] = source_state.child[i].si;

  int ckpt = exec_once(source_state, new_syscalls[process_index]);

  // save state with timing
  auto t3 = std::chrono::high_resolution_clock::now();
  result.new_state = sys_state(new_syscalls);
  auto t4 = std::chrono::high_resolution_clock::now();
  result.save_time =
      std::chrono::duration_cast<std::chrono::microseconds>(t4 - t3);

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
  /* Handle SIGURG from Go runtime (preemption signal) */
  while (WIFSTOPPED(wstatus) && WSTOPSIG(wstatus) == SIGURG) {
    ptrace_right(PTRACE_SYSCALL, pid, NULL, NULL);
    waitpid(pid, &wstatus, 0);
  }
  assert(WIFSTOPPED(wstatus) && WSTOPSIG(wstatus) == PTRACE_TRAP_SIG);

  ptrace_right(PTRACE_GET_SYSCALL_INFO, pid, (void *)sizeof(info), &info);

  assert(info.op == PTRACE_SYSCALL_INFO_ENTRY);
  ret.nr = info.entry.nr;
  for (int i = 0; i < 6; i++)
    ret.args[i] = info.entry.args[i];

  /* Handle clone: add CLONE_PTRACE flag to auto-trace child threads */
  if (ret.nr == SYS_clone) {
    uint64_t new_flags = ret.args[0] | 0x00002000; /* CLONE_PTRACE */
    tracee_set_rdi(pid, new_flags);
    LOG_INFO("Modified clone flags to add CLONE_PTRACE");
  }

  /* Handle clone3: modify clone_args.flags to add CLONE_PTRACE */
  if (ret.nr == SYS_clone3) {
    uint64_t args_ptr = ret.args[0];
    if (args_ptr != 0) {
      // Read flags from clone_args (first field)
      uint64_t flags = ptrace(PTRACE_PEEKDATA, pid, args_ptr, NULL);
      // Add CLONE_PTRACE flag
      flags |= 0x00002000; /* CLONE_PTRACE */
      ptrace(PTRACE_POKEDATA, pid, args_ptr, flags);
      LOG_INFO("Modified clone3 args at %p, flags=0x%lx", (void*)args_ptr, flags);
    }
  }

  /* exit */
  ptrace_right(PTRACE_SYSCALL, pid, NULL, NULL);
  waitpid(pid, &wstatus, 0);
  /* Handle SIGURG from Go runtime (preemption signal) */
  while (WIFSTOPPED(wstatus) && WSTOPSIG(wstatus) == SIGURG) {
    ptrace_right(PTRACE_SYSCALL, pid, NULL, NULL);
    waitpid(pid, &wstatus, 0);
  }
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
    LOG_INFO("Neutralize getrandom(%p, %d, %d)", (void *)ret.args[0],
             ret.args[1], ret.args[2]);
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

  /* Record new thread creation from clone/clone3 */
  if ((ret.nr == SYS_clone || ret.nr == SYS_clone3) && ret.rval > 0)
  {
    pid_t new_tid = (pid_t)ret.rval;
    LOG_INFO("New thread created: TID %d from parent %d", new_tid, pid);

    // Record thread creation in tracee_state
    int tracee_idx = ptmc_state.cursor;
    if (tracee_idx >= 0 && tracee_idx < NP)
    {
      auto &child_state = ptmc_state.running_state.child[tracee_idx];

      // Get clone flags from arguments
      uint64_t clone_flags = 0;
      uint64_t stack_addr = 0;
      if (ret.nr == SYS_clone)
      {
        clone_flags = ret.args[0];
        stack_addr = ret.args[1];
      }
      else if (ret.nr == SYS_clone3)
      {
        uint64_t args_ptr = ret.args[0];
        if (args_ptr != 0)
        {
          clone_flags = ptrace(PTRACE_PEEKDATA, pid, args_ptr, NULL);
          // stack is at offset 5*8 = 40 bytes in clone_args (after flags, pidfd, child_tid, parent_tid, exit_signal)
          stack_addr = ptrace(PTRACE_PEEKDATA, pid, args_ptr + 40, NULL);
        }
      }

      // Add thread to state
      child_state.add_thread(new_tid, clone_flags, stack_addr, pid, false);

      LOG_INFO("Recorded thread %d in tracee %d state (flags=0x%lx)",
               new_tid, tracee_idx, clone_flags);
    }

    // Wait for the new thread to stop (SIGSTOP from CLONE_PTRACE)
    int wstatus_new;
    pid_t waited = waitpid(new_tid, &wstatus_new, WNOHANG);
    if (waited == 0)
    {
      // Thread hasn't stopped yet, wait briefly
      usleep(1000);
      waited = waitpid(new_tid, &wstatus_new, WNOHANG);
    }
    if (waited == new_tid && WIFSTOPPED(wstatus_new))
    {
      LOG_INFO("New thread %d stopped with signal %d", new_tid, WSTOPSIG(wstatus_new));
    }
    else
    {
      LOG_WARN("New thread %d not in expected stopped state (waited=%d)", new_tid, waited);
    }
  }

  return ret;
}

static void init_tracee_state(int index)
{
  int pid = pids[index];
  int wstatus = 0;
  int stop_nr;
  // Convert to absolute path for Go detection
  char abs_path[PATH_MAX];
  realpath(ptmc_state.tracee[index].executable, abs_path);
  LOG_INFO("Checking if %s (abs: %s) is a Go program", ptmc_state.tracee[index].executable, abs_path);
  int go_check = is_go_program(abs_path);
  LOG_INFO("is_go_program result: %d", go_check);
  // Hardcoded detection for Go programs (workaround for is_go_program issues)
  if (!go_check && strstr(ptmc_state.tracee[index].executable, "test") || strstr(ptmc_state.tracee[index].executable, "detsim") != NULL) {
    LOG_INFO("Hardcoded: treating as Go program due to 'test' in name");
    go_check = 1;
  }
  if (go_check) {
    // Go programs: stop at first mmap after initialization
    LOG_INFO("Detected Go program, using SYS_mmap as stop_nr");
    stop_nr = SYS_mmap;
  } else if (is_dynamically_linked(ptmc_state.tracee[index].executable)) {
    stop_nr = SYS_munmap;
  } else {
    stop_nr = SYS_mprotect;
  }
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