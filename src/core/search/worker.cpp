#include "worker.h"
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
            auto source = publisher ? publisher->import_bundle(task.bundle, ptmc_state.model_identity, true).state
                               : snapshot::import_bundle(task.bundle, ptmc_state.model_identity);
            channel.confirm_objects();
            // Drop the raw incoming closure before capture/exporting successors.
            task.bundle = {};
            for (int node = 0; node < NP; ++node)
                prepare_thread_pool(node, ptmc_state.running_state.child[node]);
            for (int node = 0; node < NP; ++node)
                adopt_snapshot_threads(node, source.ts_hash[node], source.child[node]);
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
