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
#include "utils.h"
#include <cjson/cJSON.h>
#include <csignal>
#include <dirent.h>
#include <readline/history.h>
#include <readline/readline.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <set>

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
static int cmd_frame(char *args);
static int cmd_locals(char *args);
static int cmd_ls(char *args);
static int cmd_stat(char *args);
static int cmd_hexdump(char *args);
static int cmd_diff(char *args);

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
    {"bt", "Print backtrace with arguments", cmd_bt},
    {"frame", "Select stack frame", cmd_frame},
    {"locals", "Show local variables in current frame", cmd_locals},
    {"ls", "List files in current node's filesystem", cmd_ls},
    {"stat", "Show stat of files matching pattern", cmd_stat},
    {"hexdump", "Hexdump of a file (c: continue, q: quit)", cmd_hexdump},
    {"diff", "Compare two system states by hash prefix", cmd_diff},
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
  /* check cursor range */
  int cursor = ptmc_state.cursor;
  if (cursor < 0 || cursor >= NP)
  {
    printf("cursor = %d, out of range [0, %d). Please set again.\n", cursor,
           NP);
    return 0;
  }

  switch (ptmc_state.state)
  {
    case PTMC_LOADED:
      exec_store();
      break;
    case PTMC_PRELOAD:
      load_exec_store();
      break;
    default:
      /* copy last state */
      ptmc_state.source_state = ptmc_state.dest_state;
      ptmc_state.sysstate_hash = ptmc_state.dest_state.ss_hash;
      ptmc_state.state = PTMC_LOADED;
      /* LOADED */
      exec_store();
  }
  ptmc_state.dest_state.child[cursor].show_syscall(
      &ptmc_state.dest_state.child[cursor].si);

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
  char *arg = strtok(args, " ");
  if (arg == NULL)
  {
    printf("No Arguments!\n");
    return 1;
  }
  /* match */
  std::vector<hash_type> s;
  DIR *dir = opendir("sstate");
  assert(dir);
  struct dirent *de;
  while ((de = readdir(dir)) != NULL)
  {
    if (de->d_name[0] == '.')
      continue;
    if (strstr(de->d_name, arg) == de->d_name)
    {
      hash_type hash;
      sscanf(de->d_name, "%x", &hash);
      s.push_back(hash);
    }
  }
  closedir(dir);

  if (s.size() == 1)
  {
    ptmc_state.sysstate_hash = s[0];
    ptmc_state.state = PTMC_PRELOAD;
    load(s[0]);
  }
  else if (s.size() == 0)
    printf("sys_state hash starts with %s has 0 candidates. Please specify "
           "another\n",
           arg);
  else
    printf("sys_state hash starts with %s has multiple candidates. Please "
           "specify one\n",
           arg);
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
  expr_print(args);
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
  int pid = ptmc_state.pids[ptmc_state.cursor];
  dwarf_print_stack_trace(pid);
  return 0;
}

static int cmd_frame(char *args)
{
  if (!args || args[0] == '\0') {
    /* Show current frame */
    int frame = dwarf_get_current_frame();
    printf("Current frame: #%d\n", frame);
    return 0;
  }
  
  int frame_id = atoi(args);
  if (dwarf_set_frame(frame_id)) {
    printf("Switched to frame #%d\n", frame_id);
  } else {
    printf("Invalid frame ID: %d\n", frame_id);
  }
  return 0;
}

static int cmd_locals(char *args)
{
  int pid = ptmc_state.pids[ptmc_state.cursor];
  dwarf_print_local_vars(pid);
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

/* ======================================================================
 * cmd_diff - Compare two system states
 * ====================================================================== */

/* Helper: Find states matching a prefix */
static std::vector<hash_type> find_states_by_prefix(const char *prefix)
{
  std::vector<hash_type> matches;
  DIR *dir = opendir("sstate");
  if (!dir) return matches;
  
  struct dirent *de;
  while ((de = readdir(dir)) != NULL)
  {
    if (de->d_name[0] == '.') continue;
    if (strstr(de->d_name, prefix) == de->d_name)
    {
      hash_type hash;
      sscanf(de->d_name, "%x", &hash);
      matches.push_back(hash);
    }
  }
  closedir(dir);
  return matches;
}

/* Helper: Print formatted hash */
static std::string format_hash(hash_type h)
{
  std::stringstream ss;
  ss << std::hex << std::setw(8) << std::setfill('0') << h;
  return ss.str();
}

/* Helper: Compare syscall_info */
static void diff_syscall_info(const syscall_info &a, const syscall_info &b, int proc_id, bool &has_diff)
{
  if (a.nr != b.nr || a.rval != b.rval ||
      memcmp(a.args, b.args, sizeof(a.args)) != 0)
  {
    printf("  [Process %d - Syscall Info]\n", proc_id);
    if (a.nr != b.nr)
      printf("    nr:   %d != %d\n", a.nr, b.nr);
    if (a.rval != b.rval)
      printf("    rval: %ld != %ld\n", (long)a.rval, (long)b.rval);
    for (int i = 0; i < 6; i++)
    {
      if (a.args[i] != b.args[i])
        printf("    args[%d]: 0x%lx != 0x%lx\n", i, (long)a.args[i], (long)b.args[i]);
    }
    has_diff = true;
  }
}

/* Helper: Compare tracee_state fields (excluding memory/regs) */
static void diff_tracee_state(const tracee_state &a, const tracee_state &b, int proc_id, 
                               bool brief, bool memory_only, int max_diff, bool &has_diff)
{
  if (memory_only) return;
  
  bool proc_header = false;
  auto print_header = [&]() {
    if (!proc_header)
    {
      printf("  [Process %d]\n", proc_id);
      proc_header = true;
    }
  };
  
  /* Compare basic fields */
  if (a.ts_hash != b.ts_hash)
  {
    print_header();
    printf("    ts_hash: %s != %s\n", format_hash(a.ts_hash).c_str(), format_hash(b.ts_hash).c_str());
    has_diff = true;
  }
  
  if (a.pid != b.pid)
  {
    print_header();
    printf("    pid: %d != %d\n", a.pid, b.pid);
    has_diff = true;
  }
  
  if (a.brk != b.brk)
  {
    print_header();
    printf("    brk: 0x%lx != 0x%lx\n", (long)a.brk, (long)b.brk);
    has_diff = true;
  }
  
  if (a.tv.tv_sec != b.tv.tv_sec || a.tv.tv_usec != b.tv.tv_usec)
  {
    print_header();
    printf("    tv: %ld.%06ld != %ld.%06ld\n", 
           (long)a.tv.tv_sec, (long)a.tv.tv_usec,
           (long)b.tv.tv_sec, (long)b.tv.tv_usec);
    has_diff = true;
  }
  
  /* Compare syscall_info */
  bool syscall_diff = false;
  diff_syscall_info(a.si, b.si, proc_id, syscall_diff);
  if (syscall_diff) has_diff = true;
}

/* Helper: Read registers from memory dump */
static bool read_regs_from_dump(hash_type ts_hash, struct user_regs_struct *regs)
{
  auto mem_fp = fileutils::open_mem_file(ts_hash);
  if (!mem_fp) return false;
  
  auto map_fp = fileutils::open_map_file(ts_hash);
  if (!map_fp) return false;
  
  std::vector<maps_item> maps;
  get_maps_item(maps, map_fp.get());
  
  /* Skip all memory regions to get to registers at the end */
  for (auto &item : maps)
  {
    if (item.flags[1] == 'w' && item.start != available_memory)
    {
      fseek(mem_fp.get(), item.end - item.start, SEEK_CUR);
    }
  }
  
  /* Read registers */
  size_t n = fread(regs, sizeof(*regs), 1, mem_fp.get());
  return n == 1;
}

/* Helper: Compare registers */
static void diff_registers(hash_type ts_a, hash_type ts_b, int proc_id, bool &has_diff)
{
  struct user_regs_struct regs_a, regs_b;
  
  if (!read_regs_from_dump(ts_a, &regs_a) || !read_regs_from_dump(ts_b, &regs_b))
  {
    printf("  [Process %d - Registers] Failed to read registers\n", proc_id);
    return;
  }
  
  /* Define register fields to compare */
  struct {
    const char *name;
    unsigned long a, b;
  } regs[] = {
    {"r15", regs_a.r15, regs_b.r15},
    {"r14", regs_a.r14, regs_b.r14},
    {"r13", regs_a.r13, regs_b.r13},
    {"r12", regs_a.r12, regs_b.r12},
    {"rbp", regs_a.rbp, regs_b.rbp},
    {"rbx", regs_a.rbx, regs_b.rbx},
    {"r11", regs_a.r11, regs_b.r11},
    {"r10", regs_a.r10, regs_b.r10},
    {"r9", regs_a.r9, regs_b.r9},
    {"r8", regs_a.r8, regs_b.r8},
    {"rax", regs_a.rax, regs_b.rax},
    {"rcx", regs_a.rcx, regs_b.rcx},
    {"rdx", regs_a.rdx, regs_b.rdx},
    {"rsi", regs_a.rsi, regs_b.rsi},
    {"rdi", regs_a.rdi, regs_b.rdi},
    {"orig_rax", regs_a.orig_rax, regs_b.orig_rax},
    {"rip", regs_a.rip, regs_b.rip},
    {"cs", regs_a.cs, regs_b.cs},
    {"eflags", regs_a.eflags, regs_b.eflags},
    {"rsp", regs_a.rsp, regs_b.rsp},
    {"ss", regs_a.ss, regs_b.ss},
    {"fs_base", regs_a.fs_base, regs_b.fs_base},
    {"gs_base", regs_a.gs_base, regs_b.gs_base},
    {"ds", regs_a.ds, regs_b.ds},
    {"es", regs_a.es, regs_b.es},
    {"fs", regs_a.fs, regs_b.fs},
    {"gs", regs_a.gs, regs_b.gs},
  };
  
  bool first_diff = true;
  for (size_t i = 0; i < sizeof(regs)/sizeof(regs[0]); i++)
  {
    if (regs[i].a != regs[i].b)
    {
      if (first_diff)
      {
        printf("  [Process %d - Registers]\n", proc_id);
        printf("    %-12s %-20s %-20s\n", "Register", "State A", "State B");
        printf("    %-12s %-20s %-20s\n", "--------", "-------", "-------");
        first_diff = false;
        has_diff = true;
      }
      printf("    %-12s 0x%016lx   0x%016lx\n", regs[i].name, regs[i].a, regs[i].b);
    }
  }
}

/* Helper: Hexdump a line */
static void hexdump_line(const unsigned char *data, size_t len, size_t offset, 
                         std::stringstream &hex, std::stringstream &ascii)
{
  hex << std::hex << std::setw(8) << std::setfill('0') << offset << "  ";
  for (size_t i = 0; i < 16; i++)
  {
    if (i == 8) hex << " ";
    if (i < len)
    {
      hex << std::setw(2) << std::setfill('0') << (int)data[i] << " ";
      ascii << (isprint(data[i]) ? (char)data[i] : '.');
    }
    else
    {
      hex << "   ";
      ascii << " ";
    }
  }
}

/* Helper: Compare memory content */
static void diff_memory_content(hash_type ts_a, hash_type ts_b, 
                                 const maps_item &map_a, const maps_item &map_b,
                                 int proc_id, int max_diff, bool &has_diff)
{
  auto mem_a = fileutils::open_mem_file(ts_a);
  auto mem_b = fileutils::open_mem_file(ts_b);
  if (!mem_a || !mem_b) return;
  
  /* Seek to this region in both files */
  auto map_a_fp = fileutils::open_map_file(ts_a);
  auto map_b_fp = fileutils::open_map_file(ts_b);
  if (!map_a_fp || !map_b_fp) return;
  
  std::vector<maps_item> maps_a, maps_b;
  get_maps_item(maps_a, map_a_fp.get());
  get_maps_item(maps_b, map_b_fp.get());
  
  /* Calculate offset in memory file */
  size_t offset_a = 0, offset_b = 0;
  for (auto &m : maps_a)
  {
    if (m.start == map_a.start) break;
    if (m.flags[1] == 'w' && m.start != available_memory)
      offset_a += m.end - m.start;
  }
  for (auto &m : maps_b)
  {
    if (m.start == map_b.start) break;
    if (m.flags[1] == 'w' && m.start != available_memory)
      offset_b += m.end - m.start;
  }
  
  fseek(mem_a.get(), offset_a, SEEK_SET);
  fseek(mem_b.get(), offset_b, SEEK_SET);
  
  /* Compare content */
  size_t size = std::min(map_a.end - map_a.start, map_b.end - map_b.start);
  size_t chunk_size = 4096;
  std::vector<unsigned char> buf_a(chunk_size), buf_b(chunk_size);
  
  int diff_count = 0;
  bool header_printed = false;
  
  for (size_t pos = 0; pos < size && diff_count < max_diff; pos += chunk_size)
  {
    size_t to_read = std::min(chunk_size, size - pos);
    size_t n_a = fread(buf_a.data(), 1, to_read, mem_a.get());
    size_t n_b = fread(buf_b.data(), 1, to_read, mem_b.get());
    
    if (n_a != n_b) break;
    
    for (size_t i = 0; i < n_a && diff_count < max_diff; i += 16)
    {
      size_t line_len = std::min((size_t)16, n_a - i);
      if (memcmp(buf_a.data() + i, buf_b.data() + i, line_len) != 0)
      {
        if (!header_printed)
        {
          printf("  [Process %d - Memory: %s 0x%lx-0x%lx]\n", 
                 proc_id, map_a.name, map_a.start, map_a.end);
          header_printed = true;
          has_diff = true;
        }
        
        uint64_t addr = map_a.start + pos + i;
        std::stringstream hex_a, ascii_a, hex_b, ascii_b;
        hexdump_line(buf_a.data() + i, line_len, addr, hex_a, ascii_a);
        hexdump_line(buf_b.data() + i, line_len, addr, hex_b, ascii_b);
        
        printf("    0x%016lx:\n", addr);
        printf("      A: %s |%s|\n", hex_a.str().c_str(), ascii_a.str().c_str());
        printf("      B: %s |%s|\n", hex_b.str().c_str(), ascii_b.str().c_str());
        
        diff_count++;
      }
    }
  }
  
  if (diff_count >= max_diff && header_printed)
  {
    printf("    ... (showing first %d differences, use --max-diff=N to show more)\n", max_diff);
  }
}

/* Helper: Compare memory mappings */
static void diff_memory(hash_type ts_a, hash_type ts_b, int proc_id, 
                        int max_diff, bool &has_diff)
{
  auto map_a = fileutils::open_map_file(ts_a);
  auto map_b = fileutils::open_map_file(ts_b);
  if (!map_a || !map_b) return;
  
  std::vector<maps_item> maps_a, maps_b;
  get_maps_item(maps_a, map_a.get());
  get_maps_item(maps_b, map_b.get());
  
  /* Find mapping differences */
  std::set<uint64_t> starts_a, starts_b;
  std::map<uint64_t, maps_item> map_a_by_start, map_b_by_start;
  
  for (auto &m : maps_a) 
  {
    if (m.flags[1] == 'w' && m.start != available_memory)
    {
      starts_a.insert(m.start);
      map_a_by_start[m.start] = m;
    }
  }
  for (auto &m : maps_b) 
  {
    if (m.flags[1] == 'w' && m.start != available_memory)
    {
      starts_b.insert(m.start);
      map_b_by_start[m.start] = m;
    }
  }
  
  bool header_printed = false;
  auto print_header = [&]() {
    if (!header_printed)
    {
      printf("  [Process %d - Memory Mappings]\n", proc_id);
      header_printed = true;
      has_diff = true;
    }
  };
  
  /* Check for removed mappings */
  for (auto start : starts_a)
  {
    if (starts_b.find(start) == starts_b.end())
    {
      print_header();
      printf("    [Removed] 0x%lx-0x%lx %s\n", 
             map_a_by_start[start].start, map_a_by_start[start].end,
             map_a_by_start[start].name);
    }
  }
  
  /* Check for added mappings */
  for (auto start : starts_b)
  {
    if (starts_a.find(start) == starts_a.end())
    {
      print_header();
      printf("    [Added] 0x%lx-0x%lx %s\n", 
             map_b_by_start[start].start, map_b_by_start[start].end,
             map_b_by_start[start].name);
    }
  }
  
  /* Compare content of common mappings */
  for (auto start : starts_a)
  {
    if (starts_b.find(start) != starts_b.end())
    {
      diff_memory_content(ts_a, ts_b, map_a_by_start[start], map_b_by_start[start],
                          proc_id, max_diff, has_diff);
    }
  }
}

/* Helper: Compare FileSystemState */
static void diff_fs_state(const FileSystemState &a, const FileSystemState &b, 
                          int proc_id, bool &has_diff)
{
  bool header_printed = false;
  auto print_header = [&]() {
    if (!header_printed)
    {
      printf("  [Process %d - FileSystemState]\n", proc_id);
      header_printed = true;
    }
  };
  
  /* Find files only in A */
  for (const auto &pair : a.filesystem)
  {
    auto it = b.filesystem.find(pair.first);
    if (it == b.filesystem.end())
    {
      print_header();
      printf("    [Removed] %s\n", pair.first.c_str());
      has_diff = true;
    }
    else
    {
      /* Compare file content/metadata */
      const auto &node_a = pair.second;
      const auto &node_b = it->second;
      
      if (node_a.metadata.st_size != node_b.metadata.st_size ||
          node_a.metadata.st_mode != node_b.metadata.st_mode ||
          node_a.content != node_b.content)
      {
        print_header();
        printf("    [Modified] %s\n", pair.first.c_str());
        if (node_a.metadata.st_size != node_b.metadata.st_size)
          printf("      size: %ld != %ld\n", (long)node_a.metadata.st_size, (long)node_b.metadata.st_size);
        if (node_a.metadata.st_mode != node_b.metadata.st_mode)
          printf("      mode: 0%o != 0%o\n", (unsigned)node_a.metadata.st_mode, (unsigned)node_b.metadata.st_mode);
        if (node_a.content.size() != node_b.content.size())
          printf("      content size: %zu != %zu\n", node_a.content.size(), node_b.content.size());
        has_diff = true;
      }
    }
  }
  
  /* Find files only in B */
  for (const auto &pair : b.filesystem)
  {
    if (a.filesystem.find(pair.first) == a.filesystem.end())
    {
      print_header();
      printf("    [Added] %s\n", pair.first.c_str());
      has_diff = true;
    }
  }
}

/* Helper: Compare socket lists */
static void diff_sock_list(const std::list<ptmc_sock> &a, const std::list<ptmc_sock> &b,
                           int proc_id, bool &has_diff)
{
  if (a.size() != b.size())
  {
    printf("  [Process %d - Socket State]\n", proc_id);
    printf("    socket count: %zu != %zu\n", a.size(), b.size());
    has_diff = true;
  }
}

/* Main cmd_diff implementation */
static int cmd_diff(char *args)
{
  try
  {
  if (!args || !*args)
  {
    printf("Usage: diff [options] <hash_prefix1> <hash_prefix2>\n");
    printf("Options:\n");
    printf("  --brief, -b       Show only overview, skip detailed differences\n");
    printf("  --memory-only, -m Compare only memory content\n");
    printf("  --max-diff=N      Maximum differences to show per section (default: 10)\n");
    return 0;
  }
  
  /* Parse options */
  bool brief = false;
  bool memory_only = false;
  int max_diff = 10;
  
  std::vector<char*> hash_args;
  char *tok = strtok(args, " ");
  while (tok)
  {
    if (strcmp(tok, "--brief") == 0 || strcmp(tok, "-b") == 0)
      brief = true;
    else if (strcmp(tok, "--memory-only") == 0 || strcmp(tok, "-m") == 0)
      memory_only = true;
    else if (strncmp(tok, "--max-diff=", 11) == 0)
      max_diff = atoi(tok + 11);
    else
      hash_args.push_back(tok);
    tok = strtok(NULL, " ");
  }
  
  if (hash_args.size() < 2)
  {
    printf("Error: Two hash prefixes required\n");
    return 0;
  }
  
  /* Find matching states */
  auto matches_a = find_states_by_prefix(hash_args[0]);
  auto matches_b = find_states_by_prefix(hash_args[1]);
  
  if (matches_a.empty())
  {
    printf("No state found with prefix '%s'\n", hash_args[0]);
    return 0;
  }
  if (matches_a.size() > 1)
  {
    printf("Multiple states match prefix '%s':\n", hash_args[0]);
    for (auto h : matches_a)
      printf("  %s\n", format_hash(h).c_str());
    printf("Please specify the full hash\n");
    return 0;
  }
  
  if (matches_b.empty())
  {
    printf("No state found with prefix '%s'\n", hash_args[1]);
    return 0;
  }
  if (matches_b.size() > 1)
  {
    printf("Multiple states match prefix '%s':\n", hash_args[1]);
    for (auto h : matches_b)
      printf("  %s\n", format_hash(h).c_str());
    printf("Please specify the full hash\n");
    return 0;
  }
  
  hash_type hash_a = matches_a[0];
  hash_type hash_b = matches_b[0];
  
  /* Load states */
  sys_state state_a(hash_a);
  sys_state state_b(hash_b);
  
  /* Print header */
  printf("\n=== State Diff: %s vs %s ===\n\n", 
         format_hash(hash_a).c_str(), format_hash(hash_b).c_str());
  
  /* Overview */
  printf("[Overview]\n");
  printf("  System state hash: %s != %s %s\n",
         format_hash(state_a.ss_hash).c_str(),
         format_hash(state_b.ss_hash).c_str(),
         state_a.ss_hash == state_b.ss_hash ? "✓" : "✗");
  printf("  Process count: %d\n", NP);
  
  bool any_diff = false;
  for (int i = 0; i < NP; i++)
  {
    const char *status_a = state_a.exited[i] ? "exited" : "running";
    const char *status_b = state_b.exited[i] ? "exited" : "running";
    bool ts_same = state_a.ts_hash[i] == state_b.ts_hash[i];
    bool exited_same = state_a.exited[i] == state_b.exited[i];
    
    printf("  Process %d: ts_hash %s %s, exited=%s/%s %s\n",
           i,
           format_hash(state_a.ts_hash[i]).c_str(),
           format_hash(state_b.ts_hash[i]).c_str(),
           ts_same ? "✓" : "✗",
           status_a, status_b,
           exited_same ? "✓" : "✗");
    
    if (!ts_same || !exited_same) any_diff = true;
  }
  printf("\n");
  
  if (brief) return 0;
  
  /* Detailed comparison per process */
  for (int i = 0; i < NP; i++)
  {
    bool proc_has_diff = false;
    
    /* Skip exited processes */
    if (state_a.exited[i] && state_b.exited[i]) continue;
    
    /* Compare tracee_state fields */
    diff_tracee_state(state_a.child[i], state_b.child[i], i, 
                      brief, memory_only, max_diff, proc_has_diff);
    
    /* Compare registers */
    if (!memory_only)
    {
      diff_registers(state_a.ts_hash[i], state_b.ts_hash[i], i, proc_has_diff);
    }
    
    /* Compare memory */
    diff_memory(state_a.ts_hash[i], state_b.ts_hash[i], i, max_diff, proc_has_diff);
    
    /* Compare filesystem */
    if (!memory_only)
    {
      diff_fs_state(state_a.child[i].fs_state, state_b.child[i].fs_state, i, proc_has_diff);
    }
    
    /* Compare sockets */
    if (!memory_only)
    {
      diff_sock_list(state_a.child[i].sock_list, state_b.child[i].sock_list, i, proc_has_diff);
    }
    
    if (proc_has_diff) any_diff = true;
  }
  
  if (!any_diff)
  {
    printf("No differences found between the two states.\n");
  }
  
  printf("\n");
  }
  catch (const std::exception& e)
  {
    printf("Error comparing states: %s\n", e.what());
  }
  return 0;
}

void ui_mainloop()
{
  printf("[DEBUG] ui_mainloop started, auto_mode=%d\n", is_auto_mode());
  char lastbuf[256], lastcmd[256];
  lastbuf[0] = lastcmd[0] = '\0';
  if (is_auto_mode())
  {
    printf("[DEBUG] Running cmd_c\n");
    cmd_c(NULL);
    printf("[DEBUG] cmd_c returned\n");
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
