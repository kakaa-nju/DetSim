#include "engine/thread_control.h"
#include "state/state.h"
#include "guest.h"
#include "ncurses_ui.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sched.h>
#include <signal.h>
#include <stdexcept>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/ptrace.h>
#include <sys/rseq.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }

static bool track_capture_resumes=false;
static unsigned capture_resumes=0;
extern "C" long __real_ptrace(enum __ptrace_request, ...);
extern "C" long __wrap_ptrace(enum __ptrace_request request,pid_t pid,void* address,void* data) noexcept {
    if(track_capture_resumes && (request==PTRACE_CONT || request==PTRACE_SYSCALL || request==PTRACE_SINGLESTEP)) {
        ++capture_resumes;
        // A negative oracle must never let the pre-barrier injection run.
        // Ptrace_right reports this outside glibc's noexcept call boundary.
        errno=EIO;return -1;
    }
    return __real_ptrace(request,pid,address,data);
}

// Inject an enumeration failure after all expected entries were returned:
// accepting this prefix would let the physical-thread-set comparison pass.
static int enumeration_fault = 0;
static unsigned enumeration_ends = 0, enumeration_closes = 0;
extern "C" dirent* __real_readdir(DIR*);
extern "C" int __real_closedir(DIR*);
extern "C" dirent* __wrap_readdir(DIR* dir) {
    auto* entry = __real_readdir(dir);
    if (enumeration_fault > 0 && !entry &&
        ++enumeration_ends == static_cast<unsigned>(enumeration_fault)) errno = EIO;
    if (enumeration_fault == -1 && entry) errno = EIO; // stale errno is not EOF failure
    return entry;
}
extern "C" int __wrap_closedir(DIR* dir) {
    if (enumeration_fault) ++enumeration_closes;
    return __real_closedir(dir);
}

static void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

static int stopped(pid_t tid)
{
    int status = 0;
    for (int tick = 0; tick < 500; ++tick) {
        const pid_t result = waitpid(tid, &status, __WALL | WNOHANG);
        if (result == tid) {
            require(WIFSTOPPED(status), "tracee exited instead of stopping");
            return status;
        }
        require(result == 0 || (result < 0 && errno == EINTR), "waitpid tracee");
        usleep(1000);
    }
    throw std::runtime_error("timed out waiting for ptrace stop");
}

template<class F> static void rejected(F action)
{
    bool failed = false;
    try { action(); } catch (const std::runtime_error &) { failed = true; }
    require(failed, "invalid thread barrier unexpectedly passed");
}

alignas(16) static char worker_stack[65536];
static int spin(void *)
{
    for (;;) asm volatile("" ::: "memory");
}

static void run_case()
{
    pid_t child = fork();
    require(child >= 0, "fork tracee");
    if (!child) {
        if (__rseq_size && syscall(SYS_rseq,
                static_cast<char*>(__builtin_thread_pointer()) + __rseq_offset,
                32, RSEQ_FLAG_UNREGISTER, RSEQ_SIG)) _exit(9);
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (syscall(SYS_set_robust_list, nullptr, 24)) _exit(91);
        auto* control=static_cast<unsigned char*>(mmap(reinterpret_cast<void*>(scratch_page),4096,
            PROT_READ|PROT_WRITE|PROT_EXEC,MAP_FIXED_NOREPLACE|MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        if(control==MAP_FAILED) _exit(5);
        memset(control,0xa5,4096);control[0xff0]=0x0f;control[0xff1]=0x05;
        if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr)) _exit(2);
        raise(SIGSTOP);
        // Raw task: libc thread startup would install an unmodeled host list.
        if (clone(spin, worker_stack + sizeof(worker_stack),
                  CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD|CLONE_SYSVSEM,
                  nullptr) < 0) _exit(3);
        for (;;) asm volatile("" ::: "memory");
    }
    stopped(child);
    require(ptrace(PTRACE_SETOPTIONS, child, nullptr,
                   PTRACE_O_TRACECLONE | PTRACE_O_EXITKILL) == 0, "set ptrace options");
    require(ptrace(PTRACE_CONT, child, nullptr, nullptr) == 0, "continue to clone");
    int status = stopped(child);
    require((status >> 16) == PTRACE_EVENT_CLONE, "expected clone event");
    unsigned long event = 0;
    require(ptrace(PTRACE_GETEVENTMSG, child, nullptr, &event) == 0, "clone event message");
    const pid_t worker = static_cast<pid_t>(event);
    stopped(worker);
    // Deliberately reverse enumeration order: the gate compares sets, not slots.
    const std::array<pid_t, 2> tids{worker, child};
    verify_stopped_thread_set(child, tids);
    for (int fault : {1, 2}) {
        enumeration_fault = fault; enumeration_ends = enumeration_closes = 0;
        capture_resumes = 0; track_capture_resumes = true;
        bool failed = false;
        try { verify_stopped_thread_set(child, tids); }
        catch (const std::runtime_error& error) {
            failed = std::string(error.what()).starts_with("Cannot enumerate process threads");
        }
        enumeration_fault = 0; track_capture_resumes = false;
        require(failed, "thread barrier accepted a directory read error");
        require(enumeration_ends == static_cast<unsigned>(fault) &&
                enumeration_closes == static_cast<unsigned>(fault),
                "directory failure leaked a handle or continued enumeration");
        require(capture_resumes == 0, "directory failure resumed a tracee");
    }
    enumeration_fault = -1; enumeration_closes = 0;
    verify_stopped_thread_set(child, tids);
    enumeration_fault = 0;
    require(enumeration_closes == 2, "normal EOF leaked a directory handle");
    PendingStatusReader retained;
    retained.retain(child);
    retained.retain(worker);
    verify_stopped_thread_set(child, tids, retained);
    for (int fault : {1, 2}) {
        enumeration_fault = fault; enumeration_ends = enumeration_closes = 0;
        capture_resumes = 0; track_capture_resumes = true;
        bool failed = false;
        try { verify_stopped_thread_set(child, tids, retained); }
        catch (const std::runtime_error& error) {
            failed = std::string(error.what()).starts_with("Cannot enumerate process threads");
        }
        enumeration_fault = 0; track_capture_resumes = false;
        require(failed && enumeration_ends == static_cast<unsigned>(fault),
                "retained barrier accepted a directory read failure");
        require(enumeration_closes == 0 && capture_resumes == 0,
                "retained barrier closed its stream or resumed after a read failure");
        verify_stopped_thread_set(child, tids, retained);
    }
    rejected([&] { verify_stopped_thread_set(child, {}); });
    rejected([&] { verify_stopped_thread_set(-1, tids); });
    const std::array<pid_t, 2> duplicate{child, child};
    rejected([&] { verify_stopped_thread_set(child, duplicate); });
    const std::array<pid_t, 2> invalid{child, 0};
    rejected([&] { verify_stopped_thread_set(child, invalid); });
    const std::array<pid_t, 1> missing{child};
    rejected([&] { verify_stopped_thread_set(child, missing); });
    const std::array<pid_t, 1> foreign{getpid()};
    rejected([&] { verify_stopped_thread_set(getpid(), foreign); });

    user_regs_struct before{}, after{};
    require(ptrace(PTRACE_GETREGS, child, nullptr, &before) == 0, "leader registers before");
    require(ptrace(PTRACE_CONT, worker, nullptr, nullptr) == 0, "resume worker");
    // PTRACE_CONT returns only after removing the ptrace stop; GETREGS must fail
    // even if this thread has not yet been scheduled on another CPU.
    rejected([&] { verify_stopped_thread_set(child, tids); });
    capture_resumes = 0; track_capture_resumes = true;
    rejected([&] { verify_stopped_thread_set(child, tids, retained); });
    track_capture_resumes = false;
    require(capture_resumes == 0, "retained barrier resumed a running worker");
    require(ptrace(PTRACE_GETREGS, child, nullptr, &after) == 0, "leader remains stopped");
    require(std::memcmp(&before, &after, sizeof before) == 0,
            "failed barrier modified leader registers");
    std::array<unsigned char,4096> control_before{},control_after{};
    require(guest_read_exact(child,reinterpret_cast<void*>(scratch_page),control_before.data(),control_before.size()),
            "read control page before refused first capture");
    capture_resumes=0;track_capture_resumes=true;
    bool stopped_rejection=false;
    try {tracee_state first;(void)first.save_full_state_to_state_store(child);}
    catch(const std::runtime_error& error) {
        const std::string message=error.what();
        stopped_rejection=message=="Cannot read stopped thread signal mask" ||
            message.starts_with("Thread stop barrier cannot inspect stopped thread");
    }
    track_capture_resumes=false;
    require(stopped_rejection,"first capture did not reject the running worker at stop validation");
    require(capture_resumes==0,"first capture injected a syscall before the stop barrier");
    require(guest_read_exact(child,reinterpret_cast<void*>(scratch_page),control_after.data(),control_after.size()) &&
            control_before==control_after,"refused first capture changed control page");
    require(ptrace(PTRACE_GETREGS,child,nullptr,&after)==0 && !memcmp(&before,&after,sizeof(before)),
            "refused first capture changed stopped main registers");
    require(syscall(SYS_tgkill, child, worker, SIGSTOP) == 0, "stop running worker");
    stopped(worker);
    verify_stopped_thread_set(child, tids);
    kill(child, SIGKILL);
    while (waitpid(-1, nullptr, __WALL) > 0 || errno == EINTR) {}
}

int main()
{
    // Enforce a deadline outside the tracer, including failed fixture setup.
    const pid_t test = fork();
    if (!test) {
        setpgid(0, 0);
        const rlimit no_core{0,0};setrlimit(RLIMIT_CORE,&no_core);
        try { run_case(); _exit(0); }
        catch (const std::exception &error) {
            std::fprintf(stderr, "thread control: %s\n", error.what());
            _exit(1);
        }
    }
    if (test < 0) return 1;
    setpgid(test, test);
    int status = 0;
    bool done = false;
    for (int tick = 0; tick < 500; ++tick) {
        if (waitpid(test, &status, WNOHANG) == test) { done = true; break; }
        usleep(10000);
    }
    kill(-test, SIGKILL);
    if (!done) waitpid(test, &status, 0);
    const bool passed = done && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    std::printf("%s thread stop barrier (real clone, running worker, no mutation)\n",
                passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
