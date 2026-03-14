/*
 * config.cpp - Configuration parsing implementation
 */

#include "config.h"
#include "common.h"
#include "debug.h"
#include "monitor.h"
#include "fsstate.h"
#include "guest.h"
#include "emu.h"
#include "state_store.h"
#include <cjson/cJSON.h>
#include <cjson/cJSON_Utils.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <wait.h>
#include <assert.h>
#include <getopt.h>
#include <vector>
#include <libgen.h>
#include <limits.h>

char *cfg_file = (char *)"config.json";
char *log_file = NULL;
int loglevel = 0;
FILE *log_fp = NULL;

bool should_log(int level)
{
  if (level >= loglevel)
    return true;
  return false;
}

void init_log(const char *log_file)
{
  if (log_file == NULL)
  {
    log_fp = stdout;
    return;
  }
  log_fp = fopen(log_file, "w");
  Assert(log_fp, "Can not open '%s'", log_file);
}

static char cfg_str[4096];

/* Get absolute path relative to config file directory */
static std::string resolve_path(const char *path, const char *cfg_dir)
{
  if (!path || path[0] == '/') {
    // Absolute path or empty
    return std::string(path ? path : "");
  }
  
  // Relative path: prepend config file directory
  if (cfg_dir && strlen(cfg_dir) > 0) {
    return std::string(cfg_dir) + "/" + path;
  }
  return std::string(path);
}

void read_config(const char *cfg_file)
{
  assert(cfg_file);
  
  /* Get the directory of the config file */
  char cfg_file_copy[PATH_MAX];
  strncpy(cfg_file_copy, cfg_file, PATH_MAX - 1);
  cfg_file_copy[PATH_MAX - 1] = '\0';
  char *cfg_dir = dirname(cfg_file_copy);
  
  FILE *cfg_fp = fopen(cfg_file, "r");
  Assert(cfg_fp, "Can not open '%s'", cfg_file);

  size_t nread = fread(cfg_str, 1, sizeof(cfg_str) - 1, cfg_fp);
  cfg_str[nread] = '\0';  /* Ensure null termination */
  fclose(cfg_fp);
  cJSON *cfg = cJSON_Parse(cfg_str);
  cJSON *Loglevel = cJSON_GetObjectItem(cfg, "Loglevel");
  loglevel = cJSON_GetNumberValue(Loglevel);

  cJSON *Nodes = cJSON_GetObjectItem(cfg, "Nodes");
  int nodes = cJSON_GetNumberValue(Nodes);
  Assert(nodes == NP,
         "Build don't support %d processes, please compile with 'NP=%d' and "
         "rebuild",
         nodes, nodes);
  cJSON *tracee = cJSON_GetObjectItem(cfg, "Tracee");

  for (int i = 0; i < nodes; i++)
  {
    cJSON *argv = cJSON_GetArrayItem(tracee, i);
    int argc = cJSON_GetArraySize(argv);
    Assert(argc <= 5, "More than 5 arguments is not supported");

    ptmc_state.tracee[i].argc = argc;
    for (int j = 0; j < argc; j++)
    {
      cJSON *arg = cJSON_GetArrayItem(argv, j);
      std::string arg_str = cJSON_GetStringValue(arg);
      
      // First argument (executable) is resolved relative to config dir
      if (j == 0) {
        arg_str = resolve_path(arg_str.c_str(), cfg_dir);
      }
      
      // Store the string persistently
      char *stored = (char *)malloc(arg_str.length() + 1);
      strcpy(stored, arg_str.c_str());
      ptmc_state.tracee[i].argv[j] = stored;
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
      ptmc_state.shared_files.emplace(
          std::string(cJSON_GetStringValue(shared_file)));
    }
  }

  /* Working directory for tracees */
  cJSON *working_dir = cJSON_GetObjectItem(cfg, "WorkingDir");
  std::string workdir = "/";
  if (working_dir) {
    workdir = std::string(cJSON_GetStringValue(working_dir));
  }

  /* FSMap: array of mappings { "Host": "/path/on/host", "Target": "/" } */
  cJSON *fsmap = cJSON_GetObjectItem(cfg, "FSMap");
  std::vector<std::pair<std::string, std::string>> maps;
  if (fsmap) {
    int cnt = cJSON_GetArraySize(fsmap);
    for (int i = 0; i < cnt; i++) {
      cJSON *item = cJSON_GetArrayItem(fsmap, i);
      cJSON *host = cJSON_GetObjectItem(item, "Host");
      cJSON *target = cJSON_GetObjectItem(item, "Target");
      if (host && target) {
        std::string host_path = resolve_path(cJSON_GetStringValue(host), cfg_dir);
        maps.emplace_back(host_path, std::string(cJSON_GetStringValue(target)));
      }
    }
  }

  /* Initialize per-node fs state from mappings and set working dir */
  for (int i = 0; i < NP; i++) {
    if (!maps.empty()) {
      ptmc_state.fs_states[i].init_from_mappings(maps);
    }
    ptmc_state.fs_states[i].set_cwd(workdir);
  }

  cJSON *assertions = cJSON_GetObjectItem(cfg, "Assertions");
  if (assertions)
  {
    int assertions_cnt = cJSON_GetArraySize(assertions);
    LOG_DEBUG("Loading %d assertions", assertions_cnt);
    for (int j = 0; j < assertions_cnt; j++)
    {
      cJSON *assertion = cJSON_GetArrayItem(assertions, j);
      std::string assert_str = cJSON_GetStringValue(assertion);
      LOG_DEBUG("  Assertion %d: %s", j, assert_str.c_str());
      ptmc_state.assertions.emplace(assert_str);
    }
  }

  cJSON *user_check = cJSON_GetObjectItem(cfg, "UserCheck");
  if (user_check)
  {
    char *src = cJSON_GetStringValue(user_check);
    std::string src_path = resolve_path(src, cfg_dir);
    std::string obj_path;
    
    // If already a .so file, use it directly
    if (src_path.length() > 3 && src_path.substr(src_path.length() - 3) == ".so") {
      obj_path = src_path;
      LOG_INFO("Loading user check plugin %s", obj_path.c_str());
    } else {
      // Compile .cpp to .so
      std::string obj(src, strchr(src, '.'));
      obj = "./" + obj + ".so";
      LOG_INFO("Compiling user check sources %s to %s", src_path.c_str(), obj.c_str());

      if (access(obj.c_str(), R_OK | X_OK))
      {
        int pid = vfork();
        if (pid == 0)
        {
          char arg_d[10];
          sprintf(arg_d, "-DNP=%d", NP);
          execlp("g++", "g++", "-O2", arg_d, "-fpic", "-shared", 
                 "-I.", "-Isrc", "-Isrc/core", "-Isrc/utils", "-Isrc/subsys",
                 src_path.c_str(), "-o", obj.c_str(), NULL);
          perror("exec");
        }
        else
          waitpid(pid, 0, 0);
      }
      obj_path = obj;
    }
    
    void *handle = dlopen(obj_path.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!handle)
    {
      panic("%s", dlerror());
    }
    int (*func)() = (int (*)())dlsym(handle, "check");
    ptmc_state.user_checks.push_back(func);
    LOG_INFO("Done");
  }

  cJSON *choose_points = cJSON_GetObjectItem(cfg, "ChoosePoint");
  if (choose_points)
  {
    int choose_points_cnt = cJSON_GetArraySize(choose_points);
    for (int j = 0; j < choose_points_cnt; j++)
    {
      cJSON *choose_point = cJSON_GetArrayItem(choose_points, j);
      cJSON *syscall_item = cJSON_GetObjectItem(choose_point, "syscall");
      int nr = -1;
      
      if (cJSON_IsNumber(syscall_item))
      {
        nr = (int)cJSON_GetNumberValue(syscall_item);
      }
      else if (cJSON_IsString(syscall_item))
      {
        /* Convert syscall name to number */
        const char *syscall_name = cJSON_GetStringValue(syscall_item);
        for (int i = 0; i < 450; i++)
        {
          if (syscalls[i] && strcmp(syscalls[i], syscall_name) == 0)
          {
            nr = i;
            break;
          }
        }
        if (nr == -1)
        {
          LOG_ERROR("Unknown syscall name: %s", syscall_name);
          continue;
        }
      }
      else
      {
        LOG_ERROR("Invalid syscall type in ChoosePoint");
        continue;
      }
      
      int n_choose =
          cJSON_GetNumberValue(cJSON_GetObjectItem(choose_point, "choose"));
      choose_many[nr] = n_choose;
    }
  }

  cJSON *choose_function = cJSON_GetObjectItem(cfg, "ChooseFunc");
  if (choose_function)
  {
    char *src = cJSON_GetStringValue(choose_function);
    std::string src_path = resolve_path(src, cfg_dir);
    std::string obj_path;
    
    // If already a .so file, use it directly
    if (src_path.length() > 3 && src_path.substr(src_path.length() - 3) == ".so") {
      obj_path = src_path;
      LOG_INFO("Loading user choose plugin %s", obj_path.c_str());
    } else {
      // Compile .cpp to .so
      std::string obj(src, strchr(src, '.'));
      obj = "./" + obj + ".so";
      LOG_INFO("Compiling user choose function sources %s to %s", src_path.c_str(), obj.c_str());

      if (access(obj.c_str(), R_OK | X_OK))
      {
        int pid = vfork();
        if (pid == 0)
        {
          char arg_d[10];
          sprintf(arg_d, "-DNP=%d", NP);
          execlp("g++", "g++", "-O2", arg_d, "-fpic", "-shared",
                 "-I.", "-Isrc", "-Isrc/core", "-Isrc/utils", "-Isrc/subsys",
                 src_path.c_str(), "-o", obj.c_str(), NULL);
          perror("exec");
        }
        else
          waitpid(pid, 0, 0);
      }
      obj_path = obj;
    }
    
    void *handle = dlopen(obj_path.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!handle)
    {
      panic("%s", dlerror());
    }

    for (int nr = 0; nr < 450; nr++)
    {
      if (syscalls[nr] == NULL)
        continue;
      char funcname[64];
      sprintf(funcname, "choose_%s", syscalls[nr]);
      choose_func func = (choose_func)dlsym(handle, funcname);
      choose_syswhat[nr] = func;
    }
    LOG_INFO("Done");
  }

  /* StateStore configuration */
  cJSON *statestore = cJSON_GetObjectItem(cfg, "StateStore");
  if (statestore)
  {
    StateStore::Config ss_config;
    
    cJSON *hot_mb = cJSON_GetObjectItem(statestore, "hot_cache_mb");
    if (hot_mb) {
      ss_config.hot_cache_size = static_cast<size_t>(cJSON_GetNumberValue(hot_mb)) * 1024 * 1024;
    }
    
    cJSON *warm_mb = cJSON_GetObjectItem(statestore, "warm_cache_mb");
    if (warm_mb) {
      ss_config.warm_cache_size = static_cast<size_t>(cJSON_GetNumberValue(warm_mb)) * 1024 * 1024;
    }
    
    cJSON *prefetch_window = cJSON_GetObjectItem(statestore, "prefetch_window");
    if (prefetch_window) {
      ss_config.prefetch_window = static_cast<size_t>(cJSON_GetNumberValue(prefetch_window));
    }
    
    cJSON *prefetch_threads = cJSON_GetObjectItem(statestore, "prefetch_threads");
    if (prefetch_threads) {
      ss_config.prefetch_threads = static_cast<size_t>(cJSON_GetNumberValue(prefetch_threads));
    }
    
    cJSON *compression_level = cJSON_GetObjectItem(statestore, "compression_level");
    if (compression_level) {
      ss_config.compression_level = static_cast<int>(cJSON_GetNumberValue(compression_level));
    }
    
    cJSON *enable_malloc_trim = cJSON_GetObjectItem(statestore, "enable_malloc_trim");
    if (enable_malloc_trim) {
      ss_config.enable_malloc_trim = cJSON_IsTrue(enable_malloc_trim);
    }
    
    cJSON *malloc_trim_threshold_mb = cJSON_GetObjectItem(statestore, "malloc_trim_threshold_mb");
    if (malloc_trim_threshold_mb) {
      ss_config.malloc_trim_threshold = static_cast<size_t>(cJSON_GetNumberValue(malloc_trim_threshold_mb)) * 1024 * 1024;
    }
    
    /* Initialize StateStore with custom config */
    StateStore::instance().init(ss_config);
    LOG_INFO("StateStore configured: hot=%zuMB, warm=%zuMB, prefetch=%zu, trim=%s",
             ss_config.hot_cache_size / (1024*1024),
             ss_config.warm_cache_size / (1024*1024),
             ss_config.prefetch_window,
             ss_config.enable_malloc_trim ? "on" : "off");
  }
  
  // cJSON_Delete(cfg);
}

int auto_mode = 0;

void parse_args(int argc, char *argv[])
{
  const struct option table[] = {
      {"auto", no_argument, NULL, 'a'},
      {"log", required_argument, NULL, 'l'},
      {"help", no_argument, NULL, 'h'},
      {"cfg", required_argument, NULL, 'c'},
      {0, 0, NULL, 0},
  };
  int o;
  while ((o = getopt_long(argc, argv, "-ahl:c:", table, NULL)) != -1)
  {
    switch (o)
    {
      case 'a':
        auto_mode = 1;
        break;
      case 'l':
        log_file = optarg;
        break;
      case 'c':
        cfg_file = optarg;
        break;
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

int is_auto_mode()
{
  return auto_mode;
}

/* ======================================================================
 * Configuration Cleanup
 * ====================================================================== */

void cleanup_config()
{
  /* Free all allocated argv strings */
  for (int i = 0; i < NP; i++) {
    for (int j = 0; j < ptmc_state.tracee[i].argc; j++) {
      if (ptmc_state.tracee[i].argv[j]) {
        free(ptmc_state.tracee[i].argv[j]);
        ptmc_state.tracee[i].argv[j] = nullptr;
      }
    }
    ptmc_state.tracee[i].argc = 0;
    ptmc_state.tracee[i].executable = nullptr;
  }
}
