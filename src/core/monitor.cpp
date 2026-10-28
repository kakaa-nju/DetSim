/*
 * monitor.cpp - Monitor/CLI implementation (simplified)
 *
 * This file now focuses on CLI command handling.
 * Configuration parsing has moved to config.cpp.
 */

#include "monitor.h"
#include "ui/cli/commands.h"

/* NCursesUI integration */
#include "ui/log_wrapper.h"
#include "ui/ncurses_ui.h"

#include "config.h"
#include "fsstate.h"
#include "guest.h"
#include "proc_status.h"
#include "scheduler.h"
#include "sockstate.h"
#include "state/state.h"
#include "state/state_store.h"
#include "state/sysstate_store.h"
#include "utils.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cjson/cJSON.h>
#include <csignal>
#include <dirent.h>
#include <fmt/format.h>
#include <iomanip>
#include <map>
#include <readline/history.h>
#include <readline/readline.h>
#include <set>
#include <signal.h>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <vector>

/* Defined in config.cpp */
extern int auto_mode;

/* Defined in expr.cpp */
long expr(const char *e, bool *success);
void expr_print(const char *e);

/* Defined in dwarf.cpp */
void dwarf_print_stack_trace(int pid);
bool dwarf_set_frame(int frame_id);
int dwarf_get_current_frame(void);
void dwarf_print_local_vars(int pid);

/* Defined in state.cpp */
extern int choose_many[450];

static inline void welcome()
{
  detsim::ui::ui_printf("Build time: %s, %s\n", __TIME__, __DATE__);
  detsim::ui::ui_printf("Welcome to ptraceMC!\n");
  detsim::ui::ui_printf("For help, type \"help\"\n");
}

int sigint_received = 0;
static void ctrl_c(int _) { sigint_received = 1; }
static void handle_sigint() { signal(SIGINT, ctrl_c); }

void init_regex();
void init_monitor(int argc, char *argv[])
{
  /* Parse arguments. */
  parse_args(argc, argv);

  /* Open the log file. */
  init_log(log_file);

  init_regex();

  read_config(cfg_file);

  /* Initialize state store */
  StateStore::instance().init();

  handle_sigint();

  /* Display welcome message. */
  welcome();
}

/* We use the `readline' library to provide more flexibility to read from stdin.
 */
static char *line_read = NULL;

char *rl_gets()
{
  if (line_read)
  {
    free(line_read);
    line_read = NULL;
  }

  line_read = readline("(ptmc) ");

  if (line_read && *line_read)
  {
    add_history(line_read);
  }

  return line_read;
}

/* External declaration for NCursesUI */
extern "C" detsim::ui::NCursesUI *get_ncurses_ui();

void refresh_thread_panel()
{
  detsim::ui::NCursesUI *ui = get_ncurses_ui();
  if (!ui)
    return;

  int process = ptmc_state.cursor;
  if (process < 0 || process >= NP)
  {
    process = -1;
    for (int i = 0; i < NP; ++i)
      if (!DISDEAD(ptmc_state.status[i])) { process = i; break; }
  }
  if (process < 0)
  {
    ui->set_status_line(7, "Threads: no live process");
    ui->set_status_line(8, "");
    return;
  }

  const auto &child = ptmc_state.running_state.child[process];
  char line[512];
  snprintf(line, sizeof(line), "Process %d/%d pid=%d current=%d %s",
           process, NP, ptmc_state.pids[process],
           ptmc_state.current_thread_idx[process],
           is_auto_mode() ? "auto" : "manual");
  ui->set_status_line(7, line);

  std::string summary;
  if (child.threads.empty())
  {
    summary = "main runnable (single thread)";
  }
  else
  {
    int shown = 0;
    for (size_t i = 0; i < child.threads.size(); ++i)
    {
      const auto &thread = child.threads[i];
      const char *state = !thread.alive ? "dead" :
          !thread.pending_syscall ? "run" :
          thread.pending_syscall->phase == PendingPhase::Waiting ? "wait" :
          thread.pending_syscall->phase == PendingPhase::Ready ? "ready" : "retry";
      if (shown++) summary += " | ";
      summary += "T" + std::to_string(i) + ":" + state;
      if (thread.alive && thread.pending_syscall)
      {
        const auto &pending = *thread.pending_syscall;
        summary += pending.reason == PendingReason::Futex ? ":futex" :
                   pending.reason == PendingReason::Io ? ":io" : ":timer";
        if (pending.deadline)
          summary += "@" + std::to_string(pending.deadline->sec);
      }
      if (shown == 4 && child.threads.size() > 4)
      {
        summary += " | ...";
        break;
      }
    }
  }
  ui->set_status_line(8, summary);
}

/* External batch file from config */
extern char *batch_file;

void ui_mainloop()
{
  detsim::ui::ui_printf("[DEBUG] ui_mainloop started, auto_mode=%d\n",
                        is_auto_mode());

  /* Handle batch mode first */
  if (batch_file != NULL)
  {
    char *current_batch = batch_file;
    batch_file = NULL; // Clear to prevent recursive batch processing
    detsim::ui::ui_printf("[DEBUG] Running batch mode from file: %s\n",
                          current_batch);
    cmd_batch(current_batch);
    return;
  }

  detsim::ui::NCursesUI *ui = get_ncurses_ui();

  /* Handle auto mode, default to bfs */
  if (is_auto_mode())
  {
    detsim::ui::ui_printf("[DEBUG] Running cmd_bfs\n");
    cmd_bfs(NULL);
    detsim::ui::ui_printf("[DEBUG] cmd_bfs returned\n");
    return;
  }

  /* Manual UI: start with process 0 selected so thread/si work immediately. */
  if (ptmc_state.cursor < 0 && NP > 0)
    ptmc_state.cursor = 0;
  refresh_thread_panel();

  /* If UI is available, use it */
  if (ui)
  {
    /* Set up command callback */
    ui->set_on_command(
        [](const std::string &cmd_line)
        {
          // Create modifiable copy for strtok
          char *cmd_copy = strdup(cmd_line.c_str());
          if (cmd_copy) {
            execute_command_line(cmd_copy);
            free(cmd_copy);
          }
        });

    /* Set up selection callback */
    ui->set_on_selection([](const std::string &text) {});

    /* Run NCursesUI main loop */
    ui->run();
    return;
  }

  /* Fallback to readline mode */
  char lastcmd[256] = {0};

  for (char *str; (str = rl_gets()) != NULL;)
  {
    char line[256];
    if (str[0] != 0)
    {
      strncpy(line, str, sizeof(line) - 1);
      line[sizeof(line) - 1] = '\0';
      strncpy(lastcmd, line, sizeof(lastcmd) - 1);
      lastcmd[sizeof(lastcmd) - 1] = '\0';
    }
    else
    {
      strncpy(line, lastcmd, sizeof(line) - 1);
      line[sizeof(line) - 1] = '\0';
    }
    if (line[0] == '\0')
      continue;
    if (execute_command_line(line) < 0)
      return;
  }
}

/* ======================================================================
 * PTMC_STATE Multi-threading Methods
 * ====================================================================== */

pid_t PTMC_STATE::get_current_tid(int tracee_idx) const
{
  if (tracee_idx < 0 || tracee_idx >= NP) return -1;

  const auto& threads = running_state.child[tracee_idx].threads;
  int tidx = current_thread_idx[tracee_idx];

  if (tidx >= 0 && tidx < (int)threads.size()) {
    // Use the stored physical_tid from thread_state
    pid_t physical_tid = threads[tidx].physical_tid;
    if (threads[tidx].alive && physical_tid > 0) {
      return physical_tid;
    }
  }

  // Bootstrap has no logical slots yet; loaded/unbound slots must never fall back.
  return threads.empty() ? pids[tracee_idx] : -1;
}

pid_t PTMC_STATE::get_thread_tid(int tracee_idx, int thread_idx) const
{
  if (tracee_idx < 0 || tracee_idx >= NP) return -1;

  const auto& threads = running_state.child[tracee_idx].threads;
  if (thread_idx < 0 || thread_idx >= (int)threads.size()) {
    return -1;
  }

  // Use the stored physical_tid from thread_state
  pid_t physical_tid = threads[thread_idx].physical_tid;
  if (threads[thread_idx].alive && physical_tid > 0) {
    return physical_tid;
  }

  return -1;
}

void PTMC_STATE::set_current_thread(int tracee_idx, int thread_idx)
{
  if (tracee_idx < 0 || tracee_idx >= NP) return;

  const auto& threads = running_state.child[tracee_idx].threads;
  if (thread_idx >= 0 && thread_idx < (int)threads.size()) {
    current_thread_idx[tracee_idx] = thread_idx;
  }
}
