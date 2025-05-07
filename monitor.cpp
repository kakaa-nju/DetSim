#include <cjson/cJSON.h>
#include <cjson/cJSON_Utils.h>
#include "debug.h"
#include "state.h"
#include "monitor.h"
#include <csignal>
#include <dirent.h>
#include <getopt.h>
#include <stdio.h>
#include <string.h>
#include <wait.h>
#include <signal.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <spdlog/spdlog.h>
#include <dlfcn.h>

static char *log_file = NULL;
char *cfg_file = (char *)"config.json"; /* default */
static int auto_mode = 0;
int is_auto_mode() { return auto_mode; }

FILE *log_fp = NULL;

void init_log(const char *log_file) {
  if (log_file == NULL) 
  {
    log_fp = stdout;
    return;
  }
  log_fp = fopen(log_file, "w");
  Assert(log_fp, "Can not open '%s'", log_file);
}

static char cfg_str[4096];
void read_config(const char *cfg_file) {
  assert(cfg_file);
  FILE *cfg_fp = fopen(cfg_file, "r");
  Assert(cfg_fp, "Can not open '%s'", cfg_file);

  fread(cfg_str, 4096, 1, cfg_fp);
  fclose(cfg_fp);
  cJSON *cfg = cJSON_Parse(cfg_str);
  cJSON *Loglevel = cJSON_GetObjectItem(cfg, "Loglevel");
  int loglevel = cJSON_GetNumberValue(Loglevel);
  spdlog::set_level((spdlog::level::level_enum)loglevel);

  cJSON *Nodes = cJSON_GetObjectItem(cfg, "Nodes");
  int nodes = cJSON_GetNumberValue(Nodes);
  Assert(nodes == NP, "Build don't support %d processes, please compile with 'NP=%d' and rebuild", nodes, nodes);
  cJSON *tracee = cJSON_GetObjectItem(cfg, "Tracee");
  
  for (int i = 0; i < nodes; i++) 
  {
    cJSON *argv = cJSON_GetArrayItem(tracee, i);
    int argc = cJSON_GetArraySize(tracee);
    Assert(argc <= 5, "More than 5 arguments is not supported");
    
    ptmc_state.tracee[i].argc = argc;
    for (int j = 0; j < argc; j++) 
    {
      cJSON *arg = cJSON_GetArrayItem(argv, j);
      ptmc_state.tracee[i].argv[j] = cJSON_GetStringValue(arg);
    }
    ptmc_state.tracee[i].executable = ptmc_state.tracee[i].argv[0];
    ptmc_state.tracee[i].argv[argc] = NULL;
  }

  cJSON *addrs = cJSON_GetObjectItem(cfg, "Addr");
  if (addrs)
  {
    int addrs_cnt = cJSON_GetArraySize(addrs);
    assert(addrs_cnt == NP);
    for (int j = 0; j < addrs_cnt; j++) 
    {
      cJSON *addr = cJSON_GetArrayItem(addrs, j);
      ptmc_state.addrs[j] = std::string(cJSON_GetStringValue(addr));
    }
  }

  cJSON *shared_files = cJSON_GetObjectItem(cfg, "SharedFiles");
  if (shared_files) 
  {
    int shared_files_cnt = cJSON_GetArraySize(shared_files);
    for (int j = 0; j < shared_files_cnt; j++) 
    {
      cJSON *shared_file = cJSON_GetArrayItem(shared_files, j);
      ptmc_state.shared_files.emplace(std::string(cJSON_GetStringValue(shared_file)));
    }
  }

  cJSON *assertions = cJSON_GetObjectItem(cfg, "Assertions");
  if (assertions) 
  {
    int assertions_cnt = cJSON_GetArraySize(assertions);
    for (int j = 0; j < assertions_cnt; j++) 
    {
      cJSON *assertion = cJSON_GetArrayItem(assertions, j);
      ptmc_state.assertions.emplace(std::string(cJSON_GetStringValue(assertion)));
    }
  }

  cJSON *user_check = cJSON_GetObjectItem(cfg, "UserCheck");
  if (user_check)
  {
    LOG_INFO("Compiling user check sources");
    int user_check_cnt = cJSON_GetArraySize(user_check);
    for (int j = 0; j < user_check_cnt; j++)
    {
      cJSON *u = cJSON_GetArrayItem(user_check, j);
      char *src = cJSON_GetStringValue(u);
      std::string obj(src, strchr(src, '.'));
      obj = "./" + obj + ".so";

      if (access(obj.c_str(), R_OK | X_OK)) { /* compile */
        int pid = vfork();
        if (pid == 0)
        {
          char arg_d[10];
          sprintf(arg_d, "-DNP=%d", NP);
          execlp("g++", "g++", arg_d, "-fpic", "-shared", src, "-o", obj.c_str(), NULL);
          perror("exec");
        }
        else
          waitpid(pid, 0, 0);
      }
      void *handle = dlopen(obj.c_str(), RTLD_LAZY | RTLD_LOCAL);
      if (!handle) 
      {
        panic("%s", dlerror());
      }
      int (*func)() = (int (*)())dlsym(handle, "check");
      ptmc_state.user_checks.push_back(func);
    }
    LOG_INFO("Done");
  }
  
  free(cfg);
}

static inline void welcome() {
  printf("Build time: %s, %s\n", __TIME__, __DATE__);
  printf("Welcome to \33[1;41m\33[1;33m\33[0m-ptraceMC!\n");
  printf("For help, type \"help\"\n");
}

static inline void parse_args(int argc, char *argv[]) {
  const struct option table[] = {
    {"auto"     , no_argument      , NULL, 'a'},
    {"log"      , required_argument, NULL, 'l'},
    {"help"     , no_argument      , NULL, 'h'},
    {"cfg"      , required_argument, NULL, 'c'},
    {0          , 0                , NULL,  0 },
  };
  int o;
  while ( (o = getopt_long(argc, argv, "-ahl:c:", table, NULL)) != -1) 
  {
    switch (o) 
    {
      case 'a': auto_mode = 1; break;
      case 'l': log_file = optarg; break;
      case 'c': cfg_file = optarg; break;
      default:
        printf("Usage: %s [OPTION...]\n\n", argv[0]);
        printf("\t-a,--auto               run with auto mode\n");
        printf("\t-l,--log=FILE           output log to FILE\n");
        printf("\t-c,--cfg=FILE   specify configuration FILE\n");
        printf("\n");
        exit(0);
    }
  }
}

int sigint_received = 0;
static void ctrl_c(int _) {
  sigint_received = 1;
}

static void handle_sigint() {
  signal(SIGINT, ctrl_c);
}

void init_regex();
void init_monitor(int argc, char *argv[]) {
  /* Perform some global initialization. */

  spdlog::enable_backtrace(0);

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

int exec_store();
int load_exec_store();
int is_auto_mode();

/* We use the `readline' library to provide more flexibility to read from stdin. */
static char* rl_gets() {
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

int exec_cont();
static int cmd_c(char *args) {
  exec_cont();
  return 0;
}


static int cmd_q(char *args) {
  ptmc_state.state = PTMC_QUIT;
  return -1;
}

static int cmd_help(char *args);

static int cmd_sw(char *args);

static int cmd_si(char *args);

static int cmd_load(char *args);

static int cmd_info(char *args);

static struct {
  const char *name;
  const char *description;
  int (*handler) (char *);
} cmd_table [] = {
  { "help", "Display informations about all supported commands", cmd_help },
  { "c", "Continue the execution of the program, start from current state", cmd_c },
  { "q", "Exit ptrace_rightMC", cmd_q },
  { "si", "Step from current state, on focused process", cmd_si },
  { "sw", "Switch control focus on the n-th process", cmd_sw },
  { "load", "Load state of given StateHash", cmd_load },
  { "info", "Display current state history", cmd_info },
};

#define NR_CMD (sizeof(cmd_table) / sizeof(cmd_table[0]))

static int cmd_help(char *args) {
  char *arg = strtok(NULL, " ");
  int i;

  if (arg == NULL) 
  {
    /* no argument given */
    for (i = 0; i < NR_CMD; i ++) 
    {
      printf("%s - %s\n", cmd_table[i].name, cmd_table[i].description);
    }
  }
  else 
  {
    for (i = 0; i < NR_CMD; i ++) 
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

static int cmd_sw(char *args) {
  char *arg = strtok(args, " ");
  if (arg == NULL) 
  {
    printf("No Arguments!\n");
  }
  else 
  {
    int cursor = atoi(args);
    if (cursor < 0 || cursor >= NP)
    {
      printf("cursor = %d, out of range [0, %d). Please set again.\n",
          cursor, NP);
      return 0;
    }
    ptmc_state.cursor = cursor;
  }
  return 0;
}

static int cmd_si(char *args) {
  /* check cursor range */
  int cursor = ptmc_state.cursor;
  if (cursor < 0 || cursor >= NP)
  {
    printf("cursor = %d, out of range [0, %d). Please set again.\n",
        cursor, NP);
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
      // LOG_TRACE("run cmd_si on sys_state %s", ptmc_state.source_state.ss_hash.c_str());
      /* LOADED */
      exec_store();
  }
      
  return 0;
}

void show_syscall_history();
static int cmd_info(char *args){
  char *arg = strtok(args, " ");
  if (arg == NULL) 
  {
    // printf("No Arguments!\n");
    sys_state &s = ptmc_state.dest_state;
    printf("Stops at state " HASH_FORMAT " \n", s.ss_hash);
    for (int i = 0; i < NP; i++) 
    {
      printf("Tracee #%d: " HASH_FORMAT ", exited = %d\n", i, s.ts_hash[i], s.exited[i]);
    }
    s.child[ptmc_state.cursor].show_syscall(&s.child[ptmc_state.cursor].si);

    show_syscall_history();

    for (int i = 0; i < NP; i++) 
    {
      auto l = ptmc_state.udp_buffer_lists[i];
      printf("Tracee %d's udp packages:\n", i);
      for (auto &b: l)
      {
        printf("  for recvsock %d: %ld messages\n", b.first, b.second.size());
      }
    }
  }
  else 
  {
    
    /* TODO: show current state history */
  }
  return 0;
}

#include <vector>
/* Command `load` will only set ptmc_state.ss */
static int cmd_load(char *args) {
  char *arg = strtok(args, " ");
  if (arg == NULL) 
  {
    printf("No Arguments!\n");
  }
  else {
    /* match */
    std::vector<hash_type> s;
    DIR *dir = opendir("sstate");
    assert(dir);
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) 
    {
      if (de->d_name[0] == '.') 
        continue;
      if (strstr(de->d_name, args) == de->d_name) 
      {
        hash_type hash;
        sscanf(de->d_name, "%lx", &hash);
        s.push_back(hash);
      }
    }
    closedir(dir);

    if (s.size() == 1) 
    {
      ptmc_state.sysstate_hash = s[0];
      ptmc_state.state = PTMC_PRELOAD;
    } 
    else if (s.size() == 0)
      printf("sys_state hash starts with %s has 0 candidates. Please specify another\n", args);
    else 
      printf("sys_state hash starts with %s has multiple candidates. Please specify one\n", args);
  }
  return 0;
}


void ui_mainloop() {
  char lastbuf[256], lastcmd[256];
  lastbuf[0] = lastcmd[0] = '\0';
  if (is_auto_mode()) 
  {
    cmd_c(NULL);
    return;
  }

  for (char *str; (str = rl_gets())!= NULL; ) 
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
    for (i = 0; i < NR_CMD; i ++) 
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
