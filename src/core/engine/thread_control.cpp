#include "thread_control.h"
#include "utils.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <fstream>
#include <fcntl.h>
#include <utility>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <vector>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

static void verify_stopped_threads(pid_t main_pid, std::span<const pid_t> expected,
                                   const PendingStatusReader* reader)
{
    if (main_pid <= 0 || expected.empty())
        throw std::runtime_error("Thread stop barrier requires a process and a nonempty thread set");
    std::vector<pid_t> threads(expected.begin(), expected.end());
    std::sort(threads.begin(), threads.end());
    if (threads.front() <= 0 ||
        std::adjacent_find(threads.begin(), threads.end()) != threads.end())
        throw std::runtime_error("Thread stop barrier has invalid or duplicate thread IDs");
    if (reader) {
        // A retained descriptor set is only usable for the exact physical
        // carrier set it was opened for. Never fall back to a fresh open.
        if (reader->size() != threads.size())
            throw std::runtime_error("Thread status descriptor set differs from carrier set");
        for (pid_t tid : threads)
            if (!reader->contains(tid))
                throw std::runtime_error("Thread carrier has no retained status descriptor");
    }
    if ((reader ? reader->thread_ids(main_pid) : get_thread_list(main_pid)) != threads)
        throw std::runtime_error("Thread stop barrier found a different physical thread set");
    for (pid_t tid : threads) {
        user_regs_struct regs{};
        if (ptrace(PTRACE_GETREGS, tid, nullptr, &regs) == -1) {
            const int error = errno;
            throw std::runtime_error("Thread stop barrier cannot inspect stopped thread " +
                                     std::to_string(tid) + ": " + std::strerror(error));
        }
    }
    for (pid_t tid : threads) {
        if (reader) reader->require_no_pending(tid);
        else require_no_pending_signals(tid);
        require_no_rseq(tid);
        require_no_native_robust_list(tid);
    }
    // Detect a concurrent clone/exit between enumeration and register checks.
    if ((reader ? reader->thread_ids(main_pid) : get_thread_list(main_pid)) != threads)
        throw std::runtime_error("Thread stop barrier thread set changed during verification");
}

void verify_stopped_thread_set(pid_t main_pid, std::span<const pid_t> expected)
{
    verify_stopped_threads(main_pid, expected, nullptr);
}

void verify_stopped_thread_set(pid_t main_pid, std::span<const pid_t> expected,
                               const PendingStatusReader& reader)
{
    verify_stopped_threads(main_pid, expected, &reader);
}

uint64_t read_thread_signal_mask(pid_t tid)
{
    uint64_t mask = 0;
    if (ptrace(PTRACE_GETSIGMASK, tid, reinterpret_cast<void*>(sizeof(mask)), &mask) == -1)
        throw std::runtime_error("Cannot read stopped thread signal mask");
    return mask;
}

void write_thread_signal_mask(pid_t tid, uint64_t mask)
{
    if (ptrace(PTRACE_SETSIGMASK, tid, reinterpret_cast<void*>(sizeof(mask)), &mask) == -1)
        throw std::runtime_error("Cannot restore stopped thread signal mask");
}

namespace {

bool is_status_space(char character)
{
    return character == ' ' || character == '\t' || character == '\n' ||
           character == '\v' || character == '\f' || character == '\r';
}

enum class PendingFieldError { none, malformed, trailing };

// Parse one status bitmap field with the same accepted grammar as the former
// istream/std::hex extraction: optional C whitespace, optional '+'/'-', an
// optional 0x/0X prefix, hexadecimal digits, then only whitespace to EOL.
// "0x" without digits, overflow, and trailing non-whitespace are errors.
PendingFieldError parse_pending_field(std::string_view field, uint64_t& value)
{
    size_t position = 0;
    while (position < field.size() && is_status_space(field[position])) ++position;
    bool negative = false;
    if (position < field.size() && (field[position] == '+' || field[position] == '-')) {
        negative = field[position] == '-';
        ++position;
    }
    if (position + 1 < field.size() && field[position] == '0' &&
        (field[position + 1] == 'x' || field[position + 1] == 'X'))
        position += 2;
    uint64_t magnitude = 0;
    const char* const begin = field.data() + position;
    const char* const end = field.data() + field.size();
    const auto parsed = std::from_chars(begin, end, magnitude, 16);
    if (parsed.ec != std::errc{} || parsed.ptr == begin)
        return PendingFieldError::malformed;
    for (const char* tail = parsed.ptr; tail != end; ++tail)
        if (!is_status_space(*tail)) return PendingFieldError::trailing;
    value = negative ? uint64_t{0} - magnitude : magnitude;
    return PendingFieldError::none;
}

// Shared by the fresh-open and retained-descriptor paths. The caller must
// have read the status text to EOF already; parsing never performs I/O.
void parse_pending_signals(std::string_view text)
{
    bool thread_seen = false, shared_seen = false;
    for (;;) {
        const size_t newline = text.find('\n');
        const std::string_view line = text.substr(0, newline);
        const bool thread_field = line.starts_with("SigPnd:");
        const bool shared_field = line.starts_with("ShdPnd:");
        if (thread_field || shared_field) {
            uint64_t pending = 0;
            const auto error = parse_pending_field(line.substr(7), pending);
            if (error == PendingFieldError::malformed)
                throw std::runtime_error("Cannot parse pending thread signals");
            if (error == PendingFieldError::trailing)
                throw std::runtime_error("Invalid pending thread signal bitmap");
            if (pending)
                throw std::runtime_error("Unsupported pending signal at thread stop barrier");
            if (thread_field) thread_seen = true;
            else shared_seen = true;
        }
        if (newline == std::string_view::npos) break;
        text.remove_prefix(newline + 1);
    }
    if (!thread_seen || !shared_seen)
        throw std::runtime_error("Cannot read complete pending thread signal state");
}

} // namespace

void require_no_pending_signals(pid_t tid)
{
    // PEEKSIGINFO only exposes queued siginfo objects. The kernel can also
    // have a pending bitmap bit without an allocated sigqueue entry.
    std::ifstream status("/proc/" + std::to_string(tid) + "/status");
    if (!status) throw std::runtime_error("Cannot inspect pending thread signals");
    // Read the whole stream before deciding. A later read failure must not be
    // hidden by already having seen both zero-valued fields.
    std::string text;
    char buffer[4096];
    for (;;) {
        status.read(buffer, sizeof buffer);
        const std::streamsize count = status.gcount();
        if (count > 0) text.append(buffer, static_cast<size_t>(count));
        // Check badbit before accepting eofbit: a read error must not be
        // masked by a stream that also reports end-of-file.
        if (status.bad())
            throw std::runtime_error("Cannot read complete pending thread signal state");
        if (status.eof()) break;
        if (!status)
            throw std::runtime_error("Cannot read complete pending thread signal state");
    }
    parse_pending_signals(text);
}

void PendingStatusReader::CloseDirectory::operator()(DIR* directory) const noexcept {
    if (directory) closedir(directory);
}

std::vector<pid_t> PendingStatusReader::thread_ids(pid_t main_pid) const {
    if (main_pid <= 0 || (task_directory_ && group_pid_ != main_pid))
        throw std::runtime_error("Retained task directory belongs to another process");
    if (!task_directory_) {
        const auto path = "/proc/" + std::to_string(main_pid) + "/task";
        std::unique_ptr<DIR, CloseDirectory> directory(opendir(path.c_str()));
        if (!directory) throw std::runtime_error("Cannot retain task directory");
        const int flags = fcntl(dirfd(directory.get()), F_GETFD);
        if (flags < 0 || !(flags & FD_CLOEXEC))
            throw std::runtime_error("Cannot retain close-on-exec task directory");
        group_pid_ = main_pid;
        task_directory_ = std::move(directory);
    }
    return get_thread_list(main_pid, task_directory_.get());
}

PendingStatusReader::OwnedFd::OwnedFd(int value) : fd(value) {}
PendingStatusReader::OwnedFd::~OwnedFd() { if (fd >= 0) close(fd); }
PendingStatusReader::OwnedFd::OwnedFd(OwnedFd&& other) noexcept
    : fd(std::exchange(other.fd, -1)) {}
PendingStatusReader::OwnedFd& PendingStatusReader::OwnedFd::operator=(OwnedFd&& other) noexcept {
    if (this != &other) {
        if (fd >= 0) close(fd);
        fd = std::exchange(other.fd, -1);
    }
    return *this;
}

void PendingStatusReader::retain(pid_t tid)
{
    if (tid <= 0 || descriptors_.contains(tid))
        throw std::runtime_error("Invalid or duplicate retained thread status descriptor");
    OwnedFd descriptor(open(("/proc/" + std::to_string(tid) + "/status").c_str(),
                            O_RDONLY | O_CLOEXEC));
    const int flags = descriptor.fd < 0 ? -1 : fcntl(descriptor.fd, F_GETFD);
    if (flags < 0 || !(flags & FD_CLOEXEC))
        throw std::runtime_error("Cannot retain close-on-exec thread status descriptor");
    if (!descriptors_.emplace(tid, std::move(descriptor)).second)
        throw std::runtime_error("Duplicate retained thread status descriptor");
}

void PendingStatusReader::require_no_pending(pid_t tid) const
{
    const auto found = descriptors_.find(tid);
    if (found == descriptors_.end())
        throw std::runtime_error("Thread carrier has no retained status descriptor");
    const int fd = found->second.fd;
    off_t position;
    do { position = lseek(fd, 0, SEEK_SET); } while (position < 0 && errno == EINTR);
    if (position != 0) {
        const int error = position < 0 ? errno : EIO;
        throw std::runtime_error("Cannot rewind pending thread signal state for " +
                                 std::to_string(tid) + ": " + std::strerror(error));
    }
    // Read to EOF on every check. /proc status st_size is not authoritative.
    // One scratch buffer per pool (not per carrier), touched only by the serial
    // control thread; its capacity stops growing once it fits the status text.
    text_.clear();
    char buffer[4096];
    for (;;) {
        const auto count = read(fd, buffer, sizeof buffer);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            const int error = errno;
            throw std::runtime_error("Cannot read complete pending thread signal state for " +
                                     std::to_string(tid) + ": " + std::strerror(error));
        }
        if (!count) break;
        text_.append(buffer, static_cast<size_t>(count));
    }
    parse_pending_signals(text_);
}

void require_no_rseq(pid_t tid)
{
  __ptrace_rseq_configuration configuration{};
  const auto size = ptrace(PTRACE_GET_RSEQ_CONFIGURATION, tid,
      reinterpret_cast<void*>(sizeof(configuration)), &configuration);
  if (size != sizeof(configuration))
    throw std::runtime_error("Cannot inspect thread rseq registration: PTRACE_GET_RSEQ_CONFIGURATION is required");
  if (configuration.rseq_abi_pointer || configuration.rseq_abi_size)
    throw std::runtime_error("Unsupported active rseq registration in rseq-unavailable model");
}

uint64_t read_thread_robust_list(pid_t tid)
{
    robust_list_head* head = nullptr;
    size_t length = 0;
    if (syscall(SYS_get_robust_list, tid, &head, &length) != 0 ||
        length != sizeof(robust_list_head))
        throw std::runtime_error("Cannot inspect native thread robust-list registration");
    return reinterpret_cast<uint64_t>(head);
}

void require_no_native_robust_list(pid_t tid)
{
    if (read_thread_robust_list(tid))
        throw std::runtime_error("Thread carrier has unadopted native robust-list registration");
}
