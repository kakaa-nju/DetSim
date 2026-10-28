/*
 * user_plugin.cpp - User-facing check/choose plugin runtime
 *
 * Provides the plugin surface this tracer has always exposed to users
 * (UserCheck / UserState / ChooseFunc JSON keys, C/C++ hook functions) on top
 * of the newer core, whose native plugin ABI is an internal detail and is not
 * part of the user contract.
 *
 *   UserCheck / UserState : "check.c" (.cpp) or prebuilt "check.so"
 *       exports check(), optional user_state_size/save/load
 *   ChooseFunc            : "choose.c" (.cpp) or prebuilt "choose.so"
 *       exports choose_<syscall> for the syscalls it cares about
 *
 * Sources are compiled with subprocess g++ on demand into "<dir>/<stem>.so".
 * An existing .so is reused as long as it is not older than its source.
 */

#include "user_plugin.h"
#include "debug.h"
#include "monitor.h"
#include "guest.h"
#include "types.h"
#include <dlfcn.h>
#include <filesystem>
#include <string>
#include <vector>
#include <memory>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <unistd.h>
#include <sys/wait.h>

namespace fs = std::filesystem;

namespace detsim
{
namespace user_plugin
{
namespace
{

struct Module
{
  void *handle = nullptr;
  std::string path;
  ~Module()
  {
    if (handle)
      dlclose(handle);
  }
  Module() = default;
  Module(const Module &) = delete;
  Module &operator=(const Module &) = delete;
};

std::vector<std::unique_ptr<Module>> modules;

int (*g_check_fn)(void) = nullptr;
int (*g_state_size_fn)(int) = nullptr;
void (*g_state_save_fn)(int, void *) = nullptr;
void (*g_state_load_fn)(int, const void *, int) = nullptr;
choose_func g_choose[450] = {};

/* Maximum size of one tracee's opaque user state blob. */
constexpr size_t kMaxUserState = 64u * 1024u * 1024u;

std::string resolve_path(const char *path, const char *cfg_dir)
{
  if (!path || !*path)
    return {};
  fs::path p(path);
  if (p.is_absolute())
    return p.string();
  if (cfg_dir && *cfg_dir)
    return (fs::path(cfg_dir) / p).string();
  return p.string();
}

/* If the plugin is already a shared object, use it as-is. */
bool is_shared_object(const std::string &path)
{
  return path.size() > 3 && path.compare(path.size() - 3, 3, ".so") == 0;
}

/* The object path a source plugin is built into. */
std::string built_object_path(const std::string &source)
{
  if (is_shared_object(source))
    return source;
  fs::path src(source);
  fs::path object = src.parent_path() / (src.stem().string() + ".so");
  return object.string();
}

void *module_handle_for_object(const std::string &object)
{
  if (object.empty())
    return nullptr;
  for (const auto &module : modules)
    if (module->path == object)
      return module->handle;
  return nullptr;
}

std::string build_shared_object(const std::string &source)
{
  fs::path src(source);
  fs::path object = src.parent_path() / (src.stem().string() + ".so");
  const std::string directory = src.parent_path().empty() ? "." : src.parent_path().string();

  bool stale = false;
  try
  {
    if (!fs::exists(object))
      stale = true;
    else
      stale = fs::last_write_time(src) > fs::last_write_time(object);
  }
  catch (const fs::filesystem_error &)
  {
    stale = true;
  }

  if (stale)
  {
    LOG_INFO("Compiling %s -> %s", source.c_str(), object.c_str());
    std::vector<std::string> args = {
        "g++", "-O2", "-DNP=" + std::to_string(NP), "-fpic", "-shared",
        "-I.", "-Isrc", "-Isrc/core", "-Isrc/core/state", "-Isrc/core/engine",
        "-Isrc/core/syscall", "-Isrc/subsys", "-Isrc/subsys/fs",
        "-Isrc/subsys/net", "-Isrc/subsys/sync", "-Isrc/utils", "-Isrc/ui",
        "-Ithird_party", "-Ithird_party/cereal/include",
        "-I" + directory,
        "-D_FORTIFY_SOURCE=0",
        source, "-o", object.string()};
    std::vector<char *> argv;
    for (auto &arg : args)
      argv.push_back(arg.data());
    argv.push_back(nullptr);
    fflush(nullptr);
    pid_t pid = fork();
    if (pid < 0)
      throw std::runtime_error("cannot fork to compile user plugin");
    if (pid == 0)
    {
      execvp(argv[0], argv.data());
      fprintf(stderr, "cannot execute g++ for plugin %s: %s\n", source.c_str(),
              strerror(errno));
      _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    {
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
      throw std::runtime_error("cannot build user plugin from " + source);
  }
  return object.string();
}

Module *load(const std::string &source, bool compile)
{
  std::string path = is_shared_object(source) ? source
                     : compile                ? build_shared_object(source)
                                              : std::string{};
  if (path.empty())
    return nullptr;
  auto module = std::make_unique<Module>();
  module->handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!module->handle)
    throw std::runtime_error("cannot load user plugin " + path + ": " + dlerror());
  module->path = path;
  modules.push_back(std::move(module));
  return modules.back().get();
}

} // namespace

void reset()
{
  memset(g_choose, 0, sizeof(g_choose));
  memset(choose_syswhat, 0, sizeof(choose_syswhat));
  g_check_fn = nullptr;
  g_state_size_fn = nullptr;
  g_state_save_fn = nullptr;
  g_state_load_fn = nullptr;
  modules.clear();
}

void configure(const char *check_source, const char *state_source,
               const char *choose_source, const char *configuration_directory)
{
  reset();
  const std::string check = resolve_path(check_source, configuration_directory);
  const std::string state = resolve_path(state_source, configuration_directory);
  const std::string choose = resolve_path(choose_source, configuration_directory);

  if (!check.empty())
  {
    Module *module = load(check, true);
    if (module)
    {
      g_check_fn = reinterpret_cast<int (*)(void)>(dlsym(module->handle, "check"));
      if (!g_check_fn)
        LOG_WARN("UserCheck plugin %s does not export check()", module->path.c_str());
      LOG_INFO("User check plugin loaded");
    }
  }
  if (!state.empty())
  {
    /* The state hooks may live in the UserState file or in the check plugin;
     * reuse an already loaded module when both point at the same build. */
    std::string object = built_object_path(state);
    std::string check_object = check.empty() ? std::string{} : built_object_path(check);
    void *handle = module_handle_for_object(object);
    if (!handle && object != check_object)
    {
      Module *module = load(state, true);
      if (module)
        handle = module->handle;
    }
    if (!handle && g_check_fn && !modules.empty())
      handle = modules.front()->handle;
    if (handle)
    {
      g_state_size_fn = reinterpret_cast<int (*)(int)>(dlsym(handle, "user_state_size"));
      g_state_save_fn = reinterpret_cast<void (*)(int, void *)>(dlsym(handle, "user_state_save"));
      g_state_load_fn = reinterpret_cast<void (*)(int, const void *, int)>(dlsym(handle, "user_state_load"));
      LOG_INFO("User state hooks: size=%s save=%s load=%s",
               g_state_size_fn ? "yes" : "no", g_state_save_fn ? "yes" : "no",
               g_state_load_fn ? "yes" : "no");
    }
  }
  if (!choose.empty())
  {
    Module *module = load(choose, true);
    if (module)
    {
      for (int nr = 0; nr < 450; ++nr)
      {
        const char *name = syscalls[nr];
        if (!name)
          continue;
        std::string symbol = std::string("choose_") + name;
        g_choose[nr] = reinterpret_cast<choose_func>(dlsym(module->handle, symbol.c_str()));
        choose_syswhat[nr] = g_choose[nr];
      }
      LOG_INFO("Choose function plugin loaded");
    }
  }
}

bool has_check() { return g_check_fn != nullptr; }

bool check_state(const sys_state &state, int process)
{
  (void)state;
  (void)process;
  if (!g_check_fn)
    return false;
  return g_check_fn() != 0;
}

std::string describe()
{
  if (modules.empty())
    return "No user plugins loaded\n";
  std::string out;
  for (const auto &module : modules)
    out += "- " + module->path + "\n";
  out += std::string("Check hook: ") + (g_check_fn ? "yes" : "no") + "\n";
  if (user_state_enabled())
    out += "User state hooks: yes\n";
  int choices = 0;
  for (int nr = 0; nr < 450; ++nr)
    if (g_choose[nr])
      ++choices;
  out += "Choose hooks: " + std::to_string(choices) + "\n";
  return out;
}

bool user_state_enabled() { return g_state_size_fn != nullptr && g_state_save_fn != nullptr; }

void user_state_capture(int index, std::vector<unsigned char> &out)
{
  out.clear();
  if (!user_state_enabled())
    return;
  int size = g_state_size_fn(index);
  if (size <= 0 || static_cast<size_t>(size) > kMaxUserState)
  {
    if (size > 0)
      throw std::runtime_error("UserState plugin returned an oversized blob");
    return;
  }
  out.resize(static_cast<size_t>(size));
  g_state_save_fn(index, out.data());
}

void user_state_restore(int index, const std::vector<unsigned char> &blob)
{
  if (!g_state_load_fn || blob.empty())
    return;
  g_state_load_fn(index, blob.data(), static_cast<int>(blob.size()));
}

} // namespace user_plugin
} // namespace detsim
