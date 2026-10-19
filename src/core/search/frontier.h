#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace detsim::search {
using StateKey = uint64_t;
using WorkerId = uint64_t;
enum class Policy { Dfs, Bfs, Priority };
enum class NodePhase { Pending, Inflight, Expanded, Terminal, DepthCut, ModelBudget, Failure };
enum class ResultKind { Expanded, Terminal, DepthCut, ModelBudget, Failure };
enum class Status { Running, Complete, Bounded, Failed };
struct Ticket {
    StateKey state;
    WorkerId worker;
    uint64_t lease_id;
    uint64_t depth;
    bool may_expand; // false: inspect enabled actions only, execute no transition
};
struct Expansion {
    ResultKind kind = ResultKind::Expanded;
    std::vector<StateKey> successors;
    std::string reason;
};
struct Node {
    uint64_t depth;
    NodePhase phase;
    uint64_t sequence;
    std::optional<Ticket> ticket;
    std::vector<StateKey> successors;
    std::string reason;
};
struct Counters {
    uint64_t admitted = 0, duplicates = 0, improvements = 0;
    uint64_t leased = 0, committed = 0, requeued = 0;
    uint64_t dispatch_candidates = 0; // excludes completed nodes; no wall-time inference
};

// Single coordinator ownership; callers serialize calls and persist/validate all
// returned states before commit. Depth is search bookkeeping, never state identity.
// Parallel DFS is LIFO dispatch, not serial DFS completion order.
class Frontier {
public:
    // Evaluated on pending candidates at dispatch; smaller first, stable ties.
    using Priority = std::function<int64_t(StateKey, uint64_t)>;
    explicit Frontier(uint64_t depth_limit, Policy policy = Policy::Dfs,
                      Priority priority = {},
                      size_t max_states = std::numeric_limits<size_t>::max());
    bool admit(StateKey state, uint64_t depth = 0);
    std::optional<Ticket> lease(WorkerId worker);
    // Higher signed score wins; ties retain original dispatch order. Scores
    // must not mutate/reenter this Frontier. Priority policy ignores this hint.
    using Scorer = std::function<int64_t(StateKey)>;
    std::optional<Ticket> lease(WorkerId worker, const Scorer& scorer, size_t window = 32);
    void commit(const Ticket& ticket, Expansion expansion);
    size_t worker_lost(WorkerId worker);
    Status status() const;
    const std::map<StateKey, Node>& nodes() const { return nodes_; }
    const Counters& counters() const { return counters_; }
private:
    Ticket finalize_lease(WorkerId worker, StateKey key, uint64_t candidates);
    void propagate(StateKey state, uint64_t depth);
    void ensure_healthy() const;
    void index_add(StateKey key, const Node& node);
    void index_remove(const Node& node);
    void update(StateKey key, Node& node, NodePhase phase, uint64_t depth);
    uint64_t limit_, next_sequence_ = 0, next_lease_ = 0;
    Policy policy_;
    Priority priority_;
    size_t max_states_;
    bool poisoned_ = false;
    std::map<StateKey, Node> nodes_;
    // Only live pending work is indexed for dispatch. BFS also tracks inflight
    // layers so a temporarily empty layer does not allow deeper dispatch.
    std::map<uint64_t, StateKey> pending_;
    std::map<std::pair<uint64_t, uint64_t>, StateKey> bfs_pending_;
    std::map<uint64_t, size_t> active_layers_;
    std::array<size_t, 7> phase_counts_{};
    Counters counters_;
};
} // namespace detsim::search
