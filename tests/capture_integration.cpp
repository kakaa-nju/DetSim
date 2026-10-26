#include "config.h"
#include "monitor.h"
#include "ncurses_ui.h"
#include "state_store.h"
#include "sysstate_store.h"
#include "guest.h"
#include "dispatcher.h"
#include "proc_status.h"
#include "utils.h"
#include "page_snapshot.h"
#include "state.h"
#include "resident_memory.h"
#include "shared_object_pool.h"
#include "engine/exec_engine.h"
#include "engine/thread_pool.h"
#include "state_transition.h"
#include <array>
#include <atomic>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <arpa/inet.h>
#include <cerrno>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <algorithm>
#include <linux/prctl.h>
#include <sstream>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <sys/rseq.h>
#include <sys/syscall.h>
#include <sys/user.h>

static long guest_value = 17;
alignas(4096) static const long readonly_value __attribute__((used)) = 31;
alignas(32) static struct rseq registered_rseq{};
static_assert(NP == 2, "capture oracle requires two processes");
// The rseq fixture is registered through the host-only injection path. Count
// only PTRACE_GET_RSEQ_CONFIGURATION, never aggregate syscalls.
static unsigned long rseq_configuration_calls = 0;
static std::atomic<bool> fail_store_sync{false};
extern "C" int __real_fsync(int fd);
extern "C" int __wrap_fsync(int fd)
{
    if (fail_store_sync.load()) { errno = EIO; return -1; }
    return __real_fsync(fd);
}
// Test-only: fail the next read-only injection after its real stop has been
// consumed. Corrupting the reported wait status, rather than throwing out of a
// libc wrapper, makes tracee_do_syscall throw through its normal status check.
static pid_t fail_wait_pid = 0;
extern "C" long __real_ptrace(enum __ptrace_request request, ...);
extern "C" long __wrap_ptrace(enum __ptrace_request request, ...)
{
    va_list args;
    va_start(args, request);
    const pid_t pid = va_arg(args, pid_t);
    void* const addr = va_arg(args, void*);
    void* const data = va_arg(args, void*);
    va_end(args);
    if (request == PTRACE_GET_RSEQ_CONFIGURATION) ++rseq_configuration_calls;
    return __real_ptrace(request, pid, addr, data);
}
extern "C" pid_t __real_waitpid(pid_t, int*, int);
extern "C" pid_t __wrap_waitpid(pid_t pid, int* status, int options)
{
    const pid_t got = __real_waitpid(pid, status, options);
    if (got == pid && pid == fail_wait_pid) {
        fail_wait_pid = 0;
        *status = 0; // WIFEXITED: not the SIGTRAP stop the caller expects.
    }
    return got;
}
extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }
// Linker wrappers count the actual injected heap operations, following the
// mapping gate's convention. A capture/restore that reuses a published heap
// endpoint must inject no brk at all; anything else proves a real query ran.
static unsigned injected_brk_questions = 0;
static unsigned injected_brk_moves = 0;
extern "C" uint64_t __real__Z17tracee_do_syscalliimmmmmm(
    int, int, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
extern "C" uint64_t __wrap__Z17tracee_do_syscalliimmmmmm(
    int pid, int number, uint64_t rdi, uint64_t rsi, uint64_t rdx,
    uint64_t r10, uint64_t r8, uint64_t r9)
{
    if (number == SYS_brk) {
        if (rdi == 0) ++injected_brk_questions;
        else ++injected_brk_moves;
    }
    return __real__Z17tracee_do_syscalliimmmmmm(pid, number, rdi, rsi, rdx, r10, r8, r9);
}
static void require(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
// Synthetic /proc/<pid>/maps support. Only the one designated maps path is
// wrapped; every other fopen (stores, fixtures) reaches the real libc. A cookie
// stream lets a test control short reads, EIO and the rewind used by the
// oversize fallback without changing product code or throwing through libc.
struct MapsCookie {
    std::string data;
    size_t position = 0;
    size_t fail_at = SIZE_MAX;
    bool fail_seek = false;
    bool fail_after_seek = false;
    bool armed_eio = false;
};
static std::string maps_override_path;
static std::string maps_override_data;
static size_t maps_override_fail_at = SIZE_MAX;
static bool maps_override_fail_seek = false;
static bool maps_override_fail_after_seek = false;
static unsigned maps_override_opens = 0;
static unsigned maps_override_reads = 0;
static unsigned long long maps_override_bytes = 0;

static ssize_t maps_cookie_read(void *cookie, char *buffer, size_t size)
{
    auto *stream = static_cast<MapsCookie *>(cookie);
    ++maps_override_reads;
    if (stream->armed_eio) { errno = EIO; return -1; }
    // The configured failure boundary is checked before EOF so fail_at ==
    // data.size() injects EIO exactly at the last valid byte, not a silent EOF.
    if (stream->position >= stream->fail_at) { errno = EIO; return -1; }
    if (stream->position >= stream->data.size()) return 0;
    const size_t end = std::min(stream->data.size(), stream->fail_at);
    const size_t count = std::min(size, end - stream->position);
    if (count == 0) { errno = EIO; return -1; }
    memcpy(buffer, stream->data.data() + stream->position, count);
    stream->position += count;
    maps_override_bytes += count;
    return static_cast<ssize_t>(count);
}
static ssize_t maps_cookie_write(void *, const char *, size_t)
{
    errno = EIO;
    return -1;
}
static int maps_cookie_seek(void *cookie, off64_t *offset, int whence)
{
    auto *stream = static_cast<MapsCookie *>(cookie);
    if (stream->fail_seek) { errno = ESPIPE; return -1; }
    off64_t base = 0;
    if (whence == SEEK_SET) base = 0;
    else if (whence == SEEK_CUR) base = static_cast<off64_t>(stream->position);
    else if (whence == SEEK_END) base = static_cast<off64_t>(stream->data.size());
    else { errno = EINVAL; return -1; }
    const off64_t next = base + *offset;
    if (next < 0) { errno = EINVAL; return -1; }
    stream->position = static_cast<size_t>(next);
    if (stream->fail_after_seek) stream->armed_eio = true;
    *offset = next;
    return 0;
}
static int maps_cookie_close(void *cookie)
{
    delete static_cast<MapsCookie *>(cookie);
    return 0;
}
static FILE *open_maps_cookie()
{
    MapsCookie *stream = nullptr;
    try {
        stream = new MapsCookie{maps_override_data, 0, maps_override_fail_at,
            maps_override_fail_seek, maps_override_fail_after_seek, false};
    } catch (...) {
        // __wrap_fopen sits on libc's fopencookie contract and must never let
        // an allocation failure escape as a C++ exception.
        errno = ENOMEM;
        return nullptr;
    }
    cookie_io_functions_t callbacks{};
    callbacks.read = maps_cookie_read;
    callbacks.write = maps_cookie_write;
    callbacks.seek = maps_cookie_seek;
    callbacks.close = maps_cookie_close;
    FILE *file = fopencookie(stream, "r", callbacks);
    if (!file) delete stream;
    return file;
}
extern "C" FILE *__real_fopen(const char *path, const char *mode);
extern "C" FILE *__wrap_fopen(const char *path, const char *mode)
{
    if (!maps_override_path.empty() && path && maps_override_path == path) {
        ++maps_override_opens;
        return open_maps_cookie();
    }
    return __real_fopen(path, mode);
}
struct MapsStreamScope {
    MapsStreamScope(pid_t pid, std::string data, size_t fail_at = SIZE_MAX,
                    bool fail_seek = false, bool fail_after_seek = false)
    {
        require(maps_override_path.empty(), "nested maps stream override");
        maps_override_path = "/proc/" + std::to_string(pid) + "/maps";
        maps_override_data = std::move(data);
        maps_override_fail_at = fail_at;
        maps_override_fail_seek = fail_seek;
        maps_override_fail_after_seek = fail_after_seek;
        maps_override_opens = maps_override_reads = 0;
        maps_override_bytes = 0;
    }
    ~MapsStreamScope() { maps_override_path.clear(); }
    MapsStreamScope(const MapsStreamScope &) = delete;
    MapsStreamScope &operator=(const MapsStreamScope &) = delete;
};
static std::string read_maps_file(pid_t pid)
{
    const std::string path = "/proc/" + std::to_string(pid) + "/maps";
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("open real maps fixture");
    std::string text;
    char buffer[4096];
    for (;;) {
        const ssize_t got = ::read(fd, buffer, sizeof(buffer));
        if (got < 0) { ::close(fd); throw std::runtime_error("read real maps fixture"); }
        if (got == 0) break;
        text.append(buffer, static_cast<size_t>(got));
    }
    ::close(fd);
    return text;
}
static const char *maps_padding_prefix()
{
    return "ffffffffff600000-ffffffffff601000 r-xp 1 0:0 0 ";
}
static std::string excluded_padding_row(size_t extra_name)
{
    return std::string(maps_padding_prefix()) + "[vvar" +
        std::string(extra_name, 'x') + "\n";
}
static std::string pad_maps_to(std::string base, size_t target)
{
    if (base.empty() || base.back() != '\n') base.push_back('\n');
    require(base.size() < target, "maps padding target below base");
    const size_t marker = strlen(maps_padding_prefix());
    const size_t minimum = marker + 5 + 1;
    const size_t maximum = marker + 5 + 504 + 1;
    const size_t fixed = marker + 5 + 1 + 1;
    size_t remaining = target - base.size();
    std::string padding;
    while (remaining > maximum) {
        padding += excluded_padding_row(1);
        remaining -= fixed;
    }
    require(remaining >= minimum && remaining <= maximum,
            "exact maps padding unreachable");
    padding += excluded_padding_row(remaining - minimum);
    return base + padding;
}
struct TestMapRow {
    uint64_t start = 0;
    uint64_t end = 0;
    std::string flags;
    std::string offset;
    std::string dev;
    std::string inode;
    std::string name;
};
static TestMapRow parse_test_row(const std::string &line)
{
    std::istringstream input(line);
    std::string range;
    TestMapRow row;
    input >> range >> row.flags >> row.offset >> row.dev >> row.inode;
    std::getline(input, row.name);
    const size_t first = row.name.find_first_not_of(" \t");
    row.name = first == std::string::npos ? "" : row.name.substr(first);
    const size_t dash = range.find('-');
    require(dash != std::string::npos && row.flags.size() >= 4 &&
            row.inode.size() != 0, "fixture map fields");
    row.start = std::stoull(range.substr(0, dash), nullptr, 16);
    row.end = std::stoull(range.substr(dash + 1), nullptr, 16);
    require(row.start < row.end, "fixture map range");
    return row;
}
static std::string format_test_row(uint64_t start, uint64_t end,
    const std::string &flags, const std::string &offset, const std::string &dev,
    const std::string &inode, const std::string &name)
{
    char range[64];
    const int written = snprintf(range, sizeof(range), "%" PRIx64 "-%" PRIx64,
                                 start, end);
    require(written > 0 && static_cast<size_t>(written) < sizeof(range),
            "fixture map range format");
    return std::string(range) + " " + flags + " " + offset + " " + dev + " " +
        inode + " " + name + "\n";
}
static std::string hex_field(uint64_t value)
{
    char buffer[32];
    const int written = snprintf(buffer, sizeof(buffer), "%" PRIx64, value);
    require(written > 0 && static_cast<size_t>(written) < sizeof(buffer),
            "fixture hex field");
    return buffer;
}
template <typename Predicate, typename Rewrite>
static std::string rewrite_first_line(std::string text, Predicate predicate,
                                      Rewrite rewrite)
{
    size_t begin = 0;
    while (begin < text.size()) {
        const size_t end = text.find('\n', begin);
        require(end != std::string::npos, "fixture line newline");
        const TestMapRow row = parse_test_row(text.substr(begin, end - begin + 1));
        if (predicate(row)) {
            text.replace(begin, end - begin + 1, rewrite(row));
            return text;
        }
        begin = end + 1;
    }
    throw std::runtime_error("fixture line predicate not found");
}
static std::string change_stack_flags(std::string text, const std::string &flags)
{
    return rewrite_first_line(std::move(text),
        [](const TestMapRow &row) { return row.name == "[stack]"; },
        [&](const TestMapRow &row) {
            return format_test_row(row.start, row.end, flags, row.offset,
                                   row.dev, row.inode, row.name);
        });
}
static std::string rename_stack(std::string text, const std::string &name)
{
    return rewrite_first_line(std::move(text),
        [](const TestMapRow &row) { return row.name == "[stack]"; },
        [&](const TestMapRow &row) {
            return format_test_row(row.start, row.end, row.flags, row.offset,
                                   row.dev, row.inode, name);
        });
}
static std::string change_file_backing(std::string text)
{
    return rewrite_first_line(std::move(text),
        [](const TestMapRow &row) {
            return row.inode != "0" && row.flags.size() >= 4 &&
                row.flags[0] == 'r' && row.flags[3] == 'p';
        },
        [](const TestMapRow &row) {
            const uint64_t offset = std::stoull(row.offset, nullptr, 16) + 0x1000;
            return format_test_row(row.start, row.end, row.flags,
                                   hex_field(offset), row.dev, row.inode, row.name);
        });
}
struct SplitFixture {
    std::string single;
    std::string split;
};
static SplitFixture make_split_fixture(const std::string &base)
{
    const std::string name = "fixture-" + std::string(480, 'x');
    size_t best_begin = 0;
    size_t best_end = 0;
    uint64_t best_size = 0;
    TestMapRow best{};
    size_t begin = 0;
    while (begin < base.size()) {
        const size_t end = base.find('\n', begin);
        require(end != std::string::npos, "real maps line");
        const TestMapRow row = parse_test_row(base.substr(begin, end - begin + 1));
        const bool excluded = row.name.rfind("[vvar", 0) == 0 ||
            row.name == "[vdso]" || row.name == "[vsyscall]";
        const bool scratch_overlap = row.start <= scratch_page &&
            scratch_page < row.end;
        if (!excluded && !scratch_overlap && row.flags[0] == 'r' &&
            row.flags[3] == 'p') {
            const uint64_t size = row.end - row.start;
            if (size > best_size) {
                best_size = size;
                best_begin = begin;
                best_end = end + 1;
                best = row;
            }
        }
        begin = end + 1;
    }
    require(best_size >= 64 * 1024, "no large private mapping for split fixture");
    const std::string single_row = format_test_row(best.start, best.end,
        best.flags, best.offset, best.dev, best.inode, name);
    std::string single = base;
    single.replace(best_begin, best_end - best_begin, single_row);
    require(single.size() <= 64 * 1024, "single split fixture exceeds raw cap");

    const uint64_t chunk = std::max<uint64_t>(512, best_size / 200);
    std::string rows;
    for (uint64_t start = best.start; start < best.end; start += chunk) {
        const uint64_t end = std::min<uint64_t>(best.end, start + chunk);
        std::string offset = best.offset;
        if (best.inode != "0") {
            offset = hex_field(std::stoull(best.offset, nullptr, 16) +
                               (start - best.start));
        }
        rows += format_test_row(start, end, best.flags, offset, best.dev,
                                best.inode, name);
    }
    std::string split = base;
    split.replace(best_begin, best_end - best_begin, rows);
    require(split.size() > 64 * 1024, "split fixture did not exceed raw cap");
    return {std::move(single), std::move(split)};
}
struct EquivalentPartitions {
    std::string merged;
    std::string split;
};
static EquivalentPartitions make_equivalent_partitions(const std::string &base)
{
    size_t target_begin = 0;
    size_t target_end = 0;
    TestMapRow target{};
    bool found = false;
    size_t begin = 0;
    while (begin < base.size()) {
        const size_t end = base.find('\n', begin);
        require(end != std::string::npos, "real maps line");
        const TestMapRow row = parse_test_row(base.substr(begin, end - begin + 1));
        const bool excluded = row.name.rfind("[vvar", 0) == 0 ||
            row.name == "[vdso]" || row.name == "[vsyscall]";
        const bool scratch_overlap = row.start <= scratch_page &&
            scratch_page < row.end;
        const uint64_t size = row.end - row.start;
        if (!found && !excluded && !scratch_overlap && row.flags[0] == 'r' &&
            row.flags[3] == 'p' && size >= 8192) {
            target = row;
            target_begin = begin;
            target_end = end + 1;
            found = true;
        }
        begin = end + 1;
    }
    require(found, "no splittable private mapping for alias fixture");
    const uint64_t midpoint = target.start +
        (((target.end - target.start) / 2) & ~uint64_t(4095));
    require(midpoint > target.start && midpoint < target.end,
            "fixture split midpoint");
    std::string second_offset = target.offset;
    if (target.inode != "0") {
        second_offset = hex_field(std::stoull(target.offset, nullptr, 16) +
                                  (midpoint - target.start));
    }
    const std::string rows =
        format_test_row(target.start, midpoint, target.flags, target.offset,
                        target.dev, target.inode, target.name) +
        format_test_row(midpoint, target.end, target.flags, second_offset,
                        target.dev, target.inode, target.name);
    std::string split = base;
    split.replace(target_begin, target_end - target_begin, rows);
    return {base, std::move(split)};
}
static std::string whitespace_variant(const std::string &base)
{
    const size_t first_space = base.find(' ');
    require(first_space != std::string::npos, "real maps whitespace");
    std::string variant = base;
    variant.insert(first_space, "\t");
    return variant;
}
// One real guest syscall through the production engine. do_single_syscall owns
// the user->entry and (suppressed) entry->exit resumes, so the unprotected
// resume under test is the engine's, not a harness reimplementation. Register
// setup and restoration only place the guest at the scratch syscall.
static uint64_t native_guest_syscall(pid_t pid, int cursor, int number, uint64_t argument)
{
    user_regs_struct saved{}, injected{};
    ptrace_right(PTRACE_GETREGS, pid, nullptr, &saved);
    injected = saved;
    injected.rip = scratch_page + 0xff0;
    injected.orig_rax = number;
    injected.rax = number;
    injected.rdi = argument;
    injected.rsi = injected.rdx = injected.r10 = injected.r8 = injected.r9 = 0;
    ptrace_right(PTRACE_SETREGS, pid, nullptr, &injected);
    const int previous_cursor = ptmc_state.cursor;
    ptmc_state.cursor = cursor;
    syscall_info info{};
    ExecutionEngine engine;
    const int decision = engine.execute_syscall(pid, info);
    ptmc_state.cursor = previous_cursor;
    ptrace_right(PTRACE_SETREGS, pid, nullptr, &saved);
    require(info.nr == number, "engine did not classify the native syscall");
    require(decision != CKPT_STOP, "engine stopped on the native syscall");
    return info.rval;
}
struct CaptureLayoutMode {
    explicit CaptureLayoutMode(const char *mode) {
        if (mode) setenv("DETSIM_CAPTURE_LAYOUT_CACHE", mode, 1);
        else unsetenv("DETSIM_CAPTURE_LAYOUT_CACHE");
    }
    ~CaptureLayoutMode() { unsetenv("DETSIM_CAPTURE_LAYOUT_CACHE"); }
};
static detsim::CaptureLayoutCacheStats capture_layout_stats() {
    return detsim::capture_layout_cache_stats();
}
struct CaptureOutcome {
    sys_state state;
    detsim::CaptureLayoutCacheStats stats;
};
int main(int argc, char**)
{
    pid_t children[NP]{};
    char directory[] = "/tmp/detsim-capture-XXXXXX";
    const auto cwd = std::filesystem::current_path();
    try {
        require(mkdtemp(directory) && chdir(directory) == 0, "temporary directory");
        // Fork before the stores start writer threads. Tiny caches force disk reloads.
        for (int i = 0; i < NP; ++i) {
            children[i] = fork(); require(children[i] >= 0, "fork");
            if (!children[i]) {
                // Fork inherits libc's rseq area; injected syscalls otherwise
                // let the kernel change its CPU fields between oracle captures.
                if (__rseq_size && syscall(SYS_rseq,
                    static_cast<char*>(__builtin_thread_pointer()) + __rseq_offset,
                    32, RSEQ_FLAG_UNREGISTER, RSEQ_SIG)) _exit(5);
                if (mmap(reinterpret_cast<void*>(scratch_page), 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED) _exit(2);
                auto *code = reinterpret_cast<unsigned char*>(scratch_page + 0xff0);
                code[0] = 0x0f; code[1] = 0x05;
                if (syscall(SYS_set_robust_list, nullptr, 24)) _exit(91);
                if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr)) _exit(3);
                asm volatile("int3");
                _exit(0);
            }
            int status;
            require(waitpid(children[i], &status, 0) == children[i] && WIFSTOPPED(status), "child stop");
            require(ptrace(PTRACE_SETOPTIONS, children[i], nullptr,
                           PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL) == 0, "ptrace options");
            ptmc_state.pids[i] = children[i];
            ptmc_state.fd_managers[i] = std::make_shared<FdManager>();
            ptmc_state.fs_states[i].set_fd_manager(ptmc_state.fd_managers[i]);
            ptmc_state.sock_states[i].set_fd_manager(ptmc_state.fd_managers[i]);
            adopt_clear_child_tid(i,children[i]);
        }
        StateStore::Config config;
        if (argc > 1) config.hot_cache_size = config.warm_cache_size = 1;
        StateStore::instance().init(config);
        syscall_info info[NP][MAX_THREADS_PER_PROCESS]{};
        ptmc_state.cursor = -1;
        auto_mode = 1;
        // Use actual allocated descriptors: resource queues may never exist
        // outside the captured process descriptor namespace.
        ptmc_state.addrs[0] = "127.0.0.1"; ptmc_state.addrs[1] = "127.0.0.2";
        ptmc_state.cursor = 1;
        const int udp_fd = ptmc_state.sock_states[1].do_socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
        const int listener_fd = ptmc_state.sock_states[1].do_socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        require(udp_fd >= 0 && listener_fd >= 0, "allocate fixture sockets");
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET; endpoint.sin_port = htons(23117);
        require(inet_pton(AF_INET, "127.0.0.2", &endpoint.sin_addr) == 1, "listener address");
        void* guest_address = reinterpret_cast<void*>(scratch_page);
        require(guest_write_exact(children[1], guest_address, &endpoint, sizeof(endpoint)), "write listener address");
        require(ptmc_state.sock_states[1].do_bind(listener_fd, static_cast<sockaddr*>(guest_address), sizeof(endpoint)) == 0 &&
                ptmc_state.sock_states[1].do_listen(listener_fd, 2) == 0, "create listener");
        int pipe_ends[2];
        require(ptmc_state.fs_states[1].do_pipe(pipe_ends, O_NONBLOCK | O_CLOEXEC) == 0, "create saved pipe");
        const int epfd = ptmc_state.sock_states[1].do_epoll_create1(EPOLL_CLOEXEC);
        epoll_event watch{}; watch.events = EPOLLIN; watch.data.u64 = 0xfedcba9876543210ULL;
        require(guest_write_exact(children[1], guest_address, &watch, sizeof(watch)), "write epoll fixture");
        require(ptmc_state.sock_states[1].do_epoll_ctl(epfd, EPOLL_CTL_ADD, pipe_ends[0], static_cast<epoll_event*>(guest_address)) == 0,
                "watch saved pipe");
        const char pipe_payload[] = "pipe branch";
        require(guest_write_exact(children[1], guest_address, pipe_payload, sizeof(pipe_payload)), "write pipe fixture");
        require(ptmc_state.fs_states[1].do_pipe_write(pipe_ends[1], guest_address, sizeof(pipe_payload)) == sizeof(pipe_payload),
                "buffer saved pipe");
        ptmc_state.cursor = 0;
        const int client_fd = ptmc_state.sock_states[0].do_socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        require(guest_write_exact(children[0], guest_address, &endpoint, sizeof(endpoint)), "write client address");
        require(ptmc_state.sock_states[0].do_connect(client_fd, static_cast<sockaddr*>(guest_address), sizeof(endpoint)) == 0,
                "create preaccept connection");
        const char tcp_payload[] = "pending branch";
        require(guest_write_exact(children[0], guest_address, tcp_payload, sizeof(tcp_payload)), "write TCP fixture");
        require(ptmc_state.sock_states[0].do_send(client_fd, guest_address, sizeof(tcp_payload), 0) == sizeof(tcp_payload),
                "buffer preaccept TCP");
        const uint64_t provenance_page=tracee_do_syscall(children[1],SYS_mmap,0,4096,
            PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,uint64_t(-1),0);
        require(provenance_page>0 && provenance_page<uint64_t(-4095),"provenance fixture mapping");
        const int provenance_fd=ptmc_state.fs_states[1].do_open("/provenance",O_CREAT|O_RDWR,0600);
        require(provenance_fd>=0 && ptmc_state.fs_states[1].do_write(provenance_fd,std::string(4096,0).data(),4096)==4096,
                "provenance source file");
        require(ptmc_state.fs_states[1].do_close(provenance_fd)==0,"close provenance source fixture");
        std::array<ProcessKernelState,NP> initial_process_states;
        for (int i=0;i<NP;++i) {
            ptmc_state.process_states[i].random_state^=uint64_t(i+123);
            ptmc_state.process_states[i].signal_actions[SIGUSR1]={uint64_t(SIG_IGN),0,0,0};
            initial_process_states[i]=ptmc_state.process_states[i];
        }
        ptmc_state.cursor = -1;
        sys_state source(info);
        ptmc_state.running_state = source;
        ptmc_state.cursor = 0;
        ptmc_state.running_state.monitor_states["other-branch"] = {9};
        source.recover_running_state();
        require(ptmc_state.running_state.monitor_states == source.monitor_states &&
                ptmc_state.running_state.ss_hash == source.ss_hash,
                "restore did not select the source monitor/product identity");
        const auto before = StateStore::instance().get_stats();
        sys_state reused(info, &source);
        require(StateStore::instance().get_stats().save_calls == before.save_calls + NP,
                "capture skipped ordinary store validation");
        if (argc == 1) require(StateStore::instance().get_stats().save_reference_hits == before.save_reference_hits + 1,
                "unchanged resident inactive snapshot did not bypass hashing");
        require(reused.ss_hash == source.ss_hash, "unchanged capture identity drift");
        auto compare_full = [&](const sys_state& optimized) {
            const char* backend_value = getenv("DETSIM_SNAPSHOT_BACKEND");
            const bool had_backend = backend_value != nullptr;
            const std::string selected = had_backend ? backend_value : "full";
            const char* layout_cache_value = getenv("DETSIM_CAPTURE_LAYOUT_CACHE");
            const bool had_layout_cache = layout_cache_value != nullptr;
            const std::string saved_layout_cache =
                had_layout_cache ? layout_cache_value : "";
            setenv("DETSIM_SNAPSHOT_BACKEND", "full", 1);
            setenv("DETSIM_CAPTURE_MODE", "full", 1);
            unsetenv("DETSIM_CAPTURE_LAYOUT_CACHE");
            sys_state full(info, &source);
            unsetenv("DETSIM_CAPTURE_MODE");
            if (had_backend) setenv("DETSIM_SNAPSHOT_BACKEND", selected.c_str(), 1);
            else unsetenv("DETSIM_SNAPSHOT_BACKEND");
            if (had_layout_cache)
                setenv("DETSIM_CAPTURE_LAYOUT_CACHE", saved_layout_cache.c_str(), 1);
            else unsetenv("DETSIM_CAPTURE_LAYOUT_CACHE");
            if (selected == "full")
                require(full.ss_hash == optimized.ss_hash, "full capture system identity differs");
            for (int i = 0; i < NP; ++i) {
                const auto expected = SnapshotView::open(full.ts_hash[i]);
                const auto actual = SnapshotView::open(optimized.ts_hash[i]);
                require(expected->maps_text() == actual->maps_text() &&
                    expected->metadata().size() == actual->metadata().size() &&
                    !memcmp(expected->metadata().data(), actual->metadata().data(), expected->metadata().size()) &&
                    !memcmp(&expected->main_regs(), &actual->main_regs(), sizeof(user_regs_struct)),
                    "decoded snapshot metadata differs");
                require(expected->regions().size() == actual->regions().size(), "region count differs");
                for (size_t region = 0; region < expected->regions().size(); ++region) {
                    const auto& e = expected->regions()[region];
                    const auto& a = actual->regions()[region];
                    require(e.start == a.start && e.end == a.end && e.prot == a.prot &&
                        !memcmp(e.flags, a.flags, sizeof(e.flags)), "decoded regions differ");
                    std::vector<uint8_t> ebytes(e.end-e.start), abytes(ebytes.size());
                    expected->read_into(e.start, ebytes.data(), ebytes.size());
                    actual->read_into(a.start, abytes.data(), abytes.size());
                    require(ebytes == abytes, "decoded snapshot memory differs");
                }
            }
        };
        compare_full(reused);
        // Opt-in capture-layout cache. The cache memoizes only text-derived
        // layouts, so off/on/verify must all preserve the full byte oracle.
        detsim::clear_capture_layout_cache();
        {
            CaptureLayoutMode off_mode("off");
            sys_state off_capture(info, &source);
            require(off_capture.ss_hash == source.ss_hash,
                    "off layout cache changed unchanged identity");
            const auto off_stats = capture_layout_stats();
            require(off_stats.lookups == 0 && off_stats.builds == 0 &&
                    off_stats.entries == 0,
                    "off layout cache was consulted or populated");
            compare_full(off_capture);
        }
        detsim::clear_capture_layout_cache();
        {
            detsim::resident::Scope controlled;
            source.recover_running_state();
            CaptureLayoutMode on_mode("on");
            sys_state first(info, &source);
            const auto first_stats = capture_layout_stats();
            require(first_stats.hits == 0, "empty layout cache reported a hit");
            require(first_stats.builds == NP, "initial cached capture did not build each layout");
            require(first_stats.admits >= 1 && first_stats.entries >= 1,
                    "initial cached capture admitted no layout");
            require(first.ss_hash == source.ss_hash,
                    "layout cache changed unchanged capture identity");
            compare_full(first);

            sys_state second(info, &source);
            const auto second_stats = capture_layout_stats();
            require(second_stats.lookups == first_stats.lookups + NP &&
                    second_stats.hits == first_stats.hits + NP &&
                    second_stats.builds == first_stats.builds,
                    "unchanged resident capture did not reuse cached layout");
            require(second.ss_hash == first.ss_hash, "cached layout changed identity");
            compare_full(second);

            // A warmed raw alias must survive loss of the resident proof for an
            // unchanged stopped target, and the on path must still read the
            // designated maps file to EOF before consulting it.
            {
                // Resident admissions carry no live alias, so warm the raw
                // aliases with a no-resident capture first.
                detsim::resident::invalidate_all();
                const auto before_warm = capture_layout_stats();
                sys_state warmed(info, &source);
                const auto warmed_stats = capture_layout_stats();
                require(warmed.ss_hash == second.ss_hash,
                        "raw alias warm changed unchanged identity");
                require(warmed_stats.builds == before_warm.builds + NP,
                        "raw alias warm did not build every layout");
                require(warmed_stats.hits == before_warm.hits,
                        "raw alias warm unexpectedly hit");
                const std::string live_maps = read_maps_file(children[1]);
                const auto before_refresh = capture_layout_stats();
                detsim::resident::invalidate_all();
                MapsStreamScope stream(children[1], live_maps);
                sys_state refreshed(info, &source);
                const auto refreshed_stats = capture_layout_stats();
                require(refreshed.ss_hash == second.ss_hash,
                        "raw alias refresh changed unchanged identity");
                require(refreshed_stats.hits == before_refresh.hits + NP,
                        "warmed raw alias did not hit after resident invalidation");
                require(refreshed_stats.builds == before_refresh.builds,
                        "warmed raw alias rebuilt a layout");
                require(refreshed_stats.entries == before_refresh.entries,
                        "raw alias hit changed the cache entry count");
                require(maps_override_opens >= 1 &&
                        maps_override_bytes == live_maps.size(),
                        "on mode did not freshly read the designated maps file");
                compare_full(refreshed);
            }

            const auto mapped = tracee_do_syscall(children[1], SYS_mmap, 0, 4096,
                PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, uint64_t(-1), 0);
            require(mapped > 0 && mapped < uint64_t(-4095), "layout cache fixture mapping");
            require(!detsim::resident::get(children[1]).snapshot,
                    "mapping injection retained resident proof");
            const auto before_changed = capture_layout_stats();
            sys_state changed(info, &source);
            const auto changed_stats = capture_layout_stats();
            require(changed.ts_hash[1] != source.ts_hash[1],
                    "mapping change absent from cached identity");
            require(changed_stats.hits == before_changed.hits + 1,
                    "unchanged peer did not hit cached layout");
            require(changed_stats.builds == before_changed.builds + 1,
                    "changed mapping did not rebuild exactly one layout");
            require(changed_stats.admits == before_changed.admits + 1,
                    "changed mapping was not admitted");
            compare_full(changed);

            const auto before_reuse = capture_layout_stats();
            sys_state changed_reused(info, &source);
            const auto reused_stats = capture_layout_stats();
            require(changed_reused.ss_hash == changed.ss_hash,
                    "changed branch reuse identity drift");
            require(reused_stats.hits == before_reuse.hits + NP,
                    "changed branch did not hit after publication");

            ptmc_state.running_state = changed;
            source.recover_running_state();
            const auto before_restored = capture_layout_stats();
            sys_state restored(info, &source);
            const auto restored_stats = capture_layout_stats();
            require(restored.ss_hash == source.ss_hash,
                    "layout cache restore identity drift");
            require((restored_stats.hits - before_restored.hits) +
                    (restored_stats.builds - before_restored.builds) == NP,
                    "restore neither reused nor rebuilt each saved layout");
            compare_full(restored);
            const auto before_restored_reuse = capture_layout_stats();
            sys_state restored_reused(info, &source);
            const auto restored_reuse_stats = capture_layout_stats();
            require(restored_reused.ss_hash == source.ss_hash,
                    "restored capture reused a stale layout");
            require(restored_reuse_stats.hits == before_restored_reuse.hits + NP,
                    "restored unchanged capture did not hit");

            const auto before_pressure = capture_layout_stats();
            for (int i = 0; i < 5; ++i) {
                // Fixed addresses guarantee five distinct canonical layouts even
                // after the earlier branch mapping was restored and unmapped.
                const uint64_t pressure_address =
                    0x500000000000ull + uint64_t(i) * 0x100000ull;
                const auto extra = tracee_do_syscall(children[1], SYS_mmap,
                    pressure_address, 4096, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                    uint64_t(-1), 0);
                require(extra > 0 && extra < uint64_t(-4095),
                        "layout cache pressure mapping");
                sys_state pressure(info, &source);
                require(pressure.ts_hash[1] != source.ts_hash[1],
                        "pressure mapping absent from cached identity");
            }
            const auto pressure_stats = capture_layout_stats();
            require(pressure_stats.admits == before_pressure.admits + 5,
                    "pressure mappings were not all admitted");
            require(pressure_stats.evictions >= 1,
                    "layout cache never evicted under pressure");
            require(pressure_stats.entries <= 4, "layout cache exceeded its entry bound");
            // Entries are MRU-first and eviction removes the LRU tail, so the
            // layouts currently backing resident snapshots must survive.
            const auto before_pressure_reuse = capture_layout_stats();
            sys_state pressure_reused(info, &source);
            const auto pressure_reuse_stats = capture_layout_stats();
            require(pressure_reused.ts_hash[1] != source.ts_hash[1],
                    "pressure reuse lost the latest mapping");
            require(pressure_reuse_stats.hits == before_pressure_reuse.hits + NP &&
                    pressure_reuse_stats.builds == before_pressure_reuse.builds,
                    "layout cache evicted a currently reused layout");

            source.recover_running_state();
            // Force the live /proc path so verify must rebuild from canonical
            // text and prove the builder is idempotent, not merely hard-coded.
            detsim::clear_capture_layout_cache();
            detsim::resident::invalidate_all();
            {
                CaptureLayoutMode verify_mode("verify");
                sys_state verify_original(info, &source);
                const auto seeded_stats = capture_layout_stats();
                require(verify_original.ss_hash == source.ss_hash,
                        "verify mode changed restored identity");
                require(seeded_stats.lookups == NP && seeded_stats.builds == NP,
                        "verify seed did not miss raw text and rebuild each layout");
                require(seeded_stats.admits >= 1,
                        "verify seed did not admit canonical layout");
                const auto before_verify = capture_layout_stats();
                sys_state verified(info, &source);
                const auto verify_stats = capture_layout_stats();
                require(verified.ss_hash == verify_original.ss_hash,
                        "verify mode changed reused identity");
                require(verify_stats.lookups == before_verify.lookups + NP &&
                        verify_stats.hits == before_verify.hits + NP,
                        "verify mode did not hit both cached layouts");
                require(verify_stats.builds == before_verify.builds + NP,
                        "verify mode reused without rebuilding");
                require(verify_stats.verify_checks ==
                        before_verify.verify_checks + NP &&
                        verify_stats.verify_mismatches == 0,
                        "canonical text did not rebuild the original layout");
                compare_full(verified);

                // Losing the resident proof must fall back to the raw aliases
                // admitted when verify seeded the cache, and verify must still
                // rebuild rather than reuse.
                detsim::resident::invalidate_all();
                const auto before_raw_verify = capture_layout_stats();
                sys_state raw_verified(info, &source);
                const auto raw_verify_stats = capture_layout_stats();
                require(raw_verified.ss_hash == verify_original.ss_hash,
                        "raw verify changed restored identity");
                require(raw_verify_stats.hits == before_raw_verify.hits + NP &&
                        raw_verify_stats.builds == before_raw_verify.builds + NP,
                        "raw verify did not hit the active aliases and rebuild");
                require(raw_verify_stats.verify_checks ==
                        before_raw_verify.verify_checks + NP &&
                        raw_verify_stats.verify_mismatches == 0,
                        "raw verify did not rebuild and compare every layout");
            }

            {
                CaptureLayoutMode invalid("bogus");
                bool rejected = false;
                try { sys_state bad(info, &source); }
                catch (const std::runtime_error& error) {
                    rejected = std::string(error.what()) ==
                        "DETSIM_CAPTURE_LAYOUT_CACHE must be off, on or verify";
                }
                require(rejected, "invalid layout cache mode was not rejected");
            }
            source.recover_running_state();
        }
        detsim::clear_capture_layout_cache();
        {
            CaptureLayoutMode default_off(nullptr);
            sys_state default_off_capture(info, &source);
            require(default_off_capture.ss_hash == source.ss_hash,
                    "default layout cache mode changed identity");
            const auto default_stats = capture_layout_stats();
            require(default_stats.lookups == 0 && default_stats.builds == 0 &&
                    default_stats.entries == 0,
                    "default layout cache mode was not off");
        }
        // Synthetic maps streams exercise reader boundaries, fallback, parser
        // equivalence, admission and alias replacement. Off mode is the
        // independent oracle for every accepted or rejected synthetic text.
        detsim::clear_capture_layout_cache();
        {
            detsim::resident::Scope controlled;
            source.recover_running_state();
            const std::string base = read_maps_file(children[0]);
            auto capture_off = [&](pid_t target, const std::string& maps) {
                detsim::resident::invalidate_all();
                MapsStreamScope stream(target, maps);
                CaptureLayoutMode mode("off");
                sys_state state(info, &source);
                return CaptureOutcome{std::move(state), capture_layout_stats()};
            };
            auto capture_on = [&](pid_t target, const std::string& maps,
                                  bool clear) {
                detsim::resident::invalidate_all();
                if (clear) detsim::clear_capture_layout_cache();
                MapsStreamScope stream(target, maps);
                CaptureLayoutMode mode("on");
                sys_state state(info, &source);
                return CaptureOutcome{std::move(state), capture_layout_stats()};
            };
            auto capture_cached = [&](pid_t target, const std::string& maps) {
                MapsStreamScope stream(target, maps);
                CaptureLayoutMode mode("on");
                sys_state state(info, &source);
                return CaptureOutcome{std::move(state), capture_layout_stats()};
            };
            auto failure_of = [](auto&& operation) -> std::string {
                try { operation(); }
                catch (const std::runtime_error& error) { return error.what(); }
                return "";
            };
            auto saved_successors = [&]() {
                return StateStore::instance().get_stats().save_calls;
            };

            const std::string maps_65536 = pad_maps_to(base, 64 * 1024);
            require(maps_65536.size() == 64 * 1024, "65536 maps fixture size");
            const auto off_65536 = capture_off(children[0], maps_65536);
            const auto on_65536 = capture_on(children[0], maps_65536, true);
            require(on_65536.state.ss_hash == off_65536.state.ss_hash,
                    "65536-byte raw maps changed identity");
            require(on_65536.stats.oversize == 0,
                    "65536-byte raw maps were treated as oversized");
            require(on_65536.stats.builds == NP,
                    "65536-byte raw maps did not build exactly once");
            // compare_full cannot see a synthetic resident proof: full capture
            // mode rejects resident images and reads live maps. Off mode is the
            // independent oracle for this synthetic text.

            const auto reuse_65536 = capture_on(children[0], maps_65536, false);
            require(reuse_65536.state.ss_hash == on_65536.state.ss_hash,
                    "65536-byte raw alias reuse changed identity");
            require(reuse_65536.stats.hits == on_65536.stats.hits + NP &&
                    reuse_65536.stats.builds == on_65536.stats.builds,
                    "65536-byte raw alias did not hit without rebuilding");
            const auto cached_65536 = capture_cached(children[0], maps_65536);
            require(cached_65536.state.ss_hash == on_65536.state.ss_hash,
                    "canonical hit after raw reuse changed identity");
            require(cached_65536.stats.hits == reuse_65536.stats.hits + NP &&
                    cached_65536.stats.builds == reuse_65536.stats.builds,
                    "canonical hit after raw reuse rebuilt a layout");

            {
                // Two equivalent raw partitions must share one canonical entry.
                // Re-observing either partition replaces the one alias, never
                // allocates another slot, and both directions must hit.
                const auto parts = make_equivalent_partitions(base);
                const auto seed_split = capture_on(children[0], parts.split, true);
                require(seed_split.stats.oversize == 0,
                        "small partition fixture was treated as oversized");
                const auto merged_miss = capture_on(children[0], parts.merged, false);
                require(merged_miss.state.ss_hash == seed_split.state.ss_hash,
                        "equivalent merged partition changed identity");
                require(merged_miss.stats.builds == seed_split.stats.builds + 1 &&
                        merged_miss.stats.hits == seed_split.stats.hits + 1,
                        "equivalent merged partition did not miss the raw alias");
                require(merged_miss.stats.admits == seed_split.stats.admits &&
                        merged_miss.stats.entries == seed_split.stats.entries,
                        "equivalent merged partition allocated a new slot");
                const auto merged_hit = capture_on(children[0], parts.merged, false);
                require(merged_hit.stats.builds == merged_miss.stats.builds &&
                        merged_hit.stats.hits == merged_miss.stats.hits + NP,
                        "replaced raw alias did not hit the merged partition");
                const auto split_miss = capture_on(children[0], parts.split, false);
                require(split_miss.state.ss_hash == seed_split.state.ss_hash,
                        "equivalent split partition changed identity");
                require(split_miss.stats.builds == merged_hit.stats.builds + 1 &&
                        split_miss.stats.hits == merged_hit.stats.hits + 1,
                        "equivalent split partition did not replace the alias");
                require(split_miss.stats.admits == merged_hit.stats.admits &&
                        split_miss.stats.entries == merged_hit.stats.entries,
                        "equivalent split partition allocated a new slot");
                const auto split_hit = capture_on(children[0], parts.split, false);
                require(split_hit.stats.builds == split_miss.stats.builds &&
                        split_hit.stats.hits == split_miss.stats.hits + NP,
                        "replaced raw alias did not hit the split partition");
            }
            {
                // Same canonical key, different raw whitespace. Re-observing
                // the second spelling must replace the first alias in place;
                // the old spelling then misses and the new one hits.
                const std::string maps_ws = whitespace_variant(base);
                require(maps_ws.size() != base.size(),
                        "whitespace variant did not change the raw text");
                const auto seed = capture_on(children[0], base, true);
                const auto ws_miss = capture_on(children[0], maps_ws, false);
                require(ws_miss.state.ss_hash == seed.state.ss_hash,
                        "whitespace variant changed identity");
                require(ws_miss.stats.builds == seed.stats.builds + 1 &&
                        ws_miss.stats.hits == seed.stats.hits + 1,
                        "whitespace variant did not miss the raw alias");
                require(ws_miss.stats.admits == seed.stats.admits &&
                        ws_miss.stats.entries == seed.stats.entries,
                        "whitespace variant allocated a new slot");
                const auto ws_hit = capture_on(children[0], maps_ws, false);
                require(ws_hit.stats.builds == ws_miss.stats.builds &&
                        ws_hit.stats.hits == ws_miss.stats.hits + NP,
                        "replaced whitespace alias did not hit");
                const auto base_miss = capture_on(children[0], base, false);
                require(base_miss.state.ss_hash == seed.state.ss_hash,
                        "old whitespace alias changed identity");
                require(base_miss.stats.builds == ws_hit.stats.builds + 1 &&
                        base_miss.stats.hits == ws_hit.stats.hits + 1,
                        "old alias did not miss after whitespace replacement");
                require(base_miss.stats.admits == ws_hit.stats.admits &&
                        base_miss.stats.entries == ws_hit.stats.entries,
                        "old alias replacement allocated a new slot");
                const auto base_hit = capture_on(children[0], base, false);
                require(base_hit.stats.builds == base_miss.stats.builds &&
                        base_hit.stats.hits == base_miss.stats.hits + NP,
                        "restored base alias did not hit");
            }
            {
                // An oversize partition can share the <=64 KiB canonical key
                // of a resident raw alias. Its canonical admission must keep
                // that alias rather than erase it.
                const SplitFixture equivalent = make_split_fixture(base);
                const auto seed_single = capture_on(children[0], equivalent.single, true);
                require(seed_single.stats.oversize == 0,
                        "single oversize fixture exceeded the raw cap");
                const auto single_hit = capture_on(children[0], equivalent.single, false);
                require(single_hit.stats.builds == seed_single.stats.builds &&
                        single_hit.stats.hits == seed_single.stats.hits + NP,
                        "single partition raw alias did not hit");
                const auto oversize = capture_on(children[0], equivalent.split, false);
                require(oversize.state.ss_hash == seed_single.state.ss_hash,
                        "equivalent oversize partition changed identity");
                require(oversize.stats.oversize >= 1,
                        "equivalent oversize partition was not reported oversized");
                require(oversize.stats.builds == single_hit.stats.builds + 1 &&
                        oversize.stats.hits == single_hit.stats.hits + 1,
                        "equivalent oversize partition did not rebuild exactly once");
                require(oversize.stats.admits == single_hit.stats.admits &&
                        oversize.stats.entries == single_hit.stats.entries,
                        "equivalent oversize admission allocated a new slot");
                const auto preserved = capture_on(children[0], equivalent.single, false);
                require(preserved.stats.builds == oversize.stats.builds &&
                        preserved.stats.hits == oversize.stats.hits + NP,
                        "oversize canonical admission dropped the existing raw alias");
            }

            const std::string maps_65535 = pad_maps_to(base, 64 * 1024 - 1);
            const auto seed_65535 = capture_on(children[0], maps_65535, true);
            require(seed_65535.stats.oversize == 0,
                    "65535-byte raw maps were treated as oversized");
            const auto reuse_65535 = capture_on(children[0], maps_65535, false);
            require(reuse_65535.state.ss_hash == seed_65535.state.ss_hash,
                    "65535-byte raw alias reuse changed identity");
            require(reuse_65535.stats.hits == seed_65535.stats.hits + NP &&
                    reuse_65535.stats.builds == seed_65535.stats.builds,
                    "65535-byte raw alias did not hit");

            const std::string maps_65537 = pad_maps_to(base, 64 * 1024 + 1);
            const auto off_65537 = capture_off(children[0], maps_65537);
            const auto seed_65537 = capture_on(children[0], maps_65537, true);
            require(seed_65537.state.ss_hash == off_65537.state.ss_hash,
                    "65537-byte fallback changed identity");
            require(seed_65537.stats.oversize >= 1,
                    "65537-byte raw maps were not reported oversized");
            require(seed_65537.stats.builds == NP,
                    "65537-byte fallback did not build exactly once");
            const auto reuse_65537 = capture_on(children[0], maps_65537, false);
            require(reuse_65537.state.ss_hash == seed_65537.state.ss_hash,
                    "65537-byte fallback reuse changed identity");
            require(reuse_65537.stats.builds == seed_65537.stats.builds + 1 &&
                    reuse_65537.stats.hits == seed_65537.stats.hits + 1,
                    "65537-byte raw maps reused a raw alias or disturbed the peer");

            const std::string maps_80k = pad_maps_to(base, 80 * 1024);
            const auto seed_80k = capture_on(children[0], maps_80k, true);
            require(seed_80k.stats.oversize >= 1,
                    "over-limit raw maps were not reported oversized");
            require(seed_80k.stats.builds == NP,
                    "over-limit fallback did not build exactly once");
            const auto reuse_80k = capture_on(children[0], maps_80k, false);
            require(reuse_80k.state.ss_hash == seed_80k.state.ss_hash,
                    "over-limit fallback reuse changed identity");
            require(reuse_80k.stats.builds == seed_80k.stats.builds + 1 &&
                    reuse_80k.stats.hits == seed_80k.stats.hits + 1,
                    "over-limit raw maps reused a raw alias or disturbed the peer");

            {
                detsim::resident::invalidate_all();
                MapsStreamScope stream(children[0], maps_80k, SIZE_MAX, true, false);
                CaptureLayoutMode mode("on");
                const auto before_stats = capture_layout_stats();
                const auto before_saves = saved_successors();
                const std::string error = failure_of([&] {
                    sys_state bad(info, &source);
                });
                require(error == "Failed to read tracee memory mappings",
                        "seek failure was not rejected as a maps read failure");
                require(saved_successors() == before_saves,
                        "seek failure saved a successor state");
                const auto after_stats = capture_layout_stats();
                require(after_stats.lookups == before_stats.lookups &&
                        after_stats.hits == before_stats.hits &&
                        after_stats.builds == before_stats.builds &&
                        after_stats.admits == before_stats.admits &&
                        after_stats.entries == before_stats.entries,
                        "seek failure consulted or mutated the layout cache");
            }
            {
                detsim::resident::invalidate_all();
                MapsStreamScope stream(children[0], maps_80k, SIZE_MAX, false, true);
                CaptureLayoutMode mode("on");
                const auto before_stats = capture_layout_stats();
                const auto before_saves = saved_successors();
                const std::string error = failure_of([&] {
                    sys_state bad(info, &source);
                });
                require(error == "Failed to read tracee memory mappings",
                        "fallback read failure was not rejected");
                require(saved_successors() == before_saves,
                        "fallback read failure saved a successor state");
                const auto after_stats = capture_layout_stats();
                require(after_stats.lookups == before_stats.lookups &&
                        after_stats.hits == before_stats.hits &&
                        after_stats.builds == before_stats.builds &&
                        after_stats.admits == before_stats.admits &&
                        after_stats.entries == before_stats.entries,
                        "fallback read failure consulted or mutated the layout cache");
            }
            {
                capture_on(children[0], base, true);
                require(detsim::resident::get(children[0]).snapshot != nullptr,
                        "resident proof absent before empty-read test");
                detsim::resident::invalidate_all();
                const auto before_stats = capture_layout_stats();
                const auto before_saves = saved_successors();
                MapsStreamScope stream(children[0], "");
                CaptureLayoutMode mode("on");
                const std::string error = failure_of([&] {
                    sys_state bad(info, &source);
                });
                require(error == "Failed to read tracee memory mappings",
                        "empty live maps was not rejected");
                require(saved_successors() == before_saves,
                        "empty live maps saved a successor state");
                const auto after_stats = capture_layout_stats();
                require(after_stats.lookups == before_stats.lookups &&
                        after_stats.hits == before_stats.hits &&
                        after_stats.builds == before_stats.builds &&
                        after_stats.admits == before_stats.admits &&
                        after_stats.entries == before_stats.entries,
                        "empty live maps was looked up");
                require(detsim::resident::get(children[0]).snapshot == nullptr,
                        "empty live maps published resident proof");
            }
            {
                detsim::resident::invalidate_all();
                const auto before_stats = capture_layout_stats();
                const auto before_saves = saved_successors();
                MapsStreamScope stream(children[0], base, 64);
                CaptureLayoutMode mode("on");
                const std::string error = failure_of([&] {
                    sys_state bad(info, &source);
                });
                require(error == "Failed to read tracee memory mappings",
                        "partial read with EIO was not rejected");
                require(saved_successors() == before_saves,
                        "partial read with EIO saved a successor state");
                const auto after_stats = capture_layout_stats();
                require(after_stats.lookups == before_stats.lookups &&
                        after_stats.hits == before_stats.hits &&
                        after_stats.builds == before_stats.builds &&
                        after_stats.admits == before_stats.admits &&
                        after_stats.entries == before_stats.entries,
                        "partial read with EIO hit the cache or built a layout");
            }
            {
                // The raw alias already describes exactly this text. EIO on
                // the EOF probe must still reject instead of treating the
                // valid prefix as a cache hit.
                detsim::resident::invalidate_all();
                const auto before_stats = capture_layout_stats();
                const auto before_saves = saved_successors();
                MapsStreamScope stream(children[0], base, base.size());
                CaptureLayoutMode mode("on");
                const std::string error = failure_of([&] {
                    sys_state bad(info, &source);
                });
                require(error == "Failed to read tracee memory mappings",
                        "cached text with EOF EIO was not rejected");
                require(saved_successors() == before_saves,
                        "cached text with EOF EIO saved a successor state");
                const auto after_stats = capture_layout_stats();
                require(after_stats.lookups == before_stats.lookups &&
                        after_stats.hits == before_stats.hits &&
                        after_stats.builds == before_stats.builds &&
                        after_stats.admits == before_stats.admits &&
                        after_stats.entries == before_stats.entries,
                        "cached text with EOF EIO hit the cache or built a layout");
                require(detsim::resident::get(children[0]).snapshot == nullptr,
                        "cached text with EOF EIO published resident proof");
            }
            {
                // A valid resident capture must not open or read the live maps
                // stream: the saved canonical text stays authoritative.
                const auto resident_capture = capture_on(children[0], base, true);
                require(detsim::resident::get(children[0]).snapshot != nullptr,
                        "resident proof absent before resident-reuse test");
                const auto before_stats = capture_layout_stats();
                MapsStreamScope stream(children[0], "");
                CaptureLayoutMode mode("on");
                sys_state reused(info, &source);
                require(reused.ss_hash == resident_capture.state.ss_hash,
                        "resident reuse changed unchanged identity");
                require(maps_override_opens == 0,
                        "resident capture opened the live maps stream");
                const auto after_stats = capture_layout_stats();
                require(after_stats.hits == before_stats.hits + NP &&
                        after_stats.builds == before_stats.builds,
                        "resident capture did not reuse the canonical layout");
            }
            {
                detsim::resident::invalidate_all();
                const std::string malformed = base + "not-a-mapping\n";
                MapsStreamScope stream(children[0], malformed);
                CaptureLayoutMode mode("on");
                const auto before_stats = capture_layout_stats();
                const auto before_saves = saved_successors();
                const std::string error = failure_of([&] {
                    sys_state bad(info, &source);
                });
                require(error == "Invalid memory mapping text",
                        "malformed live tail was not rejected");
                require(saved_successors() == before_saves,
                        "malformed live tail saved a successor state");
                const auto after_stats = capture_layout_stats();
                require(after_stats.lookups == before_stats.lookups + 1 &&
                        after_stats.hits == before_stats.hits &&
                        after_stats.builds == before_stats.builds &&
                        after_stats.admits == before_stats.admits &&
                        after_stats.entries == before_stats.entries,
                        "malformed live tail was rebuilt or published");
            }

            {
                std::string parser_maps = rewrite_first_line(base,
                    [](const TestMapRow& row) { return row.name == "[stack]"; },
                    [](const TestMapRow& row) {
                        std::string line = format_test_row(row.start, row.end,
                            row.flags, row.offset, row.dev, row.inode,
                            "long-" + std::string(600, 'n'));
                        line.insert(line.size() - 1,
                            std::string(1, '\0') + "ignored-tail");
                        return line;
                    });
                parser_maps +=
                    "ffffffffff600000-ffffffffff601000 r-xp 1 0:0 0 [vvar-no-newline]";
                require(parser_maps.back() != '\n', "no-newline fixture");
                const auto off = capture_off(children[0], parser_maps);
                const auto on = capture_on(children[0], parser_maps, true);
                require(off.state.ss_hash == on.state.ss_hash,
                        "NUL, long-name or missing-final-newline parse differed");
            }
            {
                const size_t prefix = strlen(maps_padding_prefix());
                std::string line = maps_padding_prefix();
                line += "[vvar";
                line.append(1023 - prefix - 5 - 1, 'p');
                line += "\n";
                require(line.size() == 1023, "1023-byte parser fixture size");
                const auto off = capture_off(children[0], base + line);
                const auto on = capture_on(children[0], base + line, true);
                require(off.state.ss_hash == on.state.ss_hash,
                        "1023-byte parser line differed");
            }
            {
                const size_t prefix = strlen(maps_padding_prefix());
                std::string line = maps_padding_prefix();
                line += "[vvar";
                line.append(1024 - prefix - 5 - 1, 'q');
                line += "\n";
                require(line.size() == 1024, "1024-byte parser fixture size");
                const auto before_saves = saved_successors();
                const std::string off_error = failure_of([&] {
                    (void)capture_off(children[0], base + line);
                });
                const std::string on_error = failure_of([&] {
                    (void)capture_on(children[0], base + line, false);
                });
                require(off_error == "Invalid memory mapping text" &&
                        on_error == off_error,
                        "1024-byte parser line disagreement");
                require(saved_successors() == before_saves,
                        "1024-byte parser rejection saved a successor state");
            }

            const auto warm = capture_on(children[0], base, true);
            auto changed_check = [&](const std::string& maps, const char *label) {
                const auto before = capture_layout_stats();
                const auto changed = capture_on(children[0], maps, false);
                require(changed.state.ss_hash != warm.state.ss_hash, label);
                require(changed.stats.builds == before.builds + 1, label);
                require(changed.stats.hits == before.hits + 1, label);
                const auto off = capture_off(children[0], maps);
                require(off.state.ss_hash == changed.state.ss_hash, label);
            };
            changed_check(change_stack_flags(base, "r--p"),
                          "permission change did not miss and match off");
            changed_check(rename_stack(base, "renamed-stack"),
                          "name change did not miss and match off");
            changed_check(change_file_backing(base),
                          "backing change did not miss and match off");
            {
                const std::string shared = change_stack_flags(base, "r--s");
                const auto before_saves = saved_successors();
                const std::string off_error = failure_of([&] {
                    (void)capture_off(children[0], shared);
                });
                const std::string on_error = failure_of([&] {
                    (void)capture_on(children[0], shared, false);
                });
                require(off_error == on_error &&
                        off_error.find("Unsupported snapshot mapping") !=
                            std::string::npos,
                        "shared mapping rejection differed between paths");
                require(saved_successors() == before_saves,
                        "shared mapping rejection saved a successor state");
            }

            // A raw hit must move the shared entry to MRU. The canonical hit
            // path is covered by the pressure loop above; this isolates the raw
            // path because a stale tail is evicted immediately afterwards.
            {
                detsim::clear_capture_layout_cache();
                const std::string a = pad_maps_to(base, 65535);
                const std::string b = pad_maps_to(base, 65536);
                const std::string c = pad_maps_to(base, 65530);
                const std::string d = pad_maps_to(base, 65531);
                const std::string e = pad_maps_to(base, 65532);
                const auto seed_a = capture_on(children[0], a, true);
                capture_on(children[0], b, false);
                const auto seed_c = capture_on(children[0], c, false);
                require(seed_c.stats.entries == 4,
                        "LRU fixture did not fill the cache");
                const auto before_hit = capture_layout_stats();
                const auto hit_a = capture_on(children[0], a, false);
                require(hit_a.stats.builds == before_hit.builds &&
                        hit_a.stats.hits == before_hit.hits + NP,
                        "raw hit did not reuse the LRU entry");
                capture_on(children[0], d, false);
                capture_on(children[0], e, false);
                const auto before_reuse = capture_layout_stats();
                const auto reuse_a = capture_on(children[0], a, false);
                require(reuse_a.state.ss_hash == seed_a.state.ss_hash,
                        "raw LRU reuse changed identity");
                require(reuse_a.stats.hits == before_reuse.hits + NP &&
                        reuse_a.stats.builds == before_reuse.builds,
                        "raw hit did not move the shared entry to MRU");
            }
        }
        for (int i=0;i<NP;++i)
            require(source.child[i].process_state==initial_process_states[i],"process model state absent from capture");
        ptmc_state.process_states[1].random_state^=0x1234;
        sys_state random_changed(info,&source);
        require(random_changed.ts_hash[1]!=source.ts_hash[1] && random_changed.ss_hash!=source.ss_hash,
                "RNG stream position absent from snapshot identity");
        require(source.child[1].process_state==initial_process_states[1],"RNG branch mutated source snapshot");
        compare_full(random_changed);
        ptmc_state.running_state=random_changed;
        source.recover_running_state();
        for (int i=0;i<NP;++i)
            require(ptmc_state.process_states[i]==initial_process_states[i],"restored RNG/signal state differs");
        ptmc_state.process_states[0].signal_actions[SIGUSR1].handler=0x12345678;
        sys_state action_changed(info,&source);
        require(action_changed.ts_hash[0]!=source.ts_hash[0] && action_changed.ss_hash!=source.ss_hash,
                "signal disposition absent from snapshot identity");
        require(source.child[0].process_state==initial_process_states[0],"signal branch mutated source snapshot");
        compare_full(action_changed);
        ptmc_state.running_state=action_changed;
        source.recover_running_state();
        for (int i=0;i<NP;++i)
            require(ptmc_state.process_states[i]==initial_process_states[i],"signal branch restore differs");
        sys_state process_replayed(info,&source);
        require(process_replayed.ss_hash==source.ss_hash,"RNG/signal branch restore identity drift");
        compare_full(process_replayed);
        ptmc_state.fs_states[1].add_virtual_file_mapping(provenance_page,provenance_page+4096,"/provenance");
        sys_state file_backed(info,&source);
        require(file_backed.ts_hash[1]!=source.ts_hash[1] && file_backed.ss_hash!=source.ss_hash &&
                source.child[1].fs_state.virtual_file_mappings.empty(),
                "mapping provenance missing from identity or leaked across copies");
        compare_full(file_backed);
        ptmc_state.running_state=file_backed;
        source.recover_running_state();
        require(ptmc_state.fs_states[1].virtual_file_mappings.empty(),"provenance leaked into predecessor");
        sys_state loaded_backing(file_backed.ss_hash);
        loaded_backing.recover_running_state();
        require(ptmc_state.fs_states[1].overlaps_virtual_file_mapping(provenance_page,provenance_page+4096),
                "persisted restore lost mapping provenance");
        ptmc_state.cursor=1;
        syscall_info discard{};discard.nr=SYS_madvise;
        discard.args[0]=provenance_page;discard.args[1]=4096;discard.args[2]=MADV_DONTNEED;
        SyscallDispatcher dispatcher;
        bool refused=false;
        try {dispatcher.on_enter(children[1],SYS_madvise,discard);}
        catch(const std::runtime_error&) {refused=true;}
        require(refused,"restored file mapping became eligible for anonymous discard");
        sys_state provenance_replayed(info,&loaded_backing);
        require(provenance_replayed.ss_hash==file_backed.ss_hash,"provenance restore identity drift");
        source.recover_running_state();
        ptmc_state.cursor=0;
        // Cross-node modeled delivery must invalidate the idle node's hash.
        ptmc_state.time[1].tv_sec++;
        sys_state changed(info, &source);
        require(changed.ts_hash[1] != source.ts_hash[1], "inactive modeled update lost");
        compare_full(changed);
        ptmc_state.running_state = changed;
        source.recover_running_state();
        sys_state restored(info, &source);
        require(restored.ss_hash == source.ss_hash, "branch restore reused stale running-state hash");
        compare_full(restored);
        setenv("DETSIM_CAPTURE_MODE", "verify", 1);
        sys_state verified(info, &source);
        require(verified.ss_hash == source.ss_hash, "verification mode identity differs");
        unsetenv("DETSIM_CAPTURE_MODE");
        // A stopped private process can still be changed by tracer expressions
        // or another process: metadata equality alone is not a valid fast path.
        require(ptrace(PTRACE_POKEDATA, children[1], &guest_value, 99) == 0, "inactive memory edit");
        sys_state memory_changed(info, &source);
        require(memory_changed.ts_hash[1] != source.ts_hash[1], "inactive memory edit lost");
        compare_full(memory_changed);
        ptmc_state.running_state = memory_changed;
        source.recover_running_state();
        UdpDatagram datagram{};
        datagram.content = "peer delivery";
        ptmc_state.sock_states[1].deliver_udp_datagram(udp_fd, datagram);
        sys_state socket_changed(info, &source);
        require(socket_changed.ts_hash[1] != source.ts_hash[1], "inactive socket update lost");
        compare_full(socket_changed);
        ptmc_state.running_state = socket_changed;
        source.recover_running_state();
        // Restoring reconstitutes queued data, descriptor flags and opaque
        // watches, independently of later branch mutations and accepted sockets.
        ptmc_state.cursor = 1;
        require(ptmc_state.fd_managers[1]->get_descriptor_flags(pipe_ends[0]) == FD_CLOEXEC &&
                (ptmc_state.fd_managers[1]->get_status_flags(pipe_ends[0]) & O_NONBLOCK), "restored pipe flags");
        require(ptmc_state.sock_states[1].epoll_instances().at(epfd).watched_fds.at(pipe_ends[0]).data == watch.data.u64,
                "restored epoll opaque data");
        const int accepted = ptmc_state.sock_states[1].do_accept(listener_fd, nullptr, nullptr);
        require(accepted >= 0 && ptmc_state.sock_states[1].do_recv(accepted, guest_address, sizeof(tcp_payload), 0) == sizeof(tcp_payload),
                "restored preaccept TCP data missing");
        char received[sizeof(tcp_payload)]{};
        require(guest_read_exact(children[1], guest_address, received, sizeof(received)) &&
                !memcmp(received, tcp_payload, sizeof(received)), "restored TCP payload changed");
        require(ptmc_state.fs_states[1].do_pipe_read(pipe_ends[0], guest_address, sizeof(pipe_payload)) == sizeof(pipe_payload),
                "restored pipe data missing");
        require(ptmc_state.fs_states[1].do_pipe_close(pipe_ends[1]) == 0, "close branch pipe end");
        ptmc_state.cursor = 0;
        sys_state consumed(info, &source);
        require(consumed.ts_hash[1] != source.ts_hash[1], "I/O branch mutation absent from identity");
        ptmc_state.running_state = consumed;
        source.recover_running_state();
        sys_state replayed(info, &source);
        require(replayed.ss_hash == source.ss_hash, "I/O branch restore identity drift");
        compare_full(replayed);
        // The selected actor is always recaptured even when modeled metadata agrees.
        user_regs_struct regs;
        require(ptrace(PTRACE_GETREGS, children[0], nullptr, &regs) == 0, "read actor registers");
        regs.rax ^= 1;
        require(ptrace(PTRACE_SETREGS, children[0], nullptr, &regs) == 0, "change actor registers");
        sys_state active_changed(info, &source);
        require(active_changed.ts_hash[0] != source.ts_hash[0], "active actor incorrectly reused");
        compare_full(active_changed);
        // Generic captures have no comparison hint and use the ordinary store path.
        const auto before_generic = StateStore::instance().get_stats().save_calls;
        sys_state generic(info);
        require(StateStore::instance().get_stats().save_calls == before_generic + NP,
                "unanchored capture bypassed the ordinary store path");
        // Closed native execution may reuse memory, never stale model metadata.
        // The existing full capture oracle remains independent of that proof.
        source.recover_running_state();
        setenv("DETSIM_SNAPSHOT_BACKEND", "pages", 1);
        setenv("DETSIM_RESIDENT_MEMORY", "verify", 1);
        {
            detsim::resident::Scope controlled;
            source.recover_running_state();
            sys_state page_source(info, &source);
            ptmc_state.running_state = page_source;
            require(detsim::resident::get(children[1]).snapshot != nullptr, "missing resident capture proof");
            {
                const auto saved_image = detsim::resident::get(children[1]);
                detsim::resident::invalidate(children[1]);
                require(!detsim::resident::get(children[1]).snapshot,
                        "page-index hint retained live memory authority");
                sys_state recaptured(info, &page_source);
                require(recaptured.ss_hash == page_source.ss_hash,
                        "invalidated resident recapture changed identity");
                require(detsim::resident::get(children[1]).pages == saved_image.pages,
                        "identical saved page index rebuilt after live invalidation");
                compare_full(recaptured);
            }
            const auto ro_view = SnapshotView::open(page_source.ts_hash[1]);
            size_t ro_region = ro_view->regions().size();
            for (size_t index = 0; index < ro_view->regions().size(); ++index) {
                const auto& region = ro_view->regions()[index];
                if (region.start <= reinterpret_cast<uintptr_t>(&readonly_value) &&
                    reinterpret_cast<uintptr_t>(&readonly_value) < region.end && !(region.prot & PROT_WRITE))
                    ro_region = index;
            }
            require(ro_region < ro_view->regions().size(), "fixture constant is not file-backed RO");
            {
                detsim::resident::ProtectedResume authorized;
                detsim::resident::resuming(children[1]);
            }
            require(!detsim::resident::get(children[1]).snapshot, "RO capability retained full-image authority");
            const auto& ro = ro_view->regions()[ro_region];
            require(detsim::resident::get_read_only(children[1]).region(ro_region, ro.start, ro.end, ro.prot),
                    "protected region proof missing");
            const long rw_change = 18;
            require(guest_write_exact(children[1], &guest_value, &rw_change, sizeof rw_change), "RW copyout");
            require(detsim::resident::get_read_only(children[1]).region(ro_region, ro.start, ro.end, ro.prot),
                    "RW copyout unnecessarily erased disjoint RO proof");
            sys_state ro_reused(info, &page_source);
            compare_full(ro_reused);
            page_source.recover_running_state();
            {
                detsim::resident::ProtectedResume authorized;
                detsim::resident::resuming(children[1]);
            }
            require(tracee_write_word(children[1], const_cast<long*>(&readonly_value), 44) == 0, "force RO write");
            require(!detsim::resident::get_read_only(children[1]).region(ro_region, ro.start, ro.end, ro.prot),
                    "POKEDATA retained changed RO proof");
            sys_state ro_changed(info, &page_source);
            compare_full(ro_changed);
            page_source.recover_running_state();
            long ro_observed = 0;
            require(guest_read_exact(children[1], &readonly_value, &ro_observed, sizeof ro_observed) && ro_observed == 31,
                    "RO restore lost original bytes");
            const uintptr_t ro_page = reinterpret_cast<uintptr_t>(&readonly_value) & ~uintptr_t(4095);
            require(tracee_do_syscall(children[1], SYS_mprotect, ro_page, 4096, PROT_READ|PROT_WRITE, 0, 0, 0) == 0,
                    "RO to writable mapping");
            const long replacement = 52;
            require(guest_write_exact(children[1], const_cast<long*>(&readonly_value), &replacement, sizeof replacement),
                    "write newly writable region");
            require(tracee_do_syscall(children[1], SYS_mprotect, ro_page, 4096, PROT_READ, 0, 0, 0) == 0,
                    "restore RO mapping");
            require(!detsim::resident::get_read_only(children[1]).region(ro_region, ro.start, ro.end, ro.prot),
                    "native mapping change revived old RO proof");
            sys_state reprotected(info, &page_source);
            compare_full(reprotected);
            page_source.recover_running_state();
            (void)tracee_do_syscall(children[1], SYS_getuid, 0, 0, 0, 0, 0, 0);
            require(!detsim::resident::get_read_only(children[1]).region(ro_region, ro.start, ro.end, ro.prot) &&
                    !detsim::resident::get(children[1]).snapshot, "native query preserved unaudited proof");
            page_source.recover_running_state();
            ptmc_state.process_states[1].random_state ^= 0x2345;
            sys_state peer_metadata(info, &page_source);
            require(peer_metadata.ts_hash[1] != page_source.ts_hash[1], "resident reuse hid peer metadata");
            compare_full(peer_metadata);
            page_source.recover_running_state();
            const long changed_value = 93;
            require(guest_write_exact(children[1], &guest_value, &changed_value, sizeof changed_value), "tracked guest write");
            require(!detsim::resident::get(children[1]).snapshot, "guest write retained resident proof");
            sys_state memory_changed(info, &page_source);
            compare_full(memory_changed);
            // Capture can publish a new resident image without selecting it as
            // the UI/runtime state. Same selected hash is not a restore proof.
            page_source.recover_running_state();
            long observed = 0;
            require(guest_read_exact(children[1], &guest_value, &observed, sizeof observed) && observed == 17,
                    "resident restore retained changed bytes");
            const auto mapped = tracee_do_syscall(children[1], SYS_mmap, 0, 4096, PROT_READ|PROT_WRITE,
                MAP_PRIVATE|MAP_ANONYMOUS, uint64_t(-1), 0);
            require(mapped && mapped < uint64_t(-4095) && !detsim::resident::get(children[1]).snapshot,
                    "mapping injection retained resident proof");
            sys_state mapped_state(info, &page_source);
            compare_full(mapped_state);
            ptmc_state.running_state = mapped_state;
            page_source.recover_running_state();
        }
        require(!detsim::resident::get(children[1]).snapshot, "resident proof escaped closed scope");
        unsetenv("DETSIM_RESIDENT_MEMORY");
        unsetenv("DETSIM_SNAPSHOT_BACKEND");
        // Heap endpoint proof: established only by actual publication, retained
        // through demotion by ordinary guest writes, then used to avoid injected
        // brk queries while bytes and identity still match the full path.
        {
            require(!detsim::resident::known_brk(children[1]), "scope entry inherited a heap endpoint");
            detsim::resident::Scope heap_scope;
            require(!detsim::resident::known_brk(children[1]), "scope entry retained a heap endpoint");
            source.recover_running_state();
            const uint64_t heap_brk = source.child[1].brk;
            const long proof_value = 93;
            require(guest_write_exact(children[1], &guest_value, &proof_value, sizeof proof_value),
                    "heap proof fixture write");
            require(!detsim::resident::get(children[1]).snapshot,
                    "guest write retained the resident full image");
            injected_brk_questions = injected_brk_moves = 0;
            sys_state proof_capture(info, &source);
            require(injected_brk_questions == 0, "capture re-queried a proven heap endpoint");
            require(injected_brk_moves == 0, "capture moved a proven heap endpoint");
            require(detsim::resident::known_brk(children[1]).value_or(0) == heap_brk,
                    "actual capture did not publish the heap endpoint");
            require(proof_capture.ts_hash[1] != source.ts_hash[1],
                    "heap proof fixture write absent from capture identity");
            compare_full(proof_capture);
            injected_brk_questions = injected_brk_moves = 0;
            // Force the restore path: the selected state already owns identity.
            for (int i = 0; i < NP; ++i) ptmc_state.tracee_image_dirty[i] = true;
            source.recover_running_state();
            require(injected_brk_questions == 0, "equal-heap restore re-queried a proven endpoint");
            require(injected_brk_moves == 0, "equal-heap restore moved a proven endpoint");
            long restored_value = 0;
            require(guest_read_exact(children[1], &guest_value, &restored_value, sizeof restored_value) &&
                    restored_value == 17, "equal-heap restore retained changed bytes");
            require(detsim::resident::known_brk(children[1]).value_or(0) == heap_brk,
                    "actual restore did not republish the heap endpoint");
            sys_state proof_replayed(info, &source);
            require(proof_replayed.ss_hash == source.ss_hash, "proven heap restore identity drift");
            require(detsim::resident::known_brk(children[1]).value_or(0) == heap_brk,
                    "same-root publication dropped the heap endpoint");
            compare_full(proof_replayed);

            // Whitelisted read-only injections retain the proof; a plain resume
            // and an ordinary injection revoke it.
            require(tracee_do_syscall(children[1], SYS_brk, 0, 0, 0, 0, 0, 0) == heap_brk,
                    "safe brk query changed the heap");
            require(detsim::resident::known_brk(children[1]).value_or(0) == heap_brk,
                    "safe brk query revoked the heap endpoint");
            require(tracee_do_syscall(children[1], SYS_prctl, PR_GET_TID_ADDRESS,
                    scratch_page + 0x800, 0, 0, 0, 0) == 0,
                    "safe GET_TID_ADDRESS injection failed");
            require(detsim::resident::known_brk(children[1]).value_or(0) == heap_brk,
                    "safe GET_TID_ADDRESS injection revoked the heap endpoint");
            detsim::resident::resuming(children[1]);
            require(!detsim::resident::known_brk(children[1]),
                    "unprotected resume retained the heap endpoint");
            injected_brk_questions = injected_brk_moves = 0;
            source.recover_running_state();
            require(injected_brk_moves == 1,
                    "restore after an unprotected resume reused a revoked heap endpoint");

            (void)tracee_do_syscall(children[1], SYS_getuid, 0, 0, 0, 0, 0, 0);
            require(!detsim::resident::known_brk(children[1]) &&
                    !detsim::resident::get(children[1]).snapshot,
                    "ordinary injection retained the heap endpoint");
            injected_brk_questions = injected_brk_moves = 0;
            sys_state unaudited_capture(info, &source);
            require(injected_brk_questions == 1,
                    "capture did not fall back to one brk query after an ordinary injection");
            source.recover_running_state();

            const uintptr_t remapped_page = provenance_page;
            require(tracee_do_syscall(children[1], SYS_mprotect, remapped_page, 4096, PROT_READ, 0, 0, 0) == 0,
                    "heap proof mprotect fixture");
            require(!detsim::resident::known_brk(children[1]),
                    "mapping change retained the heap endpoint");
            injected_brk_questions = injected_brk_moves = 0;
            sys_state remapped_capture(info, &source);
            require(remapped_capture.ts_hash[1] != source.ts_hash[1],
                    "mapping change absent from capture identity");
            require(injected_brk_questions == 1,
                    "capture did not fall back to one brk query after a mapping change");
            source.recover_running_state();

            // Real guest brk transitions run after an unprotected resume. Grow,
            // shrink and failed requests all revoke the proof; only the absent
            // proof makes the following restore fall back to the target brk.
            require(detsim::resident::known_brk(children[1]).value_or(0) == heap_brk,
                    "restore did not republish the heap endpoint before native brk");
            const uint64_t grown_brk = native_guest_syscall(children[1], 1, SYS_brk, heap_brk + 0x4000);
            require(grown_brk == heap_brk + 0x4000, "native brk grow failed");
            require(!detsim::resident::known_brk(children[1]),
                    "native brk grow retained the heap endpoint");
            injected_brk_questions = injected_brk_moves = 0;
            sys_state grown_capture(info, &source);
            require(injected_brk_questions == 1,
                    "capture after a native grow did not query the heap");
            require(grown_capture.child[1].brk == grown_brk,
                    "capture after a native grow lost the endpoint");
            compare_full(grown_capture);
            injected_brk_questions = injected_brk_moves = 0;
            source.recover_running_state();
            require(injected_brk_moves == 1, "unequal-heap restore skipped the brk move");
            require(detsim::resident::known_brk(children[1]).value_or(0) == heap_brk,
                    "unequal-heap restore did not republish the source endpoint");
            // A real emulated getpid engine step proves the production
            // classification: the full image demotes on the protected
            // user->entry resume, while the independent heap proof survives the
            // suppressed entry->exit resume.
            require(detsim::resident::get(children[1]).snapshot != nullptr,
                    "engine getpid fixture lacks a full resident image");
            const uint64_t engine_pid = native_guest_syscall(children[1], 1, SYS_getpid, 0);
            require(engine_pid == static_cast<uint64_t>(logical_process_id(1)),
                    "engine getpid did not emulate the logical pid");
            require(!detsim::resident::get(children[1]).snapshot,
                    "engine getpid retained the full resident image");
            require(detsim::resident::known_brk(children[1]).value_or(0) == heap_brk,
                    "engine getpid revoked the heap endpoint");
            const uint64_t shrunk_brk = heap_brk - 0x1000;
            require(native_guest_syscall(children[1], 1, SYS_brk, shrunk_brk) == shrunk_brk,
                    "native brk shrink failed");
            require(!detsim::resident::known_brk(children[1]),
                    "native brk shrink retained the heap endpoint");
            injected_brk_questions = injected_brk_moves = 0;
            source.recover_running_state();
            require(injected_brk_moves == 1,
                    "restore after a native shrink skipped the brk move");
            require(native_guest_syscall(children[1], 1, SYS_brk, 0x7ffffffffffff000ULL) == heap_brk,
                    "failed native brk changed the endpoint");
            require(!detsim::resident::known_brk(children[1]),
                    "failed native brk retained the heap endpoint");
            injected_brk_questions = injected_brk_moves = 0;
            source.recover_running_state();
            require(injected_brk_moves == 1,
                    "restore after a failed native brk skipped the fallback query");
            require(detsim::resident::known_brk(children[1]).value_or(0) == heap_brk,
                    "restore after a failed native brk lost the endpoint");
        }
        require(!detsim::resident::known_brk(children[1]), "heap endpoint escaped its closed scope");

        // Off and full-capture bypasses must behave like the simple path.
        {
            setenv("DETSIM_RESIDENT_MEMORY", "off", 1);
            detsim::resident::Scope disabled_scope;
            // Without residency the selected identity may legitimately skip a
            // clean restore, so force the image path the bypass must exercise.
            for (int i = 0; i < NP; ++i) ptmc_state.tracee_image_dirty[i] = true;
            injected_brk_questions = injected_brk_moves = 0;
            source.recover_running_state();
            require(injected_brk_moves == NP, "disabled residency skipped brute-force heap restores");
            injected_brk_questions = injected_brk_moves = 0;
            sys_state disabled_capture(info, &source);
            require(injected_brk_questions == NP, "disabled residency reused a heap endpoint");
            require(!detsim::resident::known_brk(children[1]),
                    "disabled residency published a heap endpoint");
        }
        unsetenv("DETSIM_RESIDENT_MEMORY");
        {
            detsim::resident::Scope full_scope;
            source.recover_running_state();
            setenv("DETSIM_CAPTURE_MODE", "full", 1);
            injected_brk_questions = injected_brk_moves = 0;
            sys_state full_capture(info, &source);
            require(injected_brk_questions == NP, "full capture bypass reused a heap endpoint");
            require(full_capture.ss_hash == source.ss_hash, "full capture bypass identity drift");
            injected_brk_questions = injected_brk_moves = 0;
            source.recover_running_state();
            require(injected_brk_moves == NP, "full restore bypass reused a heap endpoint");
            unsetenv("DETSIM_CAPTURE_MODE");
            require(detsim::resident::known_brk(children[1]).value_or(0) == source.child[1].brk,
                    "full bypass left the published endpoint unusable");
        }

        // A store epoch change invalidates every entry, including its endpoint.
        {
            detsim::resident::Scope epoch_scope;
            source.recover_running_state();
            require(detsim::resident::known_brk(children[1]).value_or(0) == source.child[1].brk,
                    "epoch fixture did not establish a heap endpoint");
            const auto generation_before = StateStore::instance().generation();
            detsim::snapshot::SharedObjectPool epoch_pool("epoch-pool");
            StateStore::instance().attach_shared_pool("epoch-pool", false);
            require(StateStore::instance().generation() != generation_before,
                    "shared-pool attachment did not change the store epoch");
            require(!detsim::resident::known_brk(children[1]) &&
                    !detsim::resident::get(children[1]).snapshot,
                    "store epoch change retained a stale heap endpoint");
        }

        // Poisoning the store must reject an otherwise valid retained endpoint;
        // reopening it must start without that old authority.
        {
            detsim::resident::Scope poisoned_scope;
            source.recover_running_state();
            auto& store = StateStore::instance();
            require(detsim::resident::known_brk(children[0]).has_value(),
                    "poison fixture has no heap endpoint");
            store.wait_for_completion();
            fail_store_sync = true;
            bool sync_failed = false;
            try { store.wait_for_completion(); }
            catch (const std::exception&) { sync_failed = true; }
            fail_store_sync = false;
            require(sync_failed, "heap poison fixture did not fail storage");
            bool heap_rejected = false;
            try { (void)detsim::resident::known_brk(children[0]); }
            catch (const std::exception&) { heap_rejected = true; }
            require(heap_rejected, "heap endpoint bypassed a poisoned store");
            bool shutdown_failed = false;
            try { store.shutdown(); }
            catch (const std::exception&) { shutdown_failed = true; }
            require(shutdown_failed, "poisoned store lost its shutdown failure");
            store.init(config);
            require(!detsim::resident::known_brk(children[0]),
                    "reopened store reused an old heap endpoint");
            source.recover_running_state();
            require(detsim::resident::known_brk(children[0]).value_or(0) == source.child[0].brk,
                    "fresh restoration did not reestablish the heap endpoint");
        }

        // Verify mode validates every used proof against a real brk query. A
        // test-only publication of the valid root with a wrong argument must be
        // rejected before any successor save, and must not be repaired silently.
        setenv("DETSIM_RESIDENT_MEMORY", "verify", 1);
        {
            detsim::resident::Scope verify_scope;
            source.recover_running_state();
            const uint64_t live_brk = source.child[0].brk;
            require(detsim::resident::known_brk(children[0]).value_or(0) == live_brk,
                    "verify fixture did not establish a heap endpoint");
            // A failed read-only injection must revoke every proof through
            // ReadOnlyInjection unwinding. The wait wrapper consumes the real
            // entry stop but reports a non-stop, so tracee_do_syscall throws
            // through its normal status check, never across the libc ABI.
            fail_wait_pid = children[1];
            bool injection_failed = false;
            try {
                (void)tracee_do_syscall(children[1], SYS_brk, 0, 0, 0, 0, 0, 0);
            } catch (const std::runtime_error&) {
                injection_failed = true;
            }
            require(fail_wait_pid == 0 && injection_failed,
                    "injection failure fixture did not throw");
            require(!detsim::resident::known_brk(children[0]) &&
                    !detsim::resident::known_brk(children[1]) &&
                    !detsim::resident::get(children[1]).snapshot,
                    "injection failure retained heap authority");
            // The wrapper consumed the real entry stop; finish that syscall so
            // the tracee is at the usual exit stop, then recover normally.
            ptrace_right(PTRACE_SYSCALL, children[1], nullptr, nullptr);
            int normalized = 0;
            require(waitpid(children[1], &normalized, 0) == children[1] &&
                    WIFSTOPPED(normalized) &&
                    WSTOPSIG(normalized) == (SIGTRAP | 0x80),
                    "injection failure left an unknown stop state");
            source.recover_running_state();
            detsim::resident::invalidate(children[0]);
            detsim::resident::publish(children[0], source.ts_hash[0], live_brk - 0x1000);
            require(detsim::resident::known_brk(children[0]).value_or(0) == live_brk - 0x1000,
                    "test-only wrong heap endpoint was not published");
            const auto saves_before = StateStore::instance().get_stats().save_calls;
            std::string rejected;
            try { sys_state rejected_capture(info, &source); }
            catch (const std::runtime_error& error) { rejected = error.what(); }
            require(rejected.find("heap proof") != std::string::npos,
                    "verify accepted a mismatched heap endpoint");
            require(StateStore::instance().get_stats().save_calls == saves_before,
                    "mismatched heap endpoint reached a successor save");
            require(!detsim::resident::known_brk(children[0]) &&
                    !detsim::resident::get(children[0]).snapshot,
                    "mismatched heap endpoint was silently retained or repaired");
            // A proof that disagrees with the restore target must be audited and
            // rejected before the brk setter, not silently repaired.
            detsim::resident::publish(children[0], source.ts_hash[0], live_brk - 0x1000);
            require(detsim::resident::known_brk(children[0]).value_or(0) == live_brk - 0x1000,
                    "stale restore proof was not republished");
            for (int i = 0; i < NP; ++i) ptmc_state.tracee_image_dirty[i] = true;
            injected_brk_questions = injected_brk_moves = 0;
            std::string stale_rejected;
            try { source.recover_running_state(); }
            catch (const std::runtime_error& error) { stale_rejected = error.what(); }
            require(stale_rejected.find("heap proof") != std::string::npos,
                    "restore accepted a stale heap proof");
            require(injected_brk_questions == 1 && injected_brk_moves == 0,
                    "stale heap proof reached the brk setter");
            require(!detsim::resident::known_brk(children[0]) &&
                    !detsim::resident::get(children[0]).snapshot,
                    "stale heap proof survived the restore audit");
        }
        unsetenv("DETSIM_RESIDENT_MEMORY");
        // A virtual exit parks the OS task. Restoring its product-state image
        // must restore memory/resources even while that node cannot execute.
        source.recover_running_state();
        ExecutionEngine engine;
        syscall_info exit_info{};
        ptmc_state.cursor=1;
        // This slice models graceful normal exit; accept/drain the fixture's
        // connection rather than requiring the not-yet-modeled TCP reset path.
        const int exiting_connection=ptmc_state.sock_states[1].do_accept(listener_fd,nullptr,nullptr);
        require(exiting_connection>=0,"accept before graceful exit");
        require(ptmc_state.sock_states[1].do_recv(exiting_connection,guest_address,4096,0)==sizeof(tcp_payload),
                "drain unread fixture payload before graceful exit");
        require(engine.execute_syscall(children[1],exit_info)==2 && exit_info.nr==SYS_exit_group &&
                DISEXITED(ptmc_state.status[1]), "fixture must exit through the virtual lifecycle path");
        syscall_info dead_info[NP][MAX_THREADS_PER_PROCESS];
        memcpy(dead_info,info,sizeof(dead_info));dead_info[1][0]=exit_info;
        sys_state exited(dead_info,&source);
        ptmc_state.running_state=exited;
        source.recover_running_state();
        require(ptrace(PTRACE_POKEDATA,children[1],&guest_value,99)==0,"edit alternate live branch");
        ptmc_state.process_states[1].random_state^=0x5678;
        ptmc_state.fs_states[1].set_creation_mask(0077);
        ptmc_state.time[1].tv_sec+=7;
        sys_state alternate(info,&source);
        ptmc_state.running_state=alternate;
        // Decode persisted state too: this is not only an in-memory copy test.
        sys_state loaded_exit(exited.ss_hash);
        loaded_exit.recover_running_state();
        long exited_value=0;
        require(guest_read_exact(children[1],&guest_value,&exited_value,sizeof(exited_value)) &&
                exited_value==17,"exited node retained another branch's memory");
        require(ptmc_state.process_states[1]==exited.child[1].process_state &&
                ptmc_state.fs_states[1].creation_mask()==exited.child[1].fs_state.creation_mask() &&
                ptmc_state.time[1].tv_sec==exited.child[1].tv.tv_sec,
                "exited node retained another branch's modeled resources");
        ptmc_state.cursor=0;
        for (int actor : {-1,1,NP}) {
            bool rejected=false;
            try {(void)state_transition(loaded_exit,actor);}catch(const std::runtime_error&){rejected=true;}
            require(rejected && ptmc_state.cursor==0,"invalid or terminated actor reached transition execution");
        }
        sys_state recaptured_exit(dead_info,&loaded_exit);
        require(recaptured_exit.ss_hash==exited.ss_hash,"exited branch recapture identity drift");
        exec::init_all();
        const auto peer_step=state_transition(loaded_exit,0);
        require(peer_step.code==CKPT_EXIT && peer_step.new_state.ts_hash[1]==exited.ts_hash[1] &&
                peer_step.new_state.status[1]==exited.status[1],
                "stepping a live peer contaminated the parked terminal image");
        source.recover_running_state();
        sys_state resurrected(info,&source);
        require(resurrected.ss_hash==source.ss_hash && DISALIVE(ptmc_state.status[1]),
                "virtual exit predecessor cannot replay faithfully");
        // Fatal signal stops also park an image: exercise a real fault, not
        // merely a synthetic status flag, before switching branches again.
        const unsigned char fault_instruction[]={0x0f,0x0b}; // ud2 -> SIGILL
        require(guest_write_exact(children[1],reinterpret_cast<void*>(scratch_page+0x100),
                fault_instruction,sizeof(fault_instruction)),"write fault fixture");
        user_regs_struct fault_regs{};
        require(ptrace(PTRACE_GETREGS,children[1],nullptr,&fault_regs)==0,"read fault registers");
        fault_regs.rip=scratch_page+0x100;
        require(ptrace(PTRACE_SETREGS,children[1],nullptr,&fault_regs)==0,"arm native fault");
        ptmc_state.cursor=1;
        syscall_info fault_info{};
        require(engine.execute_syscall(children[1],fault_info)==3 && DISCRASHED(ptmc_state.status[1]),
                "fixture must crash at a real fatal signal stop");
        sys_state crashed(info,&source);
        ptmc_state.running_state=crashed;
        source.recover_running_state();
        require(ptrace(PTRACE_POKEDATA,children[1],&guest_value,123)==0,"edit alternate crash branch");
        ptmc_state.process_states[1].random_state^=0xabcd;
        sys_state crash_alternate(info,&source);
        ptmc_state.running_state=crash_alternate;
        sys_state loaded_crash(crashed.ss_hash);
        loaded_crash.recover_running_state();
        long crash_value=0;
        require(guest_read_exact(children[1],&guest_value,&crash_value,sizeof(crash_value)) &&
                crash_value==17 && ptmc_state.process_states[1]==crashed.child[1].process_state,
                "crashed node retained another branch's image");
        sys_state recaptured_crash(info,&loaded_crash);
        require(recaptured_crash.ss_hash==crashed.ss_hash,"crash branch recapture identity drift");
        ptmc_state.running_state=recaptured_crash;
        source.recover_running_state();
        sys_state crash_predecessor(info,&source);
        require(crash_predecessor.ss_hash==source.ss_hash,"crash predecessor restore identity drift");
        if (argc > 1) require(StateStore::instance().get_stats().disk_reads > 0, "oracle did not exercise cache eviction");
        // The all-node barrier already probes rseq once per physical thread.
        // A high-level restore must not repeat that probe inside each node's
        // memory/register restore. Count only PTRACE_GET_RSEQ_CONFIGURATION.
        require(ptrace(PTRACE_POKEDATA,children[0],&guest_value,21)==0,"edit first rseq-count branch");
        require(ptrace(PTRACE_POKEDATA,children[1],&guest_value,22)==0,"edit second rseq-count branch");
        sys_state diverged(info,&source);
        ptmc_state.running_state=diverged;
        size_t expected_rseq_checks=0;
        for (int i=0;i<NP;++i) expected_rseq_checks+=get_thread_list(ptmc_state.pids[i]).size();
        rseq_configuration_calls=0;
        source.recover_running_state();
        require(rseq_configuration_calls==expected_rseq_checks,
                "high-level restore repeated the per-node rseq guard");
        // Both public low-level APIs are reachable without the barrier and
        // must still perform their own rseq check.
        const auto public_rseq_checks=get_thread_list(children[0]).size();
        std::vector<maps_item> maps;
        rseq_configuration_calls=0;
        source.child[0].restore_memory_mappings(source.ts_hash[0],children[0],maps);
        require(rseq_configuration_calls==public_rseq_checks,
                "public mapping restore lost its rseq guard");
        rseq_configuration_calls=0;
        source.child[0].recover_mem_reg_snapshot(maps,source.ts_hash[0],children[0]);
        require(rseq_configuration_calls==public_rseq_checks,
                "public memory restore lost its rseq guard");
        // A later node's active rseq registration must be rejected by the same
        // barrier before the earlier node's image, model, budget, or store
        // records change.
        require(ptrace(PTRACE_POKEDATA,children[0],&guest_value,321)==0,"edit rseq preflight witness");
        // Change a modeled subsystem on the earlier node too, so a partial
        // restore of that node's resources would be visible after rejection.
        ptmc_state.process_states[0].random_state^=0x5a5a5a5a;
        ptmc_state.time[0].tv_sec+=11;
        ptmc_state.error_bound=5;
        sys_state before_rseq(info,&source);
        ptmc_state.running_state=before_rseq;
        user_regs_struct rseq_before{},rseq_after{};
        require(ptrace(PTRACE_GETREGS,children[0],nullptr,&rseq_before)==0,"rseq preflight registers");
        const auto rseq_tasks=get_thread_list(children[0]);
        const auto rseq_saves=StateStore::instance().get_stats().save_calls;
        require(tracee_do_syscall(children[1],SYS_rseq,
                reinterpret_cast<uintptr_t>(&registered_rseq),sizeof registered_rseq,
                0,RSEQ_SIG,0,0)==0,"register later-node rseq fixture");
        bool rseq_rejected=false;
        try {source.recover_running_state();}
        catch(const std::runtime_error& error){
            rseq_rejected=std::string(error.what()).find("active rseq registration")!=std::string::npos;
        }
        long rseq_witness=0;
        require(rseq_rejected && ptmc_state.error_bound==5 &&
                ptmc_state.running_state.ss_hash==before_rseq.ss_hash &&
                ptmc_state.process_states[0]==before_rseq.child[0].process_state &&
                ptmc_state.time[0].tv_sec==before_rseq.child[0].tv.tv_sec &&
                ptmc_state.status[0]==before_rseq.status[0] &&
                StateStore::instance().get_stats().save_calls==rseq_saves &&
                get_thread_list(children[0])==rseq_tasks &&
                guest_read_exact(children[0],&guest_value,&rseq_witness,sizeof(rseq_witness)) &&
                rseq_witness==321 &&
                ptrace(PTRACE_GETREGS,children[0],nullptr,&rseq_after)==0 &&
                !memcmp(&rseq_before,&rseq_after,sizeof(rseq_before)),
                "active rseq on a later node mutated an earlier node or changed the model");
        require(tracee_do_syscall(children[1],SYS_rseq,
                reinterpret_cast<uintptr_t>(&registered_rseq),sizeof registered_rseq,
                RSEQ_FLAG_UNREGISTER,RSEQ_SIG,0,0)==0,"unregister later-node rseq fixture");
        source.recover_running_state();
        // A later node's failure must be detected before any earlier image,
        // status, budget, or store record changes.
        require(ptrace(PTRACE_POKEDATA,children[0],&guest_value,456)==0,"edit preflight witness");
        ptmc_state.error_bound=3;
        // The proof must not outlive the process incarnation that established it.
        {
        detsim::resident::Scope death_scope;
        sys_state before_death(info,&source);
        ptmc_state.running_state=before_death;
        const pid_t vanished=children[1];
        kill(children[1],SIGKILL);waitpid(children[1],nullptr,0);children[1]=0;
        require(!detsim::resident::known_brk(vanished) && !detsim::resident::get(vanished).snapshot,
                "dead process retained a usable heap endpoint");
        user_regs_struct before_failure{},after_failure{};
        require(ptrace(PTRACE_GETREGS,children[0],nullptr,&before_failure)==0,"preflight registers");
        bool vanished_rejected=false;
        try {source.recover_running_state();}catch(const std::runtime_error&){vanished_rejected=true;}
        long witness=0;
        require(vanished_rejected && ptmc_state.error_bound==3 &&
                ptmc_state.running_state.ss_hash==before_death.ss_hash &&
                guest_read_exact(children[0],&guest_value,&witness,sizeof(witness)) && witness==456 &&
                ptrace(PTRACE_GETREGS,children[0],nullptr,&after_failure)==0 &&
                !memcmp(&before_failure,&after_failure,sizeof(before_failure)),
                "failed all-node restore preflight mutated an earlier node");
        const auto save_calls=StateStore::instance().get_stats().save_calls;
        bool capture_rejected=false;
        try {sys_state refused(info,&source);}catch(const std::runtime_error&){capture_rejected=true;}
        require(capture_rejected && StateStore::instance().get_stats().save_calls==save_calls,
                "failed all-node capture preflight published an earlier node");
        }
        // Clearing runtime provenance must not grant a disk-loaded snapshot
        // ownership of whatever physical process happens to occupy this slot.
        clear_snapshot_thread_bindings();
        bool foreign_rejected=false;
        try {(void)source.child[0].resolve_thread_bindings(0,source.ts_hash[0]);}
        catch(const std::runtime_error& error) {
            foreign_rejected=std::string(error.what()).find("no local thread binding provenance")!=std::string::npos;
        }
        require(foreign_rejected,"unregistered snapshot regained host thread ownership");
        StateStore::instance().shutdown();
        SysStateStore::instance().shutdown();
        for (pid_t &child : children) if (child > 0) { kill(child, SIGKILL); waitpid(child, nullptr, 0); child = 0; }
        std::filesystem::current_path(cwd); std::filesystem::remove_all(directory);
        puts("PASS capture: idle reuse, full byte oracle, branch restore, memory/socket updates, active capture, cache eviction");
        return 0;
    } catch (const std::exception& error) {
        for (pid_t child : children) if (child > 0) { kill(child, SIGKILL); waitpid(child, nullptr, 0); }
        fprintf(stderr, "FAIL: %s (artifacts %s)\n", error.what(), directory);
        return 1;
    }
}
