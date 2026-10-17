/*
 * commands.cpp - CLI command handlers
 */

#include "commands.h"
#include "scheduler.h"
#include "state/state_transition.h"
#include "engine/pending_syscall.h"
#include "engine/thread_pool.h"
#include "proc_status.h"
#include <charconv>
#include <filesystem>
#include <fstream>
#include <limits.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <set>
#include <unordered_set>
#include <string_view>
#include <cctype>
#include "state/state.h"
#include "state/state_view.h"
#include "state/state_store.h"
#include "state/sysstate_store.h"
#include "core/plugins.h"
#include "monitor.h"
#include "guest.h"
#include "fs/fsstate.h"
#include "net/sockstate.h"
#include "log_wrapper.h"
#include "ncurses_ui.h"
#include "utils.h"
#include <nlohmann/json.hpp>
#include <readline/readline.h>
#include <readline/history.h>
#include <cstring>
#include <cstdlib>

extern char **environ;
#include <cstdio>
#include <sstream>
#include <arpa/inet.h>
#include <netinet/in.h>

/* Defined in expr.cpp */
long expr(const char *e, bool *success);
void expr_print(const char *e);

/* Defined in dwarf.cpp */
void dwarf_print_stack_trace(int pid);
bool dwarf_set_frame(int frame_id);
int dwarf_get_current_frame(void);
void dwarf_print_local_vars(int pid);

/* Defined in config.cpp */
extern int auto_mode;
extern char *cfg_file;
extern char *log_file;

/* ======================================================================
 * Command Table
 * ====================================================================== */

namespace {
const char *pending_phase_name(PendingPhase phase) {
    switch (phase) {
        case PendingPhase::Waiting: return "waiting";
        case PendingPhase::Retry: return "retry";
        case PendingPhase::Ready: return "ready";
    }
    return "unknown";
}

const char *pending_reason_name(PendingReason reason) {
    switch (reason) {
        case PendingReason::Futex: return "futex";
        case PendingReason::Io: return "io";
        case PendingReason::Timer: return "timer";
    }
    return "unknown";
}

std::string pending_syscall_text(const PendingSyscall &pending) {
    const char *name = pending.nr >= 0 && pending.nr < 450 && syscalls[pending.nr]
        ? syscalls[pending.nr] : "unknown";
    char call[512];
    snprintf(call, sizeof(call),
             "%s(0x%lx, 0x%lx, 0x%lx, 0x%lx, 0x%lx, 0x%lx)",
             name, (unsigned long)pending.args[0], (unsigned long)pending.args[1],
             (unsigned long)pending.args[2], (unsigned long)pending.args[3],
             (unsigned long)pending.args[4], (unsigned long)pending.args[5]);
    std::string text = call;
    text += " reason=";
    text += pending_reason_name(pending.reason);
    text += " phase=";
    text += pending_phase_name(pending.phase);
    if (pending.deadline) {
        char deadline[64];
        snprintf(deadline, sizeof(deadline), " deadline=%lld.%09d",
                 (long long)pending.deadline->sec, pending.deadline->nsec);
        text += deadline;
    }
    if (pending.phase == PendingPhase::Ready) {
        text += " result=" + std::to_string(pending.result);
    }
    return text;
}

bool fixed_layout(pid_t pid) {
    std::ifstream input("/proc/" + std::to_string(pid) + "/personality");
    std::string value;
    if (!(input >> value)) return false;
    char *end = nullptr;
    const unsigned long personality = strtoul(value.c_str(), &end, 16);
    return end && !*end && (personality & 0x40000UL);
}

struct JsonDiffOptions {
    bool brief = false;
    size_t max = 10;
    size_t count = 0;
};

std::string compact_json(const nlohmann::json &value) {
    std::string text = value.dump();
    if (text.size() > 120) text = text.substr(0, 117) + "...";
    return text;
}

void json_diff(const nlohmann::json &left, const nlohmann::json &right,
               const std::string &path, JsonDiffOptions &options) {
    if (options.count >= options.max) return;
    if (options.brief) {
        if (left.is_object() && right.is_object()) {
            for (auto it = left.begin(); it != left.end(); ++it) {
                if (options.count >= options.max) return;
                if (!right.contains(it.key()) || right[it.key()] != *it) {
                    detsim::ui::ui_printf("%s/%s differs\n", path.c_str(), it.key().c_str());
                    ++options.count;
                }
            }
            for (auto it = right.begin(); it != right.end(); ++it) {
                if (options.count >= options.max) return;
                if (!left.contains(it.key())) {
                    detsim::ui::ui_printf("%s/%s only in right\n", path.c_str(), it.key().c_str());
                    ++options.count;
                }
            }
        } else if (left != right) {
            detsim::ui::ui_printf("%s differs\n", path.c_str());
            ++options.count;
        }
        return;
    }
    if (left.is_object() && right.is_object()) {
        for (auto it = left.begin(); it != left.end(); ++it) {
            if (options.count >= options.max) return;
            const std::string child = path + "/" + it.key();
            if (!right.contains(it.key())) {
                detsim::ui::ui_printf("%s: %s != <absent>\n", child.c_str(),
                                      compact_json(*it).c_str());
                ++options.count;
            } else {
                json_diff(*it, right[it.key()], child, options);
            }
        }
        for (auto it = right.begin(); it != right.end(); ++it) {
            if (options.count >= options.max) return;
            if (!left.contains(it.key())) {
                detsim::ui::ui_printf("%s/<%s>: <absent> != %s\n", path.c_str(),
                                      it.key().c_str(), compact_json(*it).c_str());
                ++options.count;
            }
        }
        return;
    }
    if (left.is_array() && right.is_array()) {
        const size_t common = std::min(left.size(), right.size());
        for (size_t i = 0; i < common; ++i) {
            if (options.count >= options.max) return;
            json_diff(left[i], right[i], path + "/" + std::to_string(i), options);
        }
        for (size_t i = common; i < left.size(); ++i) {
            if (options.count >= options.max) return;
            detsim::ui::ui_printf("%s/%zu: %s != <absent>\n", path.c_str(), i,
                                  compact_json(left[i]).c_str());
            ++options.count;
        }
        for (size_t i = common; i < right.size(); ++i) {
            if (options.count >= options.max) return;
            detsim::ui::ui_printf("%s/%zu: <absent> != %s\n", path.c_str(), i,
                                  compact_json(right[i]).c_str());
            ++options.count;
        }
        return;
    }
    if (left != right) {
        detsim::ui::ui_printf("%s: %s != %s\n", path.c_str(),
                              compact_json(left).c_str(), compact_json(right).c_str());
        ++options.count;
    }
}
}

static CommandEntry cmd_table[] = {
    {"help", "Display information about all supported commands", cmd_help},
    {"bfs", "Perform breadth-first search [--quiet]", cmd_bfs},
    {"dfs", "Perform depth-first search <depth> [--quiet]", cmd_dfs},
    {"rand", "Perform random search <depth> [--quiet]", cmd_rand},
    {"q", "Exit ptraceMC", cmd_q},
    {"si", "Step selected actor [NONNEGATIVE_CHOICE|timeout]", cmd_si},
    {"sw", "Switch control focus on the n-th process", cmd_sw},
    {"thread", "Switch to thread N in current process (thread to list)", cmd_thread},
    {"pending", "List blocked/ready syscalls, deadlines and runnable actors", cmd_pending},
    {"inspect", "Print saved process/thread/resource JSON (inspect [PROCESS])", cmd_inspect},
    {"plugins", "List loaded plugins, monitor schemas and current monitor bytes", cmd_plugins},
    {"parallel", "Run N tracer workers over distinct successor branches (parallel <N> <depth>)", cmd_parallel},
    {"tree", "Print recorded search edges (source target process choice thread)", cmd_tree},
    {"load", "Load state of given StateHash", cmd_load},
    {"info", "Display state history (info current: scalar current edge only)", cmd_info},
    {"batch", "Read command list from file", cmd_batch},
    {"p", "Calculate expression", cmd_p},
    {"x", "Examine memory: x/[count][format][size] <addr>", cmd_x},
    {"bt", "Print backtrace with arguments", cmd_bt},
    {"frame", "Select stack frame", cmd_frame},
    {"locals", "Show local variables in current frame", cmd_locals},
    {"ls", "List files in current node's filesystem", cmd_ls},
    {"stat", "Show stat of files matching pattern", cmd_stat},
    {"hexdump", "Hexdump of a file (c: continue, q: quit)", cmd_hexdump},
    {"diff", "Compare two saved StateView JSONs [--brief] [--max-diff=N] H1 H2", cmd_diff},
};

const CommandEntry* get_command_table() {
    return cmd_table;
}

size_t get_command_count() {
    return sizeof(cmd_table) / sizeof(cmd_table[0]);
}

const CommandEntry* find_command(const char *name) {
    for (size_t i = 0; i < get_command_count(); i++) {
        if (strcmp(name, cmd_table[i].name) == 0) {
            return &cmd_table[i];
        }
    }
    return nullptr;
}

/* ======================================================================
 * Readline Interface
 * ====================================================================== */

static char *line_read = NULL;

char* read_line(const char *prompt) {
    if (line_read) {
        free(line_read);
        line_read = NULL;
    }

    line_read = readline(prompt);

    if (line_read && *line_read) {
        add_history(line_read);
    }

    return line_read;
}

void cleanup_readline() {
    if (line_read) {
        free(line_read);
        line_read = NULL;
    }
    clear_history();
}

/* ======================================================================
 * Command Parsing and Execution
 * ====================================================================== */

int execute_command_line(char *cmd_line) {
    char *str_end = cmd_line + strlen(cmd_line);
    char *cmd = strtok(cmd_line, " \t\r\n");
    if (cmd == NULL) {
        return 0;
    }

    const CommandEntry *entry = find_command(cmd);
    if (entry == nullptr) {
        detsim::ui::ui_printf("Unknown command '%s'\n", cmd);
        return 0;
    }

    char *args = cmd + strlen(cmd) + 1;
    if (args >= str_end) {
        args = nullptr;
    } else {
        while (*args && isspace(static_cast<unsigned char>(*args))) args++;
        if (*args == '\0') args = nullptr;
    }

    const int result = entry->handler(args);
    refresh_thread_panel();
    return result;
}

/* ======================================================================
 * Basic Commands
 * ====================================================================== */

void show_welcome() {
    detsim::ui::ui_printf("Build time: %s, %s\n", __TIME__, __DATE__);
    detsim::ui::ui_printf("Welcome to ptraceMC!\n");
    detsim::ui::ui_printf("For help, type \"help\"\n");
}

int cmd_help(char *args) {
    if (args == NULL || *args == '\0') {
        for (size_t i = 0; i < get_command_count(); i++) {
            detsim::ui::ui_printf("%s - %s\n", cmd_table[i].name, cmd_table[i].description);
        }
    } else {
        const CommandEntry *entry = find_command(args);
        if (entry) {
            detsim::ui::ui_printf("%s - %s\n", entry->name, entry->description);
        } else {
            detsim::ui::ui_printf("Unknown command '%s'\n", args);
        }
    }
    return 0;
}

int cmd_q(char *args) {
    (void)args;
    ptmc_state.state = PTMC_QUIT;
    detsim::ui::request_ui_exit();
    return -1;
}

/* ======================================================================
 * Execution Control Commands
 * ====================================================================== */

int cmd_si(char *args) {
    int cursor = ptmc_state.cursor;
    if (cursor < 0 || cursor >= NP) {
        detsim::ui::ui_printf("cursor = %d, out of range [0, %d). Please set again.\n",
                              cursor, NP);
        return 0;
    }

    std::string_view text = args ? args : "";
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    const auto action = text == "timeout" ? TransitionAction::Timeout : TransitionAction::Normal;
    if (!text.empty() && action == TransitionAction::Normal) {
        int choice = -1;
        const auto parsed = std::from_chars(text.data(), text.data()+text.size(), choice);
        if (text.front() < '0' || text.front() > '9' || parsed.ec != std::errc{} || parsed.ptr != text.data()+text.size() || choice < 0) {
            detsim::ui::ui_printf("Usage: si [NONNEGATIVE_CHOICE|timeout]\n");
            return 0;
        }
        ptmc_state.batch_choice_preset = choice;
    }
    try {
        int ret = load_exec_store(action);
        if (ret == 0) {
            if (action == TransitionAction::Timeout) detsim::ui::ui_printf("Timeout completed.\n");
            show_syscall(ptmc_state.pids[cursor], ptmc_state.running_state.ts_hash[cursor]);
        }
    } catch (const ActionRejected& error) {
        detsim::ui::ui_printf("Timeout action rejected: %s\n", error.what());
    }
    return 0;
}

int cmd_sw(char *args) {
    if (args == NULL) {
        detsim::ui::ui_printf("Please provide a process number\n");
        return 0;
    }
    int proc = atoi(args);
    if (proc < 0 || proc >= NP) {
        detsim::ui::ui_printf("Invalid process number %d (must be 0-%d)\n", proc, NP - 1);
        return 0;
    }
    ptmc_state.cursor = proc;
    detsim::ui::ui_printf("Switched to process %d (pid=%d)\n", proc, ptmc_state.pids[proc]);
    return 0;
}

int cmd_thread(char *args) {
    int tracee_idx = ptmc_state.cursor;
    if (tracee_idx < 0 || tracee_idx >= NP) {
        detsim::ui::ui_printf("No process selected\n");
        return 0;
    }

    auto& threads = ptmc_state.running_state.child[tracee_idx].threads;

    if (args == NULL || strlen(args) == 0) {
        int current = ptmc_state.current_thread_idx[tracee_idx];
        detsim::ui::ui_printf("Threads in process %d (pid=%d):\n",
                              tracee_idx, ptmc_state.pids[tracee_idx]);
        if (threads.empty()) {
            detsim::ui::ui_printf("  (single-threaded, main thread only) state=runnable\n");
        } else {
            for (size_t i = 0; i < threads.size(); i++) {
                const char* marker = (i == (size_t)current) ? "*" : " ";
                const auto& thread = threads[i];
                const char* state = !thread.alive ? "exited" :
                    thread.pending_syscall && thread.pending_syscall->phase == PendingPhase::Waiting
                        ? "waiting" : "runnable";
                detsim::ui::ui_printf("%s [%zu] TID %d state=%s %s",
                                      marker, i, thread.tid, state,
                                      threads[i].is_main ? "(main)" : "");
                if (thread.alive && thread.pending_syscall)
                    detsim::ui::ui_printf(" %s",
                                          pending_syscall_text(*thread.pending_syscall).c_str());
                detsim::ui::ui_printf("\n");
            }
        }
        return 0;
    }

    int thread_idx = atoi(args);
    if (thread_idx < 0 || thread_idx >= (int)threads.size()) {
        detsim::ui::ui_printf("Invalid thread index %d (must be 0-%zu)\n",
                              thread_idx, threads.size() - 1);
        return 0;
    }

    ptmc_state.current_thread_idx[tracee_idx] = thread_idx;
    // Also update the tracee_state so that execute_step syncs correctly
    ptmc_state.running_state.child[tracee_idx].current_thread_idx = thread_idx;
    detsim::ui::ui_printf("Switched to thread %d (TID=%d) in process %d\n",
                          thread_idx, threads[thread_idx].tid, tracee_idx);
    return 0;
}

int cmd_pending(char *args) {
    int only = -1;
    if (args && *args) {
        char *end = nullptr;
        long value = strtol(args, &end, 10);
        while (end && *end && std::isspace(static_cast<unsigned char>(*end))) ++end;
        if (!end || *end || value < 0 || value >= NP) {
            detsim::ui::ui_printf("Usage: pending [PROCESS]\n");
            return 0;
        }
        only = static_cast<int>(value);
    }

    int pending_count = 0;
    std::vector<std::pair<int, size_t>> runnable;
    detsim::ui::ui_printf("Pending syscalls:\n");
    for (int process = 0; process < NP; ++process) {
        if (only >= 0 && process != only) continue;
        if (DISDEAD(ptmc_state.status[process])) {
            detsim::ui::ui_printf("Process %d (pid=%d): exited\n",
                                  process, ptmc_state.pids[process]);
            continue;
        }
        const auto &child = ptmc_state.running_state.child[process];
        detsim::ui::ui_printf("Process %d (pid=%d)%s:\n", process,
                              ptmc_state.pids[process],
                              process == ptmc_state.cursor ? " [selected]" : "");
        if (child.threads.empty()) {
            detsim::ui::ui_printf("  main thread: state=runnable (no threaded pending metadata)\n");
            runnable.push_back({process, 0});
            continue;
        }
        const int current = ptmc_state.current_thread_idx[process];
        for (size_t slot = 0; slot < child.threads.size(); ++slot) {
            const auto &thread = child.threads[slot];
            const char *marker = slot == static_cast<size_t>(current) ? "*" : " ";
            const char *state = !thread.alive ? "exited" :
                thread.pending_syscall &&
                thread.pending_syscall->phase == PendingPhase::Waiting
                    ? "waiting" : "runnable";
            detsim::ui::ui_printf("%s [%zu] TID %d state=%s%s", marker, slot,
                                  thread.tid, state,
                                  thread.is_main ? " (main)" : "");
            if (thread.alive && thread.pending_syscall) {
                const auto &pending = *thread.pending_syscall;
                ++pending_count;
                detsim::ui::ui_printf(" %s\n", pending_syscall_text(pending).c_str());
                if (pending.phase == PendingPhase::Waiting) {
                    if (pending.deadline)
                        detsim::ui::ui_printf(
                            "       action: si timeout (advance this process clock to the deadline and complete)\n");
                    else
                        detsim::ui::ui_printf(
                            "       action: another actor must wake/complete this wait; use 'thread N' to switch\n");
                } else if (pending.phase == PendingPhase::Ready) {
                    detsim::ui::ui_printf("       action: si 0 to publish the assigned result\n");
                } else {
                    detsim::ui::ui_printf("       action: si 0 to retry readiness\n");
                }
            } else {
                detsim::ui::ui_printf("\n");
                if (thread.alive) runnable.push_back({process, slot});
            }
        }
    }
    detsim::ui::ui_printf("Pending syscalls: %d\n", pending_count);
    if (!runnable.empty()) {
        detsim::ui::ui_printf("Runnable actors:");
        for (const auto &[process, slot] : runnable)
            detsim::ui::ui_printf(" %d:%zu", process, slot);
        detsim::ui::ui_printf("\n");
    } else if (pending_count) {
        detsim::ui::ui_printf(
            "No runnable actor; a deadline or peer I/O is still required.\n");
    }
    return 0;
}

int cmd_inspect(char *args) {
    int process = ptmc_state.cursor;
    if (args && *args) {
        char *end = nullptr;
        long value = strtol(args, &end, 10);
        while (end && *end && std::isspace(static_cast<unsigned char>(*end))) ++end;
        if (!end || *end || value < 0 || value >= NP) {
            detsim::ui::ui_printf("Usage: inspect [PROCESS]\n");
            return 0;
        }
        process = static_cast<int>(value);
    }
    if (process < 0 || process >= NP) {
        detsim::ui::ui_printf("Usage: inspect [PROCESS] (no process selected)\n");
        return 0;
    }
    try {
        detsim::StateView view(ptmc_state.running_state);
        const auto text = view.inspect_json(static_cast<size_t>(process));
        const auto json = nlohmann::json::parse(text);
        detsim::ui::ui_printf("%s\n", json.dump(2).c_str());
    } catch (const std::exception &error) {
        detsim::ui::ui_printf("inspect unavailable: %s\n", error.what());
    }
    return 0;
}

int cmd_plugins(char *args) {
    (void)args;
    detsim::ui::ui_printf("%s", detsim::plugins::describe_modules().c_str());
    detsim::ui::ui_printf("Current monitor states: %zu\n",
                          ptmc_state.running_state.monitor_states.size());
    for (const auto &[key, bytes] : ptmc_state.running_state.monitor_states)
        detsim::ui::ui_printf("  %s (%zu bytes)\n", key.c_str(), bytes.size());
    return 0;
}

int cmd_tree(char *args) {
    (void)args;
    if (state_tree.empty()) {
        detsim::ui::ui_printf("No recorded search edges.\n");
        return 0;
    }
    for (const auto &[target, edge] : state_tree) {
        detsim::ui::ui_printf("EDGE %016lx %016lx %d %d %d\n",
                              std::get<0>(edge), target, std::get<1>(edge),
                              std::get<2>(edge), std::get<3>(edge));
    }
    return 0;
}

int cmd_parallel(char *args) {
    std::istringstream input(args ? args : "");
    int workers = 0;
    int depth = 0;
    int split_depth = 1;
    std::string option;
    if (!(input >> workers >> depth) || workers < 1 || workers > 32 ||
        depth < 1) {
        detsim::ui::ui_printf(
            "Usage: parallel <workers 1..32> <depth> [--split-depth=N]\n");
        return 1;
    }
    while (input >> option) {
        std::string value;
        if (option == "--split-depth") {
            if (!(input >> value)) {
                detsim::ui::ui_printf(
                    "parallel: --split-depth must be 1..depth\n");
                return 1;
            }
        } else if (option.rfind("--split-depth=", 0) == 0) {
            value = option.substr(14);
        } else {
            detsim::ui::ui_printf(
                "Usage: parallel <workers 1..32> <depth> [--split-depth=N]\n");
            return 1;
        }
        size_t parsed = 0;
        const auto result = std::from_chars(value.data(),
            value.data() + value.size(), parsed);
        if (value.empty() || result.ec != std::errc{} ||
            result.ptr != value.data() + value.size() ||
            parsed < 1 || parsed > static_cast<size_t>(depth)) {
            detsim::ui::ui_printf("parallel: --split-depth must be 1..depth\n");
            return 1;
        }
        split_depth = static_cast<int>(parsed);
    }
    for (int i = 0; i < NP; ++i) {
        if (!fixed_layout(ptmc_state.pids[i])) {
            detsim::ui::ui_printf(
                "parallel requires fixed layout; launch with `setarch x86_64 -R`\n");
            return 1;
        }
    }

    const hash_type original_hash = ptmc_state.running_state.ss_hash;
    std::vector<hash_type> branches{original_hash};
    std::set<hash_type> seen{original_hash};
    try {
        for (int level = 0; level < split_depth; ++level) {
            std::vector<hash_type> next;
            for (hash_type hash : branches) {
                load(hash);
                for (hash_type successor : enumerate_successors(ptmc_state.running_state)) {
                    if (successor && seen.insert(successor).second)
                        next.push_back(successor);
                }
            }
            if (next.empty()) break;
            if (next.size() > 512) {
                detsim::ui::ui_printf(
                    "parallel: frontier capped at 512 branches (was %zu)\n",
                    next.size());
                next.resize(512);
            }
            branches.swap(next);
        }
        load(original_hash);
    } catch (const std::exception &error) {
        detsim::ui::ui_printf("parallel branch enumeration failed: %s\n", error.what());
        return 1;
    }
    if (branches.empty()) {
        detsim::ui::ui_printf("parallel: no runnable successor branches\n");
        return 1;
    }
    if (workers > static_cast<int>(branches.size()))
        workers = static_cast<int>(branches.size());

    namespace fs = std::filesystem;
    const fs::path cwd = fs::current_path();
    const fs::path base = cwd / (".parallel-" + std::to_string(getpid()) + "-" +
                                 std::to_string(time(nullptr)));
    const fs::path config = cfg_file ? fs::absolute(cfg_file) : cwd / "config.json";
    if (!fs::is_regular_file(config)) {
        detsim::ui::ui_printf("parallel: config file not found: %s\n",
                              config.c_str());
        return 1;
    }
    char exe_buffer[PATH_MAX];
    const ssize_t exe_size = readlink("/proc/self/exe", exe_buffer,
                                      sizeof(exe_buffer) - 1);
    if (exe_size <= 0) {
        detsim::ui::ui_printf("parallel: cannot locate tracer executable\n");
        return 1;
    }
    exe_buffer[exe_size] = '\0';
    const std::string exe = exe_buffer;

    try {
        StateStore::instance().wait_for_completion();
        SysStateStore::instance().flush_index();
        fs::create_directories(base);
    } catch (const std::exception &error) {
        detsim::ui::ui_printf("parallel store barrier failed: %s\n", error.what());
        return 1;
    }

    struct Worker {
        pid_t pid = -1;
        fs::path directory;
        fs::path output;
        size_t branch_count = 0;
        std::vector<hash_type> assigned;
    };
    std::vector<Worker> started;
    const char *shell = "exec \"$0\" -n -c \"$1\" -b \"$2\" > \"$3\" 2>&1";
    for (int index = 0; index < workers; ++index) {
        Worker worker;
        worker.directory = base / ("worker-" + std::to_string(index));
        worker.output = worker.directory / "worker.out";
        try {
            fs::create_directories(worker.directory);
            fs::copy(cwd / "memory", worker.directory / "memory",
                     fs::copy_options::recursive);
            fs::copy(cwd / "sstate", worker.directory / "sstate",
                     fs::copy_options::recursive);
            const auto lock = worker.directory / "memory" / ".packed.lock";
            if (fs::exists(lock)) fs::remove(lock);
            fs::copy_file(config, worker.directory / "config.json",
                          fs::copy_options::overwrite_existing);
        } catch (const std::exception &error) {
            detsim::ui::ui_printf("parallel: worker %d setup failed: %s\n",
                                  index, error.what());
            return 1;
        }

        const fs::path commands = worker.directory / "commands";
        std::ofstream script(commands);
        if (!script) {
            detsim::ui::ui_printf("parallel: cannot write worker commands\n");
            return 1;
        }
        script << "sw 0\n";
        for (size_t branch = index; branch < branches.size();
             branch += static_cast<size_t>(workers)) {
            char branch_hash[32];
            snprintf(branch_hash, sizeof(branch_hash), "%016lx", branches[branch]);
            script << "load --adopt " << branch_hash << "\n";
            worker.assigned.push_back(branches[branch]);
            ++worker.branch_count;
            if (depth > split_depth)
                script << "dfs " << (depth - split_depth) << " --quiet\n";
        }
        script << "tree\nq\n";
        script.close();

        posix_spawn_file_actions_t file_actions;
        if (posix_spawn_file_actions_init(&file_actions) != 0) {
            detsim::ui::ui_printf("parallel: spawn actions init failed\n");
            return 1;
        }
        posix_spawn_file_actions_addchdir_np(&file_actions,
                                             worker.directory.c_str());
        std::string exe_arg = exe;
        std::string config_arg = config.string();
        std::string commands_arg = commands.string();
        std::string output_arg = worker.output.string();
        char *argv[] = {const_cast<char *>("/bin/sh"),
                        const_cast<char *>("-c"),
                        const_cast<char *>(shell),
                        exe_arg.data(), config_arg.data(), commands_arg.data(),
                        output_arg.data(), nullptr};
        const int spawn_result = posix_spawn(&worker.pid, "/bin/sh", &file_actions,
                                             nullptr, argv, environ);
        posix_spawn_file_actions_destroy(&file_actions);
        if (spawn_result != 0) {
            detsim::ui::ui_printf("parallel: worker %d spawn failed: %s\n",
                                  index, strerror(spawn_result));
            return 1;
        }
        started.push_back(std::move(worker));
    }

    detsim::ui::ui_printf(
        "Parallel exploration: %d workers, %zu frontier branches, depth %d, split %d\n",
        workers, branches.size(), depth, split_depth);
    detsim::ui::ui_printf("Artifacts: %s\n", base.c_str());
    bool failed = false;
    std::unordered_set<hash_type> merged_states;
    std::unordered_set<std::string> merged_edges;
    for (size_t index = 0; index < started.size(); ++index) {
        int status = 0;
        if (waitpid(started[index].pid, &status, 0) != started[index].pid) {
            detsim::ui::ui_printf("parallel: worker %zu wait failed: %s\n",
                                  index, strerror(errno));
            failed = true;
            continue;
        }
        const int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        if (code != 0) failed = true;
        size_t worker_edges = 0;
        try {
            SysStateStore::inspect(
                (started[index].directory / "sstate").string(),
                [&](hash_type key, const SysStateEntry&, const std::vector<uint8_t>&) {
                    merged_states.insert(key);
                });
        } catch (const std::exception &error) {
            detsim::ui::ui_printf("parallel: worker %zu store inspect failed: %s\n",
                                  index, error.what());
            failed = true;
        }
        std::ifstream output(started[index].output);
        std::string line;
        int printed = 0;
        while (std::getline(output, line)) {
            if (line.rfind("EDGE ", 0) == 0) {
                if (merged_edges.insert(line).second) ++worker_edges;
                continue;
            }
            if (printed < 3 &&
                (line.find("Stopped for illegal state") != std::string::npos ||
                 line.find("Complete explore all") != std::string::npos ||
                 line.find("No runnable") != std::string::npos)) {
                detsim::ui::ui_printf("  %s\n", line.c_str());
                ++printed;
            }
        }
        detsim::ui::ui_printf(
            "worker %zu (%zu branches): exit %d, edges=%zu, output=%s\n",
            index, started[index].branch_count, code, worker_edges,
            started[index].output.c_str());
    }
    for (const auto &[target, edge] : state_tree) {
        char text[128];
        snprintf(text, sizeof(text), "EDGE %016lx %016lx %d %d %d",
                 std::get<0>(edge), target, std::get<1>(edge),
                 std::get<2>(edge), std::get<3>(edge));
        merged_edges.insert(text);
    }
    detsim::ui::ui_printf("Merged: %zu unique states, %zu unique edges\n",
                          merged_states.size(), merged_edges.size());
    return failed ? 1 : 0;
}

int cmd_load(char *args) {
    if (args == NULL) {
        detsim::ui::ui_printf("Usage: load [--adopt] <hash_prefix>\n");
        return 1;
    }

    std::istringstream input(args);
    std::string token;
    std::string prefix;
    bool adopt = false;
    while (input >> token) {
        if (token == "--adopt") {
            adopt = true;
        } else if (prefix.empty()) {
            prefix = token;
        } else {
            detsim::ui::ui_printf("Usage: load [--adopt] <hash_prefix>\n");
            return 1;
        }
    }
    if (prefix.empty()) {
        detsim::ui::ui_printf("Usage: load [--adopt] <hash_prefix>\n");
        return 1;
    }

    SysStateStore &store = SysStateStore::instance();
    std::vector<hash_type> matches = store.find_by_prefix(prefix);

    if (matches.size() == 1) {
        sys_state state(matches[0]);
        if (adopt) {
            if (ptmc_state.model_identity.empty() ||
                state.model_identity != ptmc_state.model_identity) {
                detsim::ui::ui_printf("Load rejected: snapshot execution model differs\n");
                return 0;
            }
            for (int i = 0; i < NP; ++i) {
                if (!fixed_layout(ptmc_state.pids[i])) {
                    detsim::ui::ui_printf(
                        "Load --adopt rejected: process %d is not fixed-layout; "
                        "launch both seed and worker with `setarch x86_64 -R`\n", i);
                    return 0;
                }
            }
            /* An adopting worker has bootstrapped the same executable but has no
             * capture provenance for the seed's snapshot. Require the local
             * logical thread layout to match exactly, then bind its carriers. */
            for (int i = 0; i < NP; ++i) {
                const auto &current = ptmc_state.running_state.child[i].threads;
                const auto &target = state.child[i].threads;
                if (current.size() != target.size() || current.empty()) {
                    detsim::ui::ui_printf(
                        "Load --adopt rejected: process %d thread layout differs\n", i);
                    return 0;
                }
                for (size_t j = 0; j < current.size(); ++j) {
                    if (current[j].tid != target[j].tid ||
                        current[j].alive != target[j].alive ||
                        current[j].is_main != target[j].is_main) {
                        detsim::ui::ui_printf(
                            "Load --adopt rejected: process %d thread layout differs\n", i);
                        return 0;
                    }
                }
            }
            for (int i = 0; i < NP; ++i)
                prepare_thread_pool(i, ptmc_state.running_state.child[i]);
            for (int i = 0; i < NP; ++i)
                adopt_snapshot_bindings(i, state.ts_hash[i], state.child[i]);
        }
        state.recover_running_state();
        detsim::ui::ui_printf("Loaded state %016lx%s\n", matches[0],
                              adopt ? " (adopted cross-process bindings)" : "");
    } else if (matches.size() == 0) {
        detsim::ui::ui_printf("No state found with prefix '%s'\n", prefix.c_str());
    } else {
        detsim::ui::ui_printf("Multiple states match prefix '%s':\n", prefix.c_str());
        for (size_t i = 0; i < matches.size() && i < 10; i++) {
            detsim::ui::ui_printf("  %016lx\n", matches[i]);
        }
        if (matches.size() > 10) {
            detsim::ui::ui_printf("  ... and %zu more\n", matches.size() - 10);
        }
    }
    return 0;
}

/* ======================================================================
 * Exploration Commands
 * ====================================================================== */

namespace {
// Display policy belongs to the command, including every scheduler early return.
struct SearchDisplayScope {
    int previous_auto = auto_mode;
    bool previous_history = syscall_history_enabled;
    explicit SearchDisplayScope(bool quiet) {
        auto_mode = 1;
        syscall_history_enabled = !quiet;
    }
    ~SearchDisplayScope() {
        auto_mode = previous_auto;
        syscall_history_enabled = previous_history;
    }
};

bool parse_search_args(char* args, bool needs_depth, int& depth, bool& quiet) {
    std::istringstream input(args ? args : "");
    std::string token;
    if (needs_depth) {
        if (!(input >> depth) || depth <= 0) return false;
    }
    if (input >> token) {
        if (token != "--quiet") return false;
        quiet = true;
        if (input >> token) return false;
    }
    return true;
}
}

int cmd_bfs(char *args) {
    int depth = 0; bool quiet = false;
    if (!parse_search_args(args, false, depth, quiet)) {
        detsim::ui::ui_printf("Usage: bfs [--quiet]\n");
        return 1;
    }
    SearchDisplayScope display(quiet);
    exec_bfs();
    return 0;
}

int cmd_dfs(char *args) {
    int depth = 0; bool quiet = false;
    if (!parse_search_args(args, true, depth, quiet)) {
        detsim::ui::ui_printf("Usage: dfs <depth> [--quiet]\n");
        return 1;
    }
    SearchDisplayScope display(quiet);
    return exec_dfs(depth);
}

int cmd_rand(char *args) {
    int depth = 0; bool quiet = false;
    if (!parse_search_args(args, true, depth, quiet)) {
        detsim::ui::ui_printf("Usage: rand <depth> [--quiet]\n");
        return 1;
    }
    SearchDisplayScope display(quiet);
    return exec_rand(depth);
}

/* ======================================================================
 * Expression and Debug Commands
 * ====================================================================== */

int cmd_p(char *args) {
    if (!args || (args[0] == 0)) {
        detsim::ui::ui_printf("Give an expression\n");
        return 1;
    }
    expr_print(args);
    return 0;
}

int cmd_bt(char *args) {
    (void)args;
    int pid = ptmc_state.pids[ptmc_state.cursor];
    dwarf_print_stack_trace(pid);
    return 0;
}

int cmd_frame(char *args) {
    if (!args) {
        detsim::ui::ui_printf("Current frame: %d\n", dwarf_get_current_frame());
        return 0;
    }
    int frame = atoi(args);
    if (dwarf_set_frame(frame)) {
        detsim::ui::ui_printf("Switched to frame %d\n", frame);
    } else {
        detsim::ui::ui_printf("Invalid frame %d\n", frame);
    }
    return 0;
}

int cmd_locals(char *args) {
    (void)args;
    int pid = ptmc_state.pids[ptmc_state.cursor];
    dwarf_print_local_vars(pid);
    return 0;
}

/* ======================================================================
 * Memory Commands
 * ====================================================================== */

int cmd_x(char *args) {
    if (!args) {
        detsim::ui::ui_printf("Usage: x/[count][format][size] <addr>\n");
        return 0;
    }

    int count = 4;
    char format = 'x';
    int size = 1;
    uint64_t addr = 0;

    if (args[0] == '/') {
        args++;
        if (isdigit(args[0])) {
            count = atoi(args);
            while (isdigit(*args)) args++;
        }
        if (*args && strchr("xd uotcsi", *args)) {
            format = *args;
            args++;
        }
        if (*args) {
            switch (*args) {
                case 'b': size = 1; break;
                case 'h': size = 2; break;
                case 'w': size = 4; break;
                case 'g': size = 8; break;
            }
            args++;
        }
        while (*args && isspace(*args)) args++;
    }

    addr = strtoul(args, NULL, 0);
    int pid = ptmc_state.pids[ptmc_state.cursor];

    detsim::ui::ui_printf("Memory at 0x%lx:\n", addr);

    for (int i = 0; i < count; i++) {
        if (i % 4 == 0) {
            if (i > 0) detsim::ui::ui_printf("\n");
            detsim::ui::ui_printf("  0x%016lx: ", addr + i * size);
        }

        uint64_t val = 0;
        switch (size) {
            case 1: val = tracee_read_byte(pid, (void *)(addr + i)); break;
            case 2: val = tracee_read_word(pid, (void *)(addr + i * 2)) & 0xFFFF; break;
            case 4: val = tracee_read_word(pid, (void *)(addr + i * 4)) & 0xFFFFFFFF; break;
            case 8: val = tracee_read_word(pid, (void *)(addr + i * 8)); break;
        }

        switch (format) {
            case 'x': detsim::ui::ui_printf("%0*lx ", size * 2, val); break;
            case 'd': detsim::ui::ui_printf("%*ld ", size * 3, (int64_t)val); break;
            case 'u': detsim::ui::ui_printf("%*lu ", size * 3, val); break;
            case 'o': detsim::ui::ui_printf("%0*lo ", size * 3, val); break;
            default:  detsim::ui::ui_printf("%0*lx ", size * 2, val); break;
        }
    }
    detsim::ui::ui_printf("\n");
    return 0;
}

/* ======================================================================
 * Info and State Commands
 * ====================================================================== */

static std::string format_addr_short(const struct sockaddr_in &addr) {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    char buf[64];
    snprintf(buf, sizeof(buf), "%s:%d", ip, ntohs(addr.sin_port));
    return std::string(buf);
}

int cmd_info(char *args) {
    if (args && strcmp(args, "current") == 0) {
        show_syscall_current();
        return 0;
    }
    if (args && (strcmp(args, "sock") == 0 || strcmp(args, "socket") == 0)) {
        detsim::ui::ui_printf("[Socket States]\n");
        for (int i = 0; i < NP; i++) {
            const auto &sockets = ptmc_state.sock_states[i].sockets();
            if (!sockets.empty()) {
                detsim::ui::ui_printf("Process %d:\n", i);
                for (const auto &kv : sockets) {
                    const Socket &sock = kv.second;
                    detsim::ui::ui_printf("  fd=%d: type=%d\n", sock.fd, sock.type);
                }
            }
        }
        return 0;
    }

    detsim::ui::ui_printf("Current process: %d (pid=%d)\n",
                          ptmc_state.cursor, ptmc_state.pids[ptmc_state.cursor]);
    show_syscall_history();
    return 0;
}

int cmd_diff(char *args) {
    std::istringstream input(args ? args : "");
    std::string token;
    std::vector<std::string> prefixes;
    JsonDiffOptions options;
    bool memory_only = false;

    while (input >> token) {
        if (token == "-b" || token == "--brief") {
            options.brief = true;
        } else if (token == "-m" || token == "--memory-only") {
            memory_only = true;
        } else if (token.rfind("--max-diff=", 0) == 0) {
            const std::string value = token.substr(11);
            size_t parsed = 0;
            const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (value.empty() || result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
                parsed == 0 || parsed > 10000) {
                detsim::ui::ui_printf("Usage: diff [--brief] [--max-diff=N] HASH1 HASH2\n");
                return 0;
            }
            options.max = parsed;
        } else if (!token.empty() && token[0] == '-') {
            detsim::ui::ui_printf("Usage: diff [--brief] [--max-diff=N] HASH1 HASH2\n");
            return 0;
        } else {
            prefixes.push_back(token);
        }
    }

    if (memory_only) {
        detsim::ui::ui_printf(
            "diff: memory-only comparison is unavailable; this command compares saved StateView JSON\n");
        return 0;
    }
    if (prefixes.size() != 2) {
        detsim::ui::ui_printf("Usage: diff [--brief] [--max-diff=N] HASH1 HASH2\n");
        return 0;
    }

    hash_type hashes[2] = {};
    for (int i = 0; i < 2; ++i) {
        auto matches = SysStateStore::instance().find_by_prefix(prefixes[i]);
        if (matches.size() == 1) {
            hashes[i] = matches[0];
        } else if (matches.empty()) {
            detsim::ui::ui_printf("No state found with prefix '%s'\n", prefixes[i].c_str());
            return 0;
        } else {
            detsim::ui::ui_printf("Multiple states match prefix '%s':\n", prefixes[i].c_str());
            for (size_t j = 0; j < matches.size() && j < 10; ++j)
                detsim::ui::ui_printf("  %016lx\n", matches[j]);
            if (matches.size() > 10)
                detsim::ui::ui_printf("  ... and %zu more\n", matches.size() - 10);
            return 0;
        }
    }

    detsim::ui::ui_printf("=== State diff: %016lx vs %016lx ===\n",
                          hashes[0], hashes[1]);
    try {
        sys_state left(hashes[0]);
        sys_state right(hashes[1]);
        for (int process = 0; process < NP; ++process) {
            if (options.count >= options.max) break;
            detsim::StateView left_view(left);
            detsim::StateView right_view(right);
            const auto left_json = nlohmann::json::parse(left_view.inspect_json(process));
            const auto right_json = nlohmann::json::parse(right_view.inspect_json(process));
            const size_t before = options.count;
            json_diff(left_json, right_json, "process" + std::to_string(process), options);
            if (options.count > before)
                detsim::ui::ui_printf("  (process %d)\n", process);
        }
    } catch (const std::exception &error) {
        detsim::ui::ui_printf("diff unavailable: %s\n", error.what());
        return 0;
    }

    if (options.count == 0)
        detsim::ui::ui_printf("No StateView differences.\n");
    else
        detsim::ui::ui_printf("Differences: %zu%s\n", options.count,
                              options.count >= options.max ? " (limit reached)" : "");
    return 0;
}

/* ======================================================================
 * Filesystem Commands
 * ====================================================================== */

int cmd_ls(char *args) {
    (void)args;
    int tracee_idx = ptmc_state.cursor;
    auto &fs = ptmc_state.fs_states[tracee_idx];

    detsim::ui::ui_printf("Files in VFS for process %d:\n", tracee_idx);
    for (const auto &kv : fs.filesystem) {
        detsim::ui::ui_printf("  %s\n", kv.first.c_str());
    }
    return 0;
}

int cmd_stat(char *args) {
    (void)args;
    detsim::ui::ui_printf("stat: not yet implemented\n");
    return 0;
}

int cmd_hexdump(char *args) {
    (void)args;
    detsim::ui::ui_printf("hexdump: not yet implemented\n");
    return 0;
}

/* ======================================================================
 * Batch Commands
 * ====================================================================== */

int cmd_batch(char *args) {
    static int in_call = 0;
    if (in_call) {
        detsim::ui::ui_printf("cmd_batch is not designed for nested invoke\n");
        return 1;
    }
    in_call = 1;

    if (args == NULL) {
        detsim::ui::ui_printf("Usage: batch <filename>\n");
        in_call = 0;
        return 1;
    }

    FILE *fp = fopen(args, "r");
    if (!fp) {
        detsim::ui::ui_printf("Cannot open file: %s\n", args);
        in_call = 0;
        return 1;
    }

    char line[1024];
    int cmd_count = 0;

    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';

        char *p = line;
        while (*p && isspace(*p)) p++;
        if (*p == '\0' || *p == '#') continue;

        int ret = execute_command_line(p);
        cmd_count++;

        if (ret < 0) break;
    }

    fclose(fp);
    in_call = 0;

    detsim::ui::ui_printf("Batch executed %d commands from %s\n", cmd_count, args);
    return 0;
}
