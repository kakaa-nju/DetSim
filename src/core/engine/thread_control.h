#ifndef DETSIM_THREAD_CONTROL_H
#define DETSIM_THREAD_CONTROL_H

#include <span>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>
#include <dirent.h>
#include <string>
#include <sys/types.h>

// Retained /proc status and task-directory handles for one physical carrier pool.
//
// Each descriptor is opened immediately after the fresh-open ownership gate
// succeeds and stays bound to that physical tid until the pool is cleared.
// require_no_pending() only rewinds and reads the retained descriptor; it
// never reopens by numeric tid, so a recycled tid cannot be mistaken for the
// task that owns the pool. The reader is used by the serial control thread;
// its scratch buffer is reused across checks and is not shared between pools.
// The task directory is also rewound and fully enumerated on every query;
// only its handle is retained, never the thread membership list.
class PendingStatusReader {
public:
    PendingStatusReader() = default;
    PendingStatusReader(const PendingStatusReader&) = delete;
    PendingStatusReader& operator=(const PendingStatusReader&) = delete;
    PendingStatusReader(PendingStatusReader&&) noexcept = default;
    PendingStatusReader& operator=(PendingStatusReader&&) noexcept = default;
    ~PendingStatusReader() = default;

    // Cold path: called after the fresh-open thread-set barrier has succeeded.
    void retain(pid_t tid);
    // The directory handle is retained, but membership is reread on every call.
    std::vector<pid_t> thread_ids(pid_t main_pid) const;
    // Hot path: lseek(0) + read to EOF + parse on every call.
    void require_no_pending(pid_t tid) const;
    bool contains(pid_t tid) const { return descriptors_.contains(tid); }
    std::size_t size() const { return descriptors_.size(); }

private:
    struct OwnedFd {
        int fd;
        explicit OwnedFd(int value);
        ~OwnedFd();
        OwnedFd(const OwnedFd&) = delete;
        OwnedFd& operator=(const OwnedFd&) = delete;
        OwnedFd(OwnedFd&& other) noexcept;
        OwnedFd& operator=(OwnedFd&& other) noexcept;
    };

    std::map<pid_t, OwnedFd> descriptors_;
    mutable std::string text_;
    struct CloseDirectory { void operator()(DIR*) const noexcept; };
    mutable std::unique_ptr<DIR, CloseDirectory> task_directory_;
    mutable pid_t group_pid_ = 0;
};

// Read-only precondition for capture and restore. The caller must keep every
// verified thread stopped until the protected operation has completed.
void verify_stopped_thread_set(pid_t main_pid, std::span<const pid_t> expected);
// Variant for pools with retained status descriptors. The retained tid set
// must equal the expected carrier set exactly; no fallback open is performed.
void verify_stopped_thread_set(pid_t main_pid, std::span<const pid_t> expected,
                               const PendingStatusReader& reader);

uint64_t read_thread_signal_mask(pid_t tid);
void write_thread_signal_mask(pid_t tid, uint64_t mask);
void require_no_pending_signals(pid_t tid);
// The modeled kernel never supports rseq; active native registration is an error.
void require_no_rseq(pid_t tid);
uint64_t read_thread_robust_list(pid_t tid);
void require_no_native_robust_list(pid_t tid);

#endif
