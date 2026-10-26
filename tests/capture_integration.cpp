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
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <linux/prctl.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
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
                require(seeded_stats.lookups == 0 && seeded_stats.builds == NP,
                        "verify seed did not rebuild from live maps");
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
