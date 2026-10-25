#include "engine/thread_control.h"
#include "ncurses_ui.h"

#include <cerrno>
#include <chrono>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }

namespace {

// ---------------------------------------------------------------------------
// Link-wrapped status I/O. Fake tids are served by a memfd containing the
// requested text; real tids go through the normal /proc open. Only descriptors
// that look like /proc/<tid>/status are counted and fault-injected.
// ---------------------------------------------------------------------------

struct TrackedFd {
    std::string path;
    unsigned lseeks = 0;
    unsigned reads = 0;
    unsigned pass_reads = 0;
    int fail_lseek = 0;
    bool positive_lseek = false;
    int lseek_eintr = 0;
    int fail_read_after = -1;
    int read_eintr = 0;
};

std::map<int, TrackedFd> tracked;
std::map<std::string, std::string> fake_status;
std::map<std::string, int> path_fds;
unsigned status_opens = 0;
unsigned total_lseeks = 0;
unsigned total_reads = 0;
unsigned task_directory_opens = 0, task_directory_closes = 0;
bool fail_task_rewind = false, fail_task_read = false, omit_task_rewind = false;
pid_t next_fake_tid = 900000;

bool is_status_path(const char* raw)
{
    const std::string_view path(raw);
    return path.starts_with("/proc/") && path.ends_with("/status");
}

std::string status_path(pid_t tid)
{
    return "/proc/" + std::to_string(tid) + "/status";
}

pid_t add_fake_status(const std::string& text)
{
    const pid_t tid = next_fake_tid++;
    fake_status[status_path(tid)] = text;
    return tid;
}

extern "C" int __real_open(const char*, int, ...);
extern "C" off_t __real_lseek(int, off_t, int);
extern "C" ssize_t __real_read(int, void*, size_t);
extern "C" int __real_close(int);

extern "C" int __wrap_open(const char* raw_path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = static_cast<mode_t>(va_arg(args, int));
        va_end(args);
    }
    const std::string path(raw_path);
    const auto fake = fake_status.find(path);
    if (fake != fake_status.end()) {
        const int fd = memfd_create("detsim-fake-status", MFD_CLOEXEC);
        if (fd < 0) return -1;
        size_t written = 0;
        while (written < fake->second.size()) {
            const ssize_t count = pwrite(fd, fake->second.data() + written,
                                         fake->second.size() - written,
                                         static_cast<off_t>(written));
            if (count < 0) {
                if (errno == EINTR) continue;
                const int error = errno;
                __real_close(fd);
                errno = error;
                return -1;
            }
            written += static_cast<size_t>(count);
        }
        ++status_opens;
        tracked.emplace(fd, TrackedFd{path});
        path_fds[path] = fd;
        return fd;
    }
    const int fd = (flags & O_CREAT) ? __real_open(raw_path, flags, mode)
                                     : __real_open(raw_path, flags);
    if (fd >= 0 && is_status_path(raw_path)) {
        ++status_opens;
        tracked.emplace(fd, TrackedFd{path});
        path_fds[path] = fd;
    }
    return fd;
}

extern "C" off_t __wrap_lseek(int fd, off_t offset, int whence)
{
    const auto found = tracked.find(fd);
    if (found == tracked.end()) return __real_lseek(fd, offset, whence);
    TrackedFd& entry = found->second;
    ++entry.lseeks;
    ++total_lseeks;
    if (entry.lseek_eintr > 0) {
        --entry.lseek_eintr;
        errno = EINTR;
        return -1;
    }
    if (entry.fail_lseek) {
        errno = entry.fail_lseek;
        return -1;
    }
    if (entry.positive_lseek) return 1;
    const off_t result = __real_lseek(fd, offset, whence);
    if (result == 0) entry.pass_reads = 0;
    return result;
}

extern "C" ssize_t __wrap_read(int fd, void* buffer, size_t size)
{
    const auto found = tracked.find(fd);
    if (found == tracked.end()) return __real_read(fd, buffer, size);
    TrackedFd& entry = found->second;
    if (entry.read_eintr > 0) {
        --entry.read_eintr;
        errno = EINTR;
        return -1;
    }
    if (entry.fail_read_after >= 0 &&
        entry.pass_reads >= static_cast<unsigned>(entry.fail_read_after)) {
        entry.fail_read_after = -1;
        errno = EIO;
        return -1;
    }
    const ssize_t count = __real_read(fd, buffer, size);
    if (count >= 0) {
        ++entry.reads;
        ++entry.pass_reads;
        ++total_reads;
    }
    return count;
}

extern "C" int __wrap_close(int fd)
{
    const auto found = tracked.find(fd);
    if (found != tracked.end()) {
        const auto mapped = path_fds.find(found->second.path);
        if (mapped != path_fds.end() && mapped->second == fd) path_fds.erase(mapped);
        tracked.erase(found);
    }
    return __real_close(fd);
}

// ---------------------------------------------------------------------------
// Assertions and helpers.
// ---------------------------------------------------------------------------

void require(bool condition, const std::string& what)
{
    if (!condition) throw std::runtime_error(what);
}

template <class F>
void expect_throw(F&& fn, const char* needle, const std::string& label)
{
    try {
        fn();
    } catch (const std::exception& error) {
        const std::string message = error.what();
        if (message.find(needle) == std::string::npos)
            throw std::runtime_error(label + ": expected '" + needle + "', got: " + message);
        return;
    }
    throw std::runtime_error(label + ": expected a failure");
}

TrackedFd& tracked_for(pid_t tid)
{
    const auto path = path_fds.find(status_path(tid));
    if (path == path_fds.end()) throw std::runtime_error("status path is not open");
    const auto found = tracked.find(path->second);
    if (found == tracked.end()) throw std::runtime_error("status fd is not tracked");
    return found->second;
}

int fd_for(pid_t tid)
{
    const auto path = path_fds.find(status_path(tid));
    if (path == path_fds.end()) throw std::runtime_error("status path is not open");
    return path->second;
}

bool has_cloexec(pid_t tid)
{
    const int flags = fcntl(fd_for(tid), F_GETFD);
    return flags >= 0 && (flags & FD_CLOEXEC);
}

bool is_closed(int fd)
{
    errno = 0;
    return fcntl(fd, F_GETFD) == -1 && errno == EBADF;
}

std::string fd_target(pid_t tid)
{
    const std::string link = "/proc/self/fd/" + std::to_string(fd_for(tid));
    char buffer[512];
    const ssize_t size = readlink(link.c_str(), buffer, sizeof(buffer));
    if (size < 0) throw std::runtime_error("Cannot read fd target");
    return std::string(buffer, static_cast<size_t>(size));
}

void require_fresh_check(const PendingStatusReader& reader, pid_t tid,
                         unsigned min_reads, unsigned expected_lseeks = 1)
{
    TrackedFd& entry = tracked_for(tid);
    const unsigned opens = status_opens;
    const unsigned lseeks = entry.lseeks;
    const unsigned reads = entry.reads;
    reader.require_no_pending(tid);
    require(status_opens == opens, "successful check reopened status");
    require(entry.lseeks == lseeks + expected_lseeks, "successful check did not rewind exactly once");
    require(entry.reads >= reads + min_reads, "successful check did not read to EOF");
}

void require_fresh_throw(const PendingStatusReader& reader, pid_t tid,
                         const char* needle, unsigned min_reads,
                         const std::string& label)
{
    TrackedFd& entry = tracked_for(tid);
    const unsigned opens = status_opens;
    const unsigned lseeks = entry.lseeks;
    const unsigned reads = entry.reads;
    expect_throw([&] { reader.require_no_pending(tid); }, needle, label);
    require(status_opens == opens, label + ": failure reopened status");
    require(entry.lseeks == lseeks + 1, label + ": failure did not rewind exactly once");
    require(entry.reads >= reads + min_reads, label + ": failure stopped before EOF");
}

unsigned minimum_reads(const std::string& text)
{
    return static_cast<unsigned>((text.size() + 4095) / 4096) + 1;
}

const char* const kZeroStatus =
    "SigPnd:\t0000000000000000\nShdPnd:\t0000000000000000\n";

// ---------------------------------------------------------------------------
// Parser acceptance contract. Expected verdicts were produced by running the
// archived istream/istringstream parser on the same input, not by mirroring
// the new implementation.
// ---------------------------------------------------------------------------

void run_parser_case(const char* label, const std::string& text, bool accept,
                     const char* needle)
{
    PendingStatusReader reader;
    const pid_t tid = add_fake_status(text);
    const unsigned opens = status_opens;
    reader.retain(tid);
    require(status_opens == opens + 1, std::string(label) + ": retain did not open once");
    const unsigned expected = minimum_reads(text);
    if (accept) require_fresh_check(reader, tid, expected);
    else require_fresh_throw(reader, tid, needle, expected, label);
}

void test_parser_contract()
{
    std::printf("[ RUN  ] parser contract\n");
    run_parser_case("valid tabs", kZeroStatus, true, nullptr);
    run_parser_case("no final newline",
                    "SigPnd:\t0\nShdPnd:\t0", true, nullptr);
    run_parser_case("duplicate SigPnd",
                    "SigPnd:\t0\nSigPnd:\t0\nShdPnd:\t0\n", true, nullptr);
    run_parser_case("duplicate ShdPnd",
                    "SigPnd:\t0\nShdPnd:\t0\nShdPnd:\t0\n", true, nullptr);
    run_parser_case("CRLF", "SigPnd:\t0\r\nShdPnd:\t0\r\n", true, nullptr);
    run_parser_case("plus zero", "SigPnd:\t+0\nShdPnd:\t0\n", true, nullptr);
    run_parser_case("minus zero", "SigPnd:\t-0\nShdPnd:\t0\n", true, nullptr);
    run_parser_case("hex zero", "SigPnd:\t0x0\nShdPnd:\t0X0\n", true, nullptr);
    run_parser_case("signed hex zero", "SigPnd:\t+0x0\nShdPnd:\t-0x0\n", true, nullptr);
    run_parser_case("long zero", "SigPnd:\t0x0000000000000000\nShdPnd:\t0\n", true, nullptr);
    run_parser_case("vertical whitespace", "SigPnd:\v0\f\nShdPnd:\t 0 \n", true, nullptr);
    run_parser_case("blank and trailing lines",
                    "\nSigPnd:\t0\n\nShdPnd:\t0\nExtra: 1\n", true, nullptr);
    run_parser_case("long ignored line",
                    "Name:\t" + std::string(20000, 'a') + "\n" + kZeroStatus,
                    true, nullptr);
    run_parser_case("long ignored line between fields",
                    "SigPnd:\t0\nName:\t" + std::string(20000, 'b') +
                        "\nShdPnd:\t0\n",
                    true, nullptr);

    // Required lines deliberately straddle 4096-byte read boundaries.
    std::string spanning;
    spanning.append(4091, 'a');
    spanning += '\n'; // SigPnd prefix starts at 4092 and crosses 4096
    spanning += "SigPnd:\t0\n";
    const size_t target = 3 * 4096 - 4;
    require(spanning.size() < target, "chunk fixture is too short");
    spanning.append(target - spanning.size() - 1, 'b');
    spanning += '\n'; // ShdPnd prefix starts at 12284 and crosses 12288
    spanning += "ShdPnd:\t0\n";
    run_parser_case("required lines span chunks", spanning, true, nullptr);

    run_parser_case("empty text", "", false, "Cannot read complete");
    run_parser_case("only SigPnd", "SigPnd:\t0\n", false, "Cannot read complete");
    run_parser_case("only ShdPnd", "ShdPnd:\t0\n", false, "Cannot read complete");
    run_parser_case("leading space before field", " SigPnd:\t0\nShdPnd:\t0\n",
                    false, "Cannot read complete");
    run_parser_case("empty value", "SigPnd:\t\nShdPnd:\t0\n",
                    false, "Cannot parse");
    run_parser_case("bare prefix", "SigPnd:\t0x\nShdPnd:\t0\n",
                    false, "Cannot parse");
    run_parser_case("bad hex digit", "SigPnd:\t0xg\nShdPnd:\t0\n",
                    false, "Cannot parse");
    run_parser_case("missing prefix", "SigPnd:\tx1\nShdPnd:\t0\n",
                    false, "Cannot parse");
    run_parser_case("overflow", "SigPnd:\t0x10000000000000000\nShdPnd:\t0\n",
                    false, "Cannot parse");
    run_parser_case("trailing junk after zero", "SigPnd:\t0 extra\nShdPnd:\t0\n",
                    false, "Invalid pending thread signal bitmap");
    run_parser_case("trailing junk after nonzero", "SigPnd:\t1 x\nShdPnd:\t0\n",
                    false, "Invalid pending thread signal bitmap");
    run_parser_case("pending decimal", "SigPnd:\t1\nShdPnd:\t0\n",
                    false, "pending signal");
    run_parser_case("pending plus", "SigPnd:\t+1\nShdPnd:\t0\n",
                    false, "pending signal");
    run_parser_case("pending minus", "SigPnd:\t-1\nShdPnd:\t0\n",
                    false, "pending signal");
    run_parser_case("pending hex", "SigPnd:\t0x1\nShdPnd:\t0\n",
                    false, "pending signal");
    run_parser_case("pending signed hex", "SigPnd:\t-0x1\nShdPnd:\t0\n",
                    false, "pending signal");
    run_parser_case("pending hex letters", "SigPnd:\t0b101\nShdPnd:\t0\n",
                    false, "pending signal");
    run_parser_case("pending max", "SigPnd:\t0xffffffffffffffff\nShdPnd:\t0\n",
                    false, "pending signal");
    run_parser_case("shared pending", "SigPnd:\t0\nShdPnd:\t1\n",
                    false, "pending signal");
    run_parser_case("pending with CR", "SigPnd:\t1\r\nShdPnd:\t0\n",
                    false, "pending signal");
}

// ---------------------------------------------------------------------------
// Retained-FD I/O, lifecycle, and exact-set behavior.
// ---------------------------------------------------------------------------

void test_reader_io_and_lifecycle()
{
    std::printf("[ RUN  ] reader I/O and lifecycle\n");
    // The fresh-open fallback (used by sigprocmask/anchor checks) is not
    // instrumented here: libstdc++ ifstream does not reach --wrap=open. Its
    // real /proc happy path is covered by thread_control_integration; only the
    // missing-status open failure is asserted directly.
    expect_throw([] { require_no_pending_signals(987654321); },
                 "Cannot inspect pending", "fresh open failure");

    PendingStatusReader reader;
    std::string text = kZeroStatus;
    text += "Name:\t" + std::string(6000, 'z') + "\n";
    const pid_t tid = add_fake_status(text);
    const unsigned opens = status_opens;
    reader.retain(tid);
    require(status_opens == opens + 1, "retain did not open once");
    require(reader.size() == 1 && reader.contains(tid), "retain did not record descriptor");
    require(has_cloexec(tid), "retained descriptor is not close-on-exec");

    require_fresh_check(reader, tid, minimum_reads(text));
    require_fresh_check(reader, tid, minimum_reads(text));
    require(status_opens == opens + 1, "repeated checks reopened status");

    // Rewind errors must fail before any read and must not close/reopen.
    TrackedFd& entry = tracked_for(tid);
    entry.fail_lseek = EIO;
    require_fresh_throw(reader, tid, "Cannot rewind", 0, "lseek EIO");
    entry.fail_lseek = 0;
    entry.positive_lseek = true;
    require_fresh_throw(reader, tid, "Cannot rewind", 0, "positive lseek");
    entry.positive_lseek = false;
    entry.lseek_eintr = 1;
    require_fresh_check(reader, tid, minimum_reads(text), 2);
    entry.read_eintr = 1;
    require_fresh_check(reader, tid, minimum_reads(text));
    require(status_opens == opens + 1, "retry path reopened status");

    // Both required fields are in the first block; a later read error must
    // still reject instead of accepting the already-seen fields.
    entry.fail_read_after = 1;
    require_fresh_throw(reader, tid, "Cannot read complete", 1, "trailing read EIO");
    entry.fail_read_after = 0;
    require_fresh_throw(reader, tid, "Cannot read complete", 0, "first read EIO");
    require_fresh_check(reader, tid, minimum_reads(text));
    require(status_opens == opens + 1, "post-error check reopened status");

    // A short/EOF-only stream is rejected, not treated as valid empty status.
    PendingStatusReader incomplete;
    const pid_t bad = add_fake_status("SigPnd:\t0\n");
    incomplete.retain(bad);
    require_fresh_throw(incomplete, bad, "Cannot read complete", minimum_reads("SigPnd:\t0\n"),
                        "missing field at EOF");

    // Retain failures do not leak a descriptor or open a duplicate tid.
    const unsigned before_bad = status_opens;
    expect_throw([&] { incomplete.retain(0); }, "Invalid or duplicate", "retain invalid tid");
    expect_throw([&] { incomplete.retain(bad); }, "Invalid or duplicate", "retain duplicate");
    require(status_opens == before_bad, "invalid/duplicate retain opened status");
    const unsigned before_missing = status_opens;
    expect_throw([&] { incomplete.retain(999999999); }, "Cannot retain", "retain missing tid");
    require(status_opens == before_missing, "failed retain counted a status open");
    require(incomplete.size() == 1, "failed retain changed descriptor set");

    const unsigned all_opens = status_opens;
    const unsigned all_lseeks = total_lseeks;
    const unsigned all_reads = total_reads;
    expect_throw([&] { reader.require_no_pending(999999999); },
                 "no retained status descriptor", "unknown tid");
    require(status_opens == all_opens && total_lseeks == all_lseeks &&
                total_reads == all_reads,
            "unknown tid performed status I/O");
}

void test_move_and_destruction()
{
    std::printf("[ RUN  ] move and destruction\n");
    int first_fd = -1, second_fd = -1, replaced_fd = -1;
    {
        PendingStatusReader source;
        const pid_t first = add_fake_status(kZeroStatus);
        const pid_t second = add_fake_status(kZeroStatus);
        source.retain(first);
        source.retain(second);
        first_fd = fd_for(first);
        second_fd = fd_for(second);
        require(source.size() == 2, "source size mismatch");

        PendingStatusReader moved(std::move(source));
        require(moved.size() == 2 && moved.contains(first) && moved.contains(second),
                "move construction lost descriptors");
        require(!is_closed(first_fd) && !is_closed(second_fd),
                "move construction closed descriptors");
        require_fresh_check(moved, first, minimum_reads(kZeroStatus));

        PendingStatusReader target;
        const pid_t replaced = add_fake_status(kZeroStatus);
        target.retain(replaced);
        replaced_fd = fd_for(replaced);
        target = std::move(moved);
        require(is_closed(replaced_fd), "move assignment did not close old descriptor");
        require(target.size() == 2 && target.contains(first) && target.contains(second),
                "move assignment lost descriptors");
        require_fresh_check(target, second, minimum_reads(kZeroStatus));
    }
    require(is_closed(first_fd) && is_closed(second_fd),
            "reader destruction did not close retained descriptors");
    require(is_closed(replaced_fd), "move-assignment replacement fd was reopened");
}

void test_exact_set_rejected_without_io()
{
    std::printf("[ RUN  ] exact retained set\n");
    PendingStatusReader reader;
    const pid_t first = add_fake_status(kZeroStatus);
    const pid_t second = add_fake_status(kZeroStatus);
    const pid_t absent = next_fake_tid++;
    reader.retain(first);
    reader.retain(second);
    const unsigned opens = status_opens;
    const unsigned lseeks = total_lseeks;
    const unsigned reads = total_reads;

    pid_t short_set[] = {first};
    expect_throw([&] { verify_stopped_thread_set(1, short_set, reader); },
                 "descriptor set differs", "short retained set");
    pid_t long_set[] = {first, second, absent};
    expect_throw([&] { verify_stopped_thread_set(1, long_set, reader); },
                 "descriptor set differs", "long retained set");
    pid_t missing_member[] = {first, absent};
    expect_throw([&] { verify_stopped_thread_set(1, missing_member, reader); },
                 "no retained status descriptor", "retained set missing member");
    require(status_opens == opens && total_lseeks == lseeks && total_reads == reads,
            "exact-set rejection performed status I/O");
}

// ---------------------------------------------------------------------------
// Real stopped leader + worker: thread-directed and shared pending bits are
// re-read after every set/clear; dead tasks never trigger a reopen.
// ---------------------------------------------------------------------------

struct WorkerArgs {
    int ready_fd;
    int ack_fd;
};

void* worker_main(void* raw)
{
    WorkerArgs* args = static_cast<WorkerArgs*>(raw);
    const pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
    if (write(args->ready_fd, &tid, sizeof(tid)) != static_cast<ssize_t>(sizeof(tid)))
        return nullptr;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    int consumed = 0;
    for (;;) {
        int signal = 0;
        if (sigwait(&set, &signal) != 0) break;
        ++consumed;
        const char byte = consumed == 1 ? 'W' : 'X';
        if (write(args->ack_fd, &byte, 1) != 1) break;
        if (consumed > 1) break;
    }
    return nullptr;
}

[[noreturn]] void child_main(int ready_fd, int ack_fd)
{
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) == -1) _exit(11);
    if (getppid() == 1) _exit(12);
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    sigaddset(&blocked, SIGUSR2);
    if (pthread_sigmask(SIG_BLOCK, &blocked, nullptr) != 0) _exit(13);
    WorkerArgs args{ready_fd, ack_fd};
    pthread_t worker;
    if (pthread_create(&worker, nullptr, worker_main, &args) != 0) _exit(14);
    sigset_t leader_set;
    sigemptyset(&leader_set);
    sigaddset(&leader_set, SIGUSR1);
    for (;;) {
        int signal = 0;
        if (sigwait(&leader_set, &signal) != 0) _exit(15);
        const char byte = 'L';
        if (write(ack_fd, &byte, 1) != 1) _exit(16);
    }
}

int read_byte(int fd, int timeout_ms, const char* what)
{
    pollfd ready{fd, POLLIN, 0};
    int result;
    do { result = poll(&ready, 1, timeout_ms); } while (result < 0 && errno == EINTR);
    if (result != 1) throw std::runtime_error(std::string("timed out waiting for ") + what);
    char byte = 0;
    ssize_t count;
    do { count = read(fd, &byte, 1); } while (count < 0 && errno == EINTR);
    if (count != 1) throw std::runtime_error(std::string("failed waiting for ") + what);
    return byte;
}

pid_t read_tid(int fd)
{
    pollfd ready{fd, POLLIN, 0};
    int result;
    do { result = poll(&ready, 1, 5000); } while (result < 0 && errno == EINTR);
    if (result != 1) throw std::runtime_error("timed out waiting for worker tid");
    pid_t tid = 0;
    ssize_t count;
    do { count = read(fd, &tid, sizeof(tid)); } while (count < 0 && errno == EINTR);
    if (count != static_cast<ssize_t>(sizeof(tid)))
        throw std::runtime_error("failed reading worker tid");
    return tid;
}

struct RealFixture {
    pid_t pid = 0;
    int ack_fd = -1;
    bool reaped = false;
    ~RealFixture()
    {
        if (ack_fd >= 0) close(ack_fd);
        if (pid <= 0) return;
        kill(pid, SIGKILL);
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
};

void stop_group(pid_t pid)
{
    if (kill(pid, SIGSTOP) == -1) throw std::runtime_error("SIGSTOP failed");
    int status = 0;
    pid_t result;
    do { result = waitpid(pid, &status, WUNTRACED); } while (result < 0 && errno == EINTR);
    if (result != pid || !WIFSTOPPED(status))
        throw std::runtime_error("child did not enter group stop");
}

void resume_group(pid_t pid)
{
    if (kill(pid, SIGCONT) == -1) throw std::runtime_error("SIGCONT failed");
}

extern "C" DIR* __real_opendir(const char*);
extern "C" void __real_rewinddir(DIR*);
extern "C" dirent* __real_readdir(DIR*);
extern "C" int __real_closedir(DIR*);
extern "C" DIR* __wrap_opendir(const char* path) {
    DIR* directory = __real_opendir(path);
    if (directory) ++task_directory_opens;
    return directory;
}
extern "C" void __wrap_rewinddir(DIR* directory) {
    if (fail_task_rewind) { fail_task_rewind = false; errno = EIO; return; }
    if (!omit_task_rewind) __real_rewinddir(directory);
}
extern "C" dirent* __wrap_readdir(DIR* directory) {
    if (fail_task_read) { fail_task_read = false; errno = EIO; return nullptr; }
    return __real_readdir(directory);
}
extern "C" int __wrap_closedir(DIR* directory) {
    ++task_directory_closes;
    return __real_closedir(directory);
}

void test_real_child_pending_state()
{
    std::printf("[ RUN  ] real leader/worker pending state\n");
    int ready_pipe[2] = {-1, -1};
    int ack_pipe[2] = {-1, -1};
    require(pipe(ready_pipe) == 0 && pipe(ack_pipe) == 0, "pipe failed");
    const pid_t pid = fork();
    if (pid < 0) throw std::runtime_error("fork failed");
    if (pid == 0) {
        close(ready_pipe[0]);
        close(ack_pipe[0]);
        child_main(ready_pipe[1], ack_pipe[1]);
    }
    close(ready_pipe[1]);
    close(ack_pipe[1]);
    RealFixture fixture;
    fixture.pid = pid;
    fixture.ack_fd = ack_pipe[0];
    PendingStatusReader reader;
    try {
        const pid_t worker = read_tid(ready_pipe[0]);
        close(ready_pipe[0]);
        ready_pipe[0] = -1;
        require(worker > 0 && worker != pid, "worker tid is invalid");
        stop_group(pid);

        const unsigned opens = status_opens;
        reader.retain(pid);
        reader.retain(worker);
        std::vector<pid_t> members{pid, worker};
        std::sort(members.begin(), members.end());
        const auto directory_opens = task_directory_opens;
        require(reader.thread_ids(pid) == members, "cold task membership");
        require(reader.thread_ids(pid) == members, "warm task membership must rewind");
        require(task_directory_opens == directory_opens + 1, "task directory reopened");
        fail_task_rewind = true;
        expect_throw([&] { reader.thread_ids(pid); }, "rewind", "task rewind failure");
        fail_task_read = true;
        expect_throw([&] { reader.thread_ids(pid); }, "enumerate", "task read failure");
        expect_throw([&] { reader.thread_ids(getpid()); }, "another process", "foreign task directory");
        require(reader.thread_ids(pid) == members && task_directory_opens == directory_opens + 1,
                "task directory errors must not reopen or retain results");
        PendingStatusReader moved(std::move(reader));
        reader = std::move(moved);
        require(reader.thread_ids(pid) == members && task_directory_opens == directory_opens + 1,
                "task directory move keeps its owned stream");
        require(status_opens == opens + 2, "real retain count mismatch");
        require(reader.size() == 2, "real reader size mismatch");
        require(has_cloexec(pid) && has_cloexec(worker), "real descriptors lack CLOEXEC");
        require(fd_target(pid).find("/" + std::to_string(pid)) != std::string::npos,
                "leader descriptor target mismatch");
        require(fd_target(worker).find("/" + std::to_string(worker)) != std::string::npos,
                "worker descriptor target mismatch");

        require_fresh_check(reader, pid, 2);
        require_fresh_check(reader, worker, 2);

        // Thread-directed pending signal: only the worker sees SigPnd.
        if (syscall(SYS_tgkill, pid, worker, SIGUSR2) != 0)
            throw std::runtime_error("tgkill failed");
        require_fresh_throw(reader, worker, "pending signal", 2, "worker SigPnd");
        require_fresh_check(reader, pid, 2);
        resume_group(pid);
        require(read_byte(ack_pipe[0], 5000, "worker consume") == 'W',
                "worker did not consume SIGUSR2");
        stop_group(pid);
        require_fresh_check(reader, worker, 2);
        require_fresh_check(reader, pid, 2);

        // Process-directed signal: ShdPnd is visible from both threads.
        if (kill(pid, SIGUSR1) == -1) throw std::runtime_error("kill SIGUSR1 failed");
        require_fresh_throw(reader, pid, "pending signal", 2, "leader ShdPnd");
        require_fresh_throw(reader, worker, "pending signal", 2, "worker ShdPnd");
        resume_group(pid);
        require(read_byte(ack_pipe[0], 5000, "leader consume") == 'L',
                "leader did not consume SIGUSR1");
        stop_group(pid);
        require_fresh_check(reader, pid, 2);
        require_fresh_check(reader, worker, 2);

        const unsigned before_death = status_opens;
        if (syscall(SYS_tgkill, pid, worker, SIGUSR2) != 0)
            throw std::runtime_error("second tgkill failed");
        resume_group(pid);
        require(read_byte(ack_pipe[0], 5000, "worker exit") == 'X',
                "worker did not acknowledge exit");
        // X is written before pthread returns. Stopping the group immediately
        // can park the worker before its exit syscall, so wait for the kernel
        // to remove the task before asserting that its retained fd is dead.
        const auto worker_path = "/proc/" + std::to_string(pid) + "/task/" + std::to_string(worker);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (access(worker_path.c_str(), F_OK) == 0) {
            require(std::chrono::steady_clock::now() < deadline, "worker did not leave task set");
            usleep(1000);
        }
        require(errno == ENOENT, "cannot establish worker exit");
        stop_group(pid);
        require(reader.thread_ids(pid) == std::vector<pid_t>{pid}, "task membership retained dead worker");
        require_fresh_throw(reader, worker, "Cannot ", 0, "worker dead first");
        require_fresh_throw(reader, worker, "Cannot ", 0, "worker dead second");
        require_fresh_check(reader, pid, 2);
        require(status_opens == before_death, "worker death triggered status reopen");
        require(!is_closed(fd_for(worker)), "worker death closed retained descriptor");

        // Reap the leader: the old descriptor stays bound to the dead task and
        // must keep failing, while its fd is retained until reader destruction.
        kill(pid, SIGKILL);
        int status = 0;
        pid_t reaped;
        do { reaped = waitpid(pid, &status, 0); } while (reaped < 0 && errno == EINTR);
        require(reaped == pid, "leader reap failed");
        fixture.reaped = true;
        fixture.pid = 0;
        const unsigned before_leader_death = status_opens;
        require_fresh_throw(reader, pid, "Cannot ", 0, "leader dead first");
        require_fresh_throw(reader, pid, "Cannot ", 0, "leader dead second");
        require(status_opens == before_leader_death, "leader death triggered status reopen");
        require(!is_closed(fd_for(pid)), "leader death closed retained descriptor");
    } catch (...) {
        if (ready_pipe[0] >= 0) close(ready_pipe[0]);
        throw;
    }
}

} // namespace

int main(int argc, char** argv)
{
    omit_task_rewind = argc == 2 && !strcmp(argv[1], "--omit-rewind");
    alarm(60); // child has PDEATHSIG; a wedged fixture must not hang make
    try {
        test_parser_contract();
        test_reader_io_and_lifecycle();
        test_move_and_destruction();
        test_exact_set_rejected_without_io();
        test_real_child_pending_state();
        require(task_directory_opens == task_directory_closes, "retained task directory leaked");
        std::printf("pending_status_reader_integration: all checks passed\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "pending_status_reader_integration: FAILED: %s\n", error.what());
        return 1;
    }
}
