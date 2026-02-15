/*
 * monitor.cpp - Monitor/CLI implementation (simplified)
 * 
 * This file now focuses on CLI command handling.
 * Configuration parsing has moved to config.cpp.
 */

#include "monitor.h"
#include "config.h"
#include "fsstate.h"
#include "debug.h"
#include "state.h"
#include "guest.h"
#include "scheduler.h"
#include <cjson/cJSON.h>
#include <csignal>
#include <dirent.h>
#include <readline/history.h>
#include <readline/readline.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_map>

/* Defined in config.cpp */
extern int auto_mode;

/* Defined in expr.cpp */
long expr(const char *e, bool *success);

/* Defined in state.cpp */
extern int choose_many[450];

/* Forward declarations for command handlers */
static int cmd_c(char *args);
static int cmd_q(char *args);
static int cmd_help(char *args);
static int cmd_sw(char *args);
static int cmd_si(char *args);
static int cmd_load(char *args);
static int cmd_info(char *args);
static int cmd_batch(char *args);
static int cmd_p(char *args);
static int cmd_x(char *args);
static int cmd_bt(char *args);
static int cmd_ls(char *args);
static int cmd_stat(char *args);
static int cmd_hexdump(char *args);

static struct
{
  const char *name;
  const char *description;
  int (*handler)(char *);
} cmd_table[] = {
    {"help", "Display informations about all supported commands", cmd_help},
    {"c", "Continue the execution of the program, start from current state",
     cmd_c},
    {"q", "Exit ptraceMC", cmd_q},
    {"si", "Step from current state, on focused process", cmd_si},
    {"sw", "Switch control focus on the n-th process", cmd_sw},
    {"load", "Load state of given StateHash", cmd_load},
    {"info", "Display current state history", cmd_info},
    {"batch", "Read command list from file", cmd_batch},
    {"p", "Calculate expression", cmd_p},
    {"x", "Display tracee memory by byte", cmd_x},
    {"bt", "Print backtrace", cmd_bt},
    {"ls", "List files in current node's filesystem", cmd_ls},
    {"stat", "Show stat of files matching pattern", cmd_stat},
    {"hexdump", "Hexdump of a file (c: continue, q: quit)", cmd_hexdump},
};

#define NR_CMD (sizeof(cmd_table) / sizeof(cmd_table[0]))

static inline void welcome()
{
  printf("Build time: %s, %s\n", __TIME__, __DATE__);
  printf("Welcome to \33[1;41m\33[1;33m\33[0m-ptraceMC!\n");
  printf("For help, type \"help\"\n");
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

  handle_sigint();

  /* Display welcome message. */
  welcome();
}

/* We use the `readline' library to provide more flexibility to read from stdin.
 */
char *rl_gets()
{
  static char *line_read = NULL;

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

static int cmd_c(char *args)
{
  auto_mode = 1;
  exec_cont();
  auto_mode = 0;
  return 0;
}

static int cmd_q(char *args)
{
  ptmc_state.state = PTMC_QUIT;
  return -1;
}

static int cmd_help(char *args)
{
  char *arg = strtok(NULL, " ");
  int i;

  if (arg == NULL)
  {
    /* no argument given */
    for (i = 0; i < NR_CMD; i++)
    {
      printf("%s - %s\n", cmd_table[i].name, cmd_table[i].description);
    }
  }
  else
  {
    for (i = 0; i < NR_CMD; i++)
    {
      if (strcmp(arg, cmd_table[i].name) == 0)
      {
        printf("%s - %s\n", cmd_table[i].name, cmd_table[i].description);
        return 0;
      }
    }
    printf("Unknown command '%s'\n", arg);
  }
  return 0;
}

static int cmd_si(char *args)
{
  exec_once(NULL);
  return 0;
}

static int cmd_sw(char *args)
{
  if (args == NULL)
  {
    printf("Please provide a process number\n");
    return 0;
  }
  int proc = atoi(args);
  if (proc < 0 || proc >= NP)
  {
    printf("Invalid process number %d (must be 0-%d)\n", proc, NP - 1);
    return 0;
  }
  ptmc_state.cursor = proc;
  printf("Switched to process %d (pid=%d)\n", proc, ptmc_state.pids[proc]);
  return 0;
}

void load(hash_type hash);
static int cmd_load(char *args)
{
  if (args == NULL)
  {
    printf("Usage: load <hash_prefix>\n");
    return 0;
  }
  hash_type hash = strtoul(args, NULL, 16);
  load(hash);
  return 0;
}

static int cmd_info(char *args)
{
  printf("Current process: %d (pid=%d)\n", ptmc_state.cursor,
         ptmc_state.pids[ptmc_state.cursor]);
  show_syscall_history();
  return 0;
}

void ui_mainloop();
static int cmd_batch(char *args)
{
  static int in_call = 0;
  if (in_call)
  {
    printf("cmd_batch is not designed for nested invoke\n");
    return 1;
  }
  in_call = 1;

  if (args == NULL)
  {
    printf("No arguments!");
    return 1;
  }
  if (access(args, R_OK))
  {
    printf("Please provide a filename\n");
    return 1;
  }

  int saved_fd = dup(fileno(stdin));
  freopen(args, "r", stdin);

  ui_mainloop();

  dup2(saved_fd, fileno(stdin));
  close(saved_fd);

  in_call = 0;
  return 0;
}

static int cmd_p(char *args)
{
  if (!args || (args[0] == 0))
  {
    printf("Give an expression\n");
    return 1;
  }
  bool success;
  long val = expr(args, &success);
  if (!success)
    printf("Error at computing\n");
  else
    printf("%ld\n", val);
  return 0;
}

static int cmd_x(char *args)
{
  if (!args)
  {
    printf("Usage: x <addr>\n");
    return 0;
  }
  long addr = strtol(args, NULL, 0);
  printf("Memory at 0x%lx:\n", addr);
  for (int i = 0; i < 4; i++)
  {
    printf("  0x%016lx: ", addr + i * 8);
    for (int j = 0; j < 8; j++)
    {
      printf("%02x ", tracee_read_byte(ptmc_state.pids[ptmc_state.cursor],
                                        (void *)(addr + i * 8 + j)));
    }
    printf("\n");
  }
  return 0;
}

static int cmd_bt(char *args)
{
  tracee_backtrace(ptmc_state.pids[ptmc_state.cursor]);
  return 0;
}

static int cmd_ls(char *args)
{
  int cursor = ptmc_state.cursor;
  auto &fs = ptmc_state.dest_state.child[cursor].fs_state.filesystem;
  printf("Files in process %d filesystem:\n", cursor);
  for (const auto &pair : fs)
  {
    printf("  %s\n", pair.first.c_str());
  }
  return 0;
}

static int cmd_stat(char *args)
{
  if (!args)
  {
    printf("Usage: stat <pattern>\n");
    return 0;
  }
  int cursor = ptmc_state.cursor;
  auto &fs = ptmc_state.dest_state.child[cursor].fs_state.filesystem;
  bool found = false;
  for (const auto &pair : fs)
  {
    if (pair.first.find(args) != std::string::npos)
    {
      found = true;
      const auto &node = pair.second;

      printf("File: %s\n", pair.first.c_str());
      printf("  Size: %-10ld\n", node.metadata.st_size);
      printf("  Access: (%04o)\n", (node.metadata.st_mode & 0777));

#ifdef FSSTATE_DETAILED_METADATA
      const struct stat &st = node.metadata;
      printf("  Blocks: %-10ld IO Block: %-10ld\n", st.st_blocks, st.st_blksize);
      printf("  Device: %-8ld Inode: %-11ld Links: %-10ld\n", st.st_dev,
             st.st_ino, st.st_nlink);
      printf("  Uid: %-10d Gid: %-10d\n", st.st_uid, st.st_gid);

      char buffer[80];
      struct tm *tm_info;

      tm_info = localtime(&st.st_atime);
      strftime(buffer, 80, "%Y-%m-%d %H:%M:%S", tm_info);
      printf("  Access: %s\n", buffer);

      tm_info = localtime(&st.st_mtime);
      strftime(buffer, 80, "%Y-%m-%d %H:%M:%S", tm_info);
      printf("  Modify: %s\n", buffer);

      tm_info = localtime(&st.st_ctime);
      strftime(buffer, 80, "%Y-%m-%d %H:%M:%S", tm_info);
      printf("  Change: %s\n", buffer);
#endif
      printf("----------------------------------------\n");
    }
  }
  if (!found)
  {
    printf("No files found matching pattern: %s\n", args);
  }
  return 0;
}

static int cmd_hexdump(char *args)
{
  if (!args)
  {
    printf("Usage: hexdump <filepath>\n");
    return 0;
  }
  int cursor = ptmc_state.cursor;
  auto &fs = ptmc_state.dest_state.child[cursor].fs_state.filesystem;

  auto it = fs.find(args);
  if (it == fs.end())
  {
    printf("File not found: %s\n", args);
    return 0;
  }

  const std::vector<char> &content = it->second.content;
  size_t len = content.size();
  size_t offset = 0;
  const size_t lines_per_page = 16;

  while (offset < len)
  {
    for (size_t line = 0; line < lines_per_page && offset < len; ++line)
    {
      printf("%08lx  ", offset);

      // Hex bytes
      for (size_t j = 0; j < 16; ++j)
      {
        if (j == 8) printf(" ");
        if (offset + j < len)
        {
          printf("%02x ", (unsigned char)content[offset + j]);
        }
        else
        {
          printf("   ");
        }
      }

      printf(" |");
      // ASCII
      for (size_t j = 0; j < 16; ++j)
      {
        if (offset + j < len)
        {
          char c = content[offset + j];
          if (c >= 32 && c <= 126)
            printf("%c", c);
          else
            printf(".");
        }
      }
      printf("|\n");
      offset += 16;
    }

    if (offset < len)
    {
      printf("--More-- (c: continue, q: quit) ");
      char buf[10];
      if (fgets(buf, sizeof(buf), stdin))
      {
        if (buf[0] == 'q') break;
      }
    }
  }
  return 0;
}

void ui_mainloop()
{
  char lastbuf[256], lastcmd[256];
  lastbuf[0] = lastcmd[0] = '\0';
  if (is_auto_mode())
  {
    cmd_c(NULL);
  }

  for (char *str; (str = rl_gets()) != NULL;)
  {
    if (str[0] != 0)
      strcpy(lastbuf, str);
    char *str_end = str + strlen(str);
    /* extract the first token as the command */
    char *cmd = strtok(str, " ");
    if (cmd == NULL)
    {
      strcpy(str, lastcmd);
      cmd = strtok(str, " ");
      if (cmd == NULL)
      {
        lastbuf[0] = lastcmd[0] = '\0';
        continue;
      }
    }

    /* treat the remaining string as the arguments,
     * which may need further parsing
     */
    char *args = cmd + strlen(cmd) + 1;
    if (args >= str_end)
      args = NULL;

    int i;
    for (i = 0; i < NR_CMD; i++)
    {
      if (strcmp(cmd, cmd_table[i].name) == 0)
      {
        if (cmd_table[i].handler(args) < 0)
          return;
        break;
      }
    }

    if (i == NR_CMD)
      printf("Unknown command '%s'\n", cmd);
    strcpy(lastcmd, lastbuf);
  }
}
