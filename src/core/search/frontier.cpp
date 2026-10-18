#include "frontier.h"

#include <iterator>
#include <new>
#include <set>
#include <stdexcept>
#include <utility>

namespace detsim::search {
Frontier::Frontier(uint64_t depth_limit, Policy policy, Priority priority, size_t max_states)
    : limit_(depth_limit), policy_(policy), priority_(std::move(priority)), max_states_(max_states) {
    if (policy != Policy::Dfs && policy != Policy::Bfs && policy != Policy::Priority)
        throw std::invalid_argument("unknown frontier policy");
    if ((policy == Policy::Priority) != bool(priority_))
        throw std::invalid_argument("priority callback required only for priority policy");
    if (!max_states_) throw std::invalid_argument("frontier capacity must be positive");
}
void Frontier::ensure_healthy() const {
    if (poisoned_) throw std::runtime_error("frontier poisoned by allocation failure");
}
void Frontier::index_add(StateKey key, const Node& node) {
    ++phase_counts_[static_cast<size_t>(node.phase)];
    if (node.phase == NodePhase::Pending) {
        pending_.emplace(node.sequence, key);
        if (policy_ == Policy::Bfs) bfs_pending_.emplace(std::make_pair(node.depth, node.sequence), key);
    }
    if (policy_ == Policy::Bfs && (node.phase == NodePhase::Pending || node.phase == NodePhase::Inflight))
        ++active_layers_[node.depth];
}
void Frontier::index_remove(const Node& node) {
    --phase_counts_[static_cast<size_t>(node.phase)];
    if (node.phase == NodePhase::Pending) {
        pending_.erase(node.sequence);
        if (policy_ == Policy::Bfs) bfs_pending_.erase({node.depth, node.sequence});
    }
    if (policy_ == Policy::Bfs && (node.phase == NodePhase::Pending || node.phase == NodePhase::Inflight)) {
        const auto layer = active_layers_.find(node.depth);
        if (--layer->second == 0) active_layers_.erase(layer);
    }
}
void Frontier::update(StateKey key, Node& node, NodePhase phase, uint64_t depth) {
    index_remove(node);
    node.phase = phase;
    node.depth = depth;
    index_add(key, node);
}
void Frontier::propagate(StateKey state, uint64_t depth) {
    std::vector<std::pair<StateKey, uint64_t>> work{{state, depth}};
    while (!work.empty()) {
        auto [key, distance] = work.back();
        work.pop_back();
        auto found = nodes_.find(key);
        if (found == nodes_.end()) {
            if (nodes_.size() == max_states_) throw std::length_error("frontier capacity exceeded");
            if (next_sequence_ == UINT64_MAX) throw std::overflow_error("frontier sequence exhausted");
            auto inserted = nodes_.emplace(key, Node{distance, NodePhase::Pending,
                                                    next_sequence_++, {}, {}, {}});
            index_add(key, inserted.first->second);
            ++counters_.admitted;
            continue;
        }
        Node& node = found->second;
        ++counters_.duplicates;
        if (distance >= node.depth) continue;
        update(key, node, node.phase == NodePhase::DepthCut ? NodePhase::Pending : node.phase, distance);
        ++counters_.improvements;
        if (node.phase == NodePhase::Expanded)
            for (auto it = node.successors.rbegin(); it != node.successors.rend(); ++it)
                work.emplace_back(*it, distance + 1);
    }
}
bool Frontier::admit(StateKey state, uint64_t depth) {
    ensure_healthy();
    if (depth > limit_) throw std::invalid_argument("admission beyond depth limit");
    auto found = nodes_.find(state);
    const bool changed = found == nodes_.end() || depth < found->second.depth;
    if (found == nodes_.end() && nodes_.size() == max_states_)
        throw std::length_error("frontier capacity exceeded");
    try { propagate(state, depth); }
    catch (const std::bad_alloc&) { poisoned_ = true; throw; }
    return changed;
}
std::optional<Ticket> Frontier::lease(WorkerId worker) {
    ensure_healthy();
    if (pending_.empty()) return {};
    StateKey selected_key;
    uint64_t candidates = 1;
    if (policy_ == Policy::Dfs) {
        selected_key = pending_.rbegin()->second;
    } else if (policy_ == Policy::Bfs) {
        const auto first = bfs_pending_.begin();
        if (first->first.first != active_layers_.begin()->first) return {};
        selected_key = first->second;
    } else {
        // No phase/index/counter mutation until every callback has succeeded.
        auto first = pending_.begin();
        selected_key = first->second;
        int64_t best = priority_(selected_key, nodes_.at(selected_key).depth);
        for (auto it = std::next(first); it != pending_.end(); ++it) {
            ++candidates;
            const auto priority = priority_(it->second, nodes_.at(it->second).depth);
            if (priority < best) {
                selected_key = it->second;
                best = priority;
            }
        }
    }
    auto selected = nodes_.find(selected_key);
    if (next_lease_ == UINT64_MAX) throw std::overflow_error("frontier lease IDs exhausted");
    Ticket result{selected->first, worker, ++next_lease_, selected->second.depth, selected->second.depth < limit_};
    try {
        update(selected->first, selected->second, NodePhase::Inflight, selected->second.depth);
        selected->second.ticket = result;
    } catch (const std::bad_alloc&) { poisoned_ = true; throw; }
    counters_.dispatch_candidates += candidates;
    ++counters_.leased;
    return result;
}
void Frontier::commit(const Ticket& ticket, Expansion expansion) {
    ensure_healthy();
    const auto found = nodes_.find(ticket.state);
    if (found == nodes_.end() || found->second.phase != NodePhase::Inflight ||
        !found->second.ticket || found->second.ticket->worker != ticket.worker ||
        found->second.ticket->lease_id != ticket.lease_id || found->second.ticket->depth != ticket.depth ||
        found->second.ticket->may_expand != ticket.may_expand)
        throw std::invalid_argument("stale or invalid frontier lease");
    NodePhase phase;
    switch (expansion.kind) {
    case ResultKind::Expanded: phase = NodePhase::Expanded; break;
    case ResultKind::Terminal: phase = NodePhase::Terminal; break;
    case ResultKind::DepthCut: phase = NodePhase::DepthCut; break;
    case ResultKind::ModelBudget: phase = NodePhase::ModelBudget; break;
    case ResultKind::Failure: phase = NodePhase::Failure; break;
    default: throw std::invalid_argument("unknown expansion result");
    }
    if ((!ticket.may_expand && phase == NodePhase::Expanded) ||
        (ticket.may_expand && phase == NodePhase::DepthCut))
        throw std::invalid_argument("expansion result incompatible with probe lease");
    if (phase != NodePhase::Expanded && !expansion.successors.empty())
        throw std::invalid_argument("incomplete or terminal expansion cannot publish successors");
    if ((phase == NodePhase::ModelBudget || phase == NodePhase::Failure) && expansion.reason.empty())
        throw std::invalid_argument("cutoff/failure requires a reason");
    try {
        std::set<StateKey> missing;
        for (auto key : expansion.successors)
            if (!nodes_.count(key)) missing.insert(key);
        if (missing.size() > max_states_ - nodes_.size())
            throw std::length_error("frontier capacity exceeded");
        if (missing.size() > UINT64_MAX - next_sequence_)
            throw std::overflow_error("frontier sequence exhausted");
        Node& node = found->second;
        node.successors = std::move(expansion.successors);
        node.reason = std::move(expansion.reason);
        update(found->first, node,
               phase == NodePhase::DepthCut && node.depth < limit_ ? NodePhase::Pending : phase, node.depth);
        node.ticket.reset();
        // Improved in-flight depth wins over the original ticket's depth.
        for (auto key : node.successors) propagate(key, node.depth + 1);
        ++counters_.committed;
    } catch (const std::bad_alloc&) { poisoned_ = true; throw; }
}
size_t Frontier::worker_lost(WorkerId worker) {
    ensure_healthy();
    size_t count = 0;
    try {
        for (auto& [key, node] : nodes_) {
            if (node.ticket && node.ticket->worker == worker) {
                update(key, node, NodePhase::Pending, node.depth);
                node.ticket.reset();
                ++count;
            }
        }
    } catch (const std::bad_alloc&) { poisoned_ = true; throw; }
    counters_.requeued += count;
    return count;
}
Status Frontier::status() const {
    if (poisoned_) return Status::Failed;
    const auto count = [this](NodePhase phase) { return phase_counts_[static_cast<size_t>(phase)]; };
    if (count(NodePhase::Failure)) return Status::Failed;
    if (count(NodePhase::Pending) || count(NodePhase::Inflight)) return Status::Running;
    return count(NodePhase::DepthCut) || count(NodePhase::ModelBudget) ? Status::Bounded : Status::Complete;
}
} // namespace detsim::search
