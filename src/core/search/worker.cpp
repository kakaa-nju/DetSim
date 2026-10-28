#include "worker.h"
#include <filesystem>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sched.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include "compatibility.h"
#include "scheduler.h"
#include "monitor.h"
#include "runtime_profile.h"
#include "engine/thread_pool.h"
#include "state/resident_memory.h"
#include "state/state_store.h"
#include "config.h"
#include <exception>
#include <cerrno>
#include <poll.h>
#include <stdexcept>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <signal.h>
#include <unistd.h>

namespace detsim::search {
namespace {
std::string host_mask(const cpu_set_t& mask) {
    std::string result;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        if (CPU_ISSET(cpu, &mask)) {
            if (!result.empty()) result += ',';
            result += std::to_string(cpu);
        }
    return result;
}
std::vector<pid_t> host_inventory(pid_t pid) {
    std::vector<pid_t> tids;
    const auto directory = std::filesystem::path("/proc") / std::to_string(pid) / "task";
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const auto name = entry.path().filename().string();
        size_t consumed = 0;
        const auto value = std::stol(name, &consumed);
        if (consumed != name.size() || value <= 0 || value > std::numeric_limits<pid_t>::max())
            throw std::runtime_error("HOST_PIN invalid task directory entry");
        tids.push_back(static_cast<pid_t>(value));
    }
    std::sort(tids.begin(), tids.end());
    if (tids.empty() || !std::binary_search(tids.begin(), tids.end(), pid))
        throw std::runtime_error("HOST_PIN incomplete process thread inventory");
    return tids;
}
void host_pin_once(uint64_t worker, int cpu) {
    if (!worker || cpu < 0 || cpu >= CPU_SETSIZE)
        throw std::runtime_error("HOST_PIN invalid worker identity or CPU");
    require_bootstrap_cpu_affinity();
    cpu_set_t desired{}; CPU_ZERO(&desired); CPU_SET(cpu, &desired);
    const pid_t owner = getpid();
    std::vector<pid_t> processes{owner};
    for (int node = 0; node < NP; ++node) {
        const pid_t pid = ptmc_state.pids[node];
        if (pid <= 0 || std::find(processes.begin(), processes.end(), pid) != processes.end())
            throw std::runtime_error("HOST_PIN invalid or duplicate owned process");
        processes.push_back(pid);
    }
    // All guest carriers (including parked carriers) are ptrace-stopped here.
    // Host helper threads must not be created/destroyed during this one-time
    // boundary: two inventories detect observed membership changes and fail
    // closed. Afterward new threads inherit their pinned creator's CPU mask.
    for (const auto pid : processes) {
        const auto tids = host_inventory(pid);
        for (const auto tid : tids) {
            std::ifstream status("/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid) + "/status");
            if (!status) throw std::runtime_error("HOST_PIN thread disappeared before binding");
            pid_t tgid = -1, tracer = -1;
            std::string line;
            while (std::getline(status, line)) {
                if (line.starts_with("Tgid:")) tgid = std::stoi(line.substr(5));
                if (line.starts_with("TracerPid:")) tracer = std::stoi(line.substr(10));
            }
            if (status.bad() || tgid != pid || (pid != owner && tracer != owner))
                throw std::runtime_error("HOST_PIN thread ownership verification failed");
            if (pid != owner) {
                user_regs_struct registers{};
                if (ptrace(PTRACE_GETREGS, tid, nullptr, &registers) == -1)
                    throw std::runtime_error("HOST_PIN target thread is not owned and ptrace-stopped");
            }
            cpu_set_t before{}, actual{};
            if (sched_getaffinity(tid, sizeof(before), &before) != 0)
                throw std::runtime_error("HOST_PIN cannot read thread affinity");
            if (!CPU_ISSET(cpu, &before))
                throw std::runtime_error("HOST_PIN selected CPU is outside thread's existing mask");
            if (sched_setaffinity(tid, sizeof(desired), &desired) != 0 ||
                sched_getaffinity(tid, sizeof(actual), &actual) != 0 ||
                !CPU_EQUAL(&desired, &actual))
                throw std::runtime_error("HOST_PIN cannot set and verify thread affinity");
            std::fprintf(stderr, "HOST_PIN worker=%llu cpu=%d pid=%d tid=%d original=%s actual=%s\n",
                         static_cast<unsigned long long>(worker), cpu, pid, tid,
                         host_mask(before).c_str(), host_mask(actual).c_str());
        }
        if (host_inventory(pid) != tids)
            throw std::runtime_error("HOST_PIN thread set changed during binding");
    }
    std::fflush(stderr);
}
} // namespace

wire::DoneKind completion_kind(EnumerationKind kind) {
    switch (kind) {
        case EnumerationKind::Expanded: return wire::DoneKind::Expanded;
        case EnumerationKind::Terminal: return wire::DoneKind::Terminal;
        case EnumerationKind::DepthCut: return wire::DoneKind::DepthCut;
        case EnumerationKind::ModelBudget: return wire::DoneKind::ModelBudget;
        case EnumerationKind::Violation: return wire::DoneKind::Violation;
    }
    throw std::runtime_error("Invalid single-state expansion outcome");
}
void prepare_worker_process(int fd, int blob_fd) {
    ucred peer{}; socklen_t size = sizeof(peer);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) || size != sizeof(peer) || peer.pid <= 0)
        throw std::runtime_error("Worker cannot identify its coordinator parent");
    if (blob_fd >= 0) {
        ucred data_peer{}; socklen_t data_size = sizeof(data_peer);
        if (getsockopt(blob_fd, SOL_SOCKET, SO_PEERCRED, &data_peer, &data_size) || data_size != sizeof(data_peer) ||
            data_peer.pid != peer.pid || data_peer.uid != peer.uid || data_peer.gid != peer.gid)
            throw std::runtime_error("Worker object channel differs from coordinator parent");
    }
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != peer.pid)
        throw std::runtime_error("Worker coordinator parent differs or has exited");
}
int worker_loop(int fd, wire::Limits limits, int blob_fd) {
    wire::Channel channel(fd, limits, blob_fd);
    uint64_t lease = 0, source_key = 0;
    try {
        if (ptmc_state.model_identity.empty())
            throw std::runtime_error("Worker has no initialized execution model");
        auto& store = StateStore::instance();
        if (!worker_shared_pool_path.empty()) store.attach_shared_pool(worker_shared_pool_path, true);
        const auto pool = worker_shared_pool_path.empty() ? nullptr : store.shared_pool();
        // Publisher proofs belong to this immutable pool instance, never a
        // pathname. Declaration order keeps the pool alive through destruction.
        const auto publisher = pool ? std::make_unique<snapshot::SharedPublisher>(*pool) : nullptr;
        detsim::resident::Scope resident_scope;
        channel.send(wire::Hello{ptmc_state.model_identity, compatibility_manifest()});
        uint64_t placed_worker = 0;
        for (;;) {
            // Idle workers hold no lease. Start the frame deadline only once a
            // coordinator frame begins, rather than expiring a quiet pool slot.
            pollfd control{channel.fd(), POLLIN, 0};
            int ready;
            if (channel.has_buffered_input()) ready = 1;
            else do { ready = poll(&control, 1, -1); } while (ready < 0 && errno == EINTR);
            if (ready < 0) throw std::runtime_error("Worker control poll failed");
            auto message = channel.receive();
            if (!message) throw std::runtime_error("Worker control stream closed without Shutdown");
            if (std::holds_alternative<wire::Shutdown>(*message)) return 0;
            if (!std::holds_alternative<wire::Task>(*message))
                throw std::runtime_error("Worker expected Task or Shutdown");
            auto task = std::get<wire::Task>(std::move(*message));
            lease = task.ticket.lease_id; source_key = task.ticket.state;
            if (!lease || !source_key || task.bundle.system_key != source_key)
                throw std::runtime_error("Worker task identity/lease mismatch");
            // Configuration and actual target layout are both required. No host
            // personality changes or inferred address normalization are allowed.
            if (!runtime_profile().fixed_layout)
                throw std::runtime_error("Worker requires configured canonical tracee layout");
            for (int node = 0; node < NP; ++node)
                if (!tracee_has_fixed_layout(ptmc_state.pids[node]))
                    throw std::runtime_error("Worker tracee does not have canonical fixed layout");
            if (worker_host_cpu >= 0) {
                if (placed_worker && placed_worker != task.ticket.worker)
                    throw std::runtime_error("HOST_PIN Task changed worker identity");
                if (!placed_worker) {
                    // Partial failure is fail-stop: the coordinator tears down
                    // the worker tree. Never continue with partial placement.
                    host_pin_once(task.ticket.worker, worker_host_cpu);
                    placed_worker = task.ticket.worker;
                }
            }
            auto source = publisher ? publisher->import_bundle(task.bundle, ptmc_state.model_identity, true).state
                               : snapshot::import_bundle(task.bundle, ptmc_state.model_identity);
            channel.confirm_objects();
            // Drop the raw incoming closure before capture/exporting successors.
            task.bundle = {};
            for (int node = 0; node < NP; ++node)
                prepare_thread_pool(node, ptmc_state.running_state.child[node]);
            for (int node = 0; node < NP; ++node)
                adopt_snapshot_threads(node, source.ts_hash[node], source.child[node]);
            // Expanded tasks restore before every actual action in state_transition.
            // Avoid restoring twice before their first action; no-action tasks still
            // validate and install their source before reporting completion.
            if (classify_enumeration(source, enabled_actor_actions(source), task.ticket.may_expand) !=
                EnumerationKind::Expanded)
                source.recover_running_state();
            const auto summary = enumerate_state(source, task.ticket.may_expand,
                [&](const ActorAction& actor, int choice, int count, const TransitionResult& result) {
                    wire::Edge edge;
                    edge.lease_id = lease; edge.source = source_key; edge.target = result.new_state.ss_hash;
                    edge.actor = actor; edge.choice = choice; edge.choice_count = static_cast<uint32_t>(count);
                    edge.property_violation = result.property_violation || result.new_state.property_failed;
                    edge.result_code = result.code;
                    edge.bundle = publisher ? publisher->export_bundle(edge.target)
                                       : snapshot::export_bundle(edge.target);
                    channel.send(std::move(edge));
                });
            channel.send(wire::Done{lease, source_key, completion_kind(summary.kind), summary.emitted});
            lease = source_key = 0;
        }
    } catch (const std::exception& error) {
        channel.abort_objects();
        try { channel.send(wire::Error{lease, source_key, error.what()}); } catch (...) {}
        return 1;
    } catch (...) {
        channel.abort_objects();
        try { channel.send(wire::Error{lease, source_key, "Unknown native worker failure"}); } catch (...) {}
        return 1;
    }
}
} // namespace detsim::search
