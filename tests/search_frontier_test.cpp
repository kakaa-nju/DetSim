#include "core/search/frontier.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>
using namespace detsim::search;
template<class F> void rejects(F fn) {
    bool threw = false;
    try { fn(); } catch (const std::exception&) { threw = true; }
    assert(threw);
}
Expansion edges(std::initializer_list<StateKey> ids) { return {ResultKind::Expanded, ids, {}}; }
void ordering() {
    for (auto policy : {Policy::Dfs, Policy::Bfs, Policy::Priority}) {
        Frontier f(5, policy, policy == Policy::Priority ? Frontier::Priority([](StateKey k, uint64_t) { return -int64_t(k); }) : Frontier::Priority{});
        f.admit(1); f.admit(2); f.admit(3);
        assert(f.lease(1)->state == (policy == Policy::Bfs ? 1 : 3));
    }
    bool throw_priority = true;
    Frontier throwing(2, Policy::Priority, [&](StateKey k, uint64_t) {
        if (throw_priority && k == 2) throw std::runtime_error("heuristic failed");
        return int64_t(k);
    });
    throwing.admit(1); throwing.admit(2);
    rejects([&] { throwing.lease(1); });
    assert(throwing.counters().leased == 0);
    assert(throwing.nodes().at(1).phase == NodePhase::Pending);
    throw_priority = false; assert(throwing.lease(1)->state == 1);
    Frontier stable(2, Policy::Priority, [](StateKey, uint64_t) { return 0; });
    stable.admit(8); stable.admit(2);
    assert(stable.lease(0)->state == 8);
    Frontier bfs(10, Policy::Bfs);
    bfs.admit(1); bfs.admit(2);
    auto a = *bfs.lease(1), b = *bfs.lease(2);
    bfs.commit(b, edges({3}));
    assert(!bfs.lease(3)); // pending depth 1 cannot pass inflight depth 0
    assert(bfs.status() == Status::Running);
    bfs.commit(a, {ResultKind::Terminal, {}, {}});
    assert(bfs.lease(3)->state == 3);
}
void recovery() {
    Frontier f(5); f.admit(1); f.admit(2);
    auto a = *f.lease(7), b = *f.lease(7);
    assert(!f.lease(8)); assert(f.status() == Status::Running);
    auto fake = a; fake.worker = 8;
    rejects([&] { f.commit(fake, edges({3})); });
    rejects([&] { f.commit(a, {ResultKind::Terminal, {3}, {}}); });
    rejects([&] { f.commit(a, {ResultKind::Failure, {}, {}}); });
    rejects([&] { f.commit(a, {static_cast<ResultKind>(99), {}, {}}); });
    rejects([&] { f.commit(a, {ResultKind::DepthCut, {}, {}}); });
    fake = a; ++fake.depth;
    rejects([&] { f.commit(fake, edges({3})); });
    fake = a; fake.may_expand = false;
    rejects([&] { f.commit(fake, edges({3})); });
    assert(f.nodes().size() == 2 && f.counters().committed == 0);
    assert(f.worker_lost(7) == 2 && f.worker_lost(7) == 0);
    auto replacement = *f.lease(8);
    rejects([&] { f.commit(a, edges({3})); });
    rejects([&] { f.commit(b, edges({3})); });
    f.commit(replacement, {ResultKind::Terminal, {}, {}});
    rejects([&] { f.commit(replacement, edges({3})); });
    f.commit(*f.lease(9), {ResultKind::Terminal, {}, {}});
    assert(f.status() == Status::Complete);
    assert(f.counters().requeued == 2);
}
void depth() {
    Frontier f(3); f.admit(10, 2);
    f.commit(*f.lease(1), edges({11}));
    auto probe = *f.lease(1); assert(!probe.may_expand);
    rejects([&] { f.commit(probe, edges({12})); });
    f.commit(probe, {ResultKind::DepthCut, {}, {}});
    assert(f.nodes().at(11).phase == NodePhase::DepthCut);
    assert(f.status() == Status::Bounded);
    f.admit(10, 0); // cached edge must reopen descendant
    assert(f.nodes().at(11).depth == 1);
    auto t = *f.lease(2); assert(t.state == 11);
    f.commit(t, edges({12}));
    f.commit(*f.lease(2), edges({10})); // cycle closes without a depth cut
    assert(f.status() == Status::Complete);
    assert(f.counters().committed == 4);
    Frontier inflight(3); inflight.admit(1, 2);
    auto old = *inflight.lease(1);
    inflight.admit(1, 0);
    assert(!inflight.lease(2)); // no second live lease
    inflight.commit(old, edges({2}));
    assert(inflight.nodes().at(2).depth == 1);
    // Improvement crosses multiple already expanded nodes and their cutoff.
    Frontier chain(4); chain.admit(1, 2);
    chain.commit(*chain.lease(0), edges({2}));
    chain.commit(*chain.lease(0), edges({3}));
    chain.admit(1, 0);
    assert(chain.nodes().at(2).depth == 1 && chain.nodes().at(3).depth == 2);
    assert(chain.lease(0)->state == 3);
}
void capacity_and_outcomes() {
    Frontier f(2); f.admit(0);
    Expansion wide; for (StateKey i = 1; i <= 1025; ++i) wide.successors.push_back(i);
    f.commit(*f.lease(0), wide);
    assert(f.nodes().size() == 1026);
    while (auto t = f.lease(0)) f.commit(*t, {ResultKind::Terminal, {}, {}});
    assert(f.status() == Status::Complete);
    Frontier limited(2, Policy::Dfs, {}, 2); limited.admit(0);
    auto t = *limited.lease(1);
    rejects([&] { limited.commit(t, edges({1,2})); });
    assert(limited.nodes().size() == 1 && limited.nodes().at(0).ticket);
    limited.commit(t, edges({1,1}));
    assert(limited.nodes().size() == 2);
    rejects([&] { limited.admit(2); });
    rejects([&] { limited.admit(1, 3); });
    for (auto kind : {ResultKind::ModelBudget, ResultKind::Failure}) {
        Frontier cut(3); cut.admit(1);
        cut.commit(*cut.lease(1), {kind, {}, "explicit reason"});
        assert(cut.status() == (kind == ResultKind::Failure ? Status::Failed : Status::Bounded));
        assert(cut.nodes().at(1).reason == "explicit reason");
    }
    Frontier zero(0); zero.admit(0);
    auto probe = *zero.lease(1); assert(!probe.may_expand);
    zero.commit(probe, {ResultKind::DepthCut, {}, {}});
    assert(zero.status() == Status::Bounded);
    Frontier exact(0); exact.admit(0);
    exact.commit(*exact.lease(1), {ResultKind::Terminal, {}, {}});
    assert(exact.status() == Status::Complete);
    Frontier improved(2); improved.admit(0, 2);
    auto old_probe = *improved.lease(1); improved.admit(0, 0);
    improved.commit(old_probe, {ResultKind::DepthCut, {}, {}});
    auto real = *improved.lease(2); assert(real.may_expand);
    improved.commit(real, {ResultKind::Terminal, {}, {}});
    assert(improved.status() == Status::Complete);
    rejects([] { Frontier invalid(1, Policy::Priority); });
}
// Serial BFS oracle for a diamond, unequal path convergence, cycle, and terminal.
using Graph = std::map<StateKey, std::vector<StateKey>>;
const Graph graph{{0,{1,2}}, {1,{3}}, {2,{4,6}}, {3,{4}}, {4,{5}}, {5,{1,7}}, {6,{7}}, {7,{}}};
std::map<StateKey,uint64_t> oracle(uint64_t bound) {
    std::map<StateKey,uint64_t> seen{{0,0}};
    std::vector<StateKey> queue{0};
    for(size_t i=0;i<queue.size();++i) {
        auto key=queue[i]; if(seen.at(key)==bound) continue;
        for(auto child:graph.at(key)) if(!seen.count(child)) { seen[child]=seen.at(key)+1; queue.push_back(child); }
    }
    return seen;
}
void equivalence() {
    for(auto policy:{Policy::Dfs,Policy::Bfs,Policy::Priority})
    for(unsigned workers:{1,2,4}) for(uint64_t bound:{2,3,4,10}) for(bool reverse:{false,true}) {
        Frontier f(bound, policy, policy==Policy::Priority ? Frontier::Priority([](StateKey k,uint64_t) {return int64_t(k%3);}) : Frontier::Priority{});
        f.admit(0);
        std::set<StateKey> completed;
        while(f.status()==Status::Running) {
            std::vector<Ticket> batch;
            for(unsigned w=0;w<workers;++w) if(auto t=f.lease(w)) batch.push_back(*t);
            assert(!batch.empty());
            if(reverse) std::reverse(batch.begin(),batch.end());
            for(auto t:batch) {
                if(t.may_expand) assert(completed.insert(t.state).second);
                const auto& successors=graph.at(t.state);
                if(successors.empty()) f.commit(t,{ResultKind::Terminal,{},{}});
                else if(!t.may_expand) f.commit(t,{ResultKind::DepthCut,{},{}});
                else f.commit(t,{ResultKind::Expanded,successors,{}});
            }
        }
        const auto expected=oracle(bound);
        assert(f.nodes().size()==expected.size());
        bool cut=false;
        for(auto [key,depth]:expected) {
            const auto& node=f.nodes().at(key); assert(node.depth==depth);
            if(depth==bound && !graph.at(key).empty()) { assert(node.phase==NodePhase::DepthCut); cut=true; }
            else {
                assert(node.phase==(graph.at(key).empty()?NodePhase::Terminal:NodePhase::Expanded));
                assert(node.successors == graph.at(key));
            }
        }
        assert(f.status()==(cut?Status::Bounded:Status::Complete));
    }
}
void indexed_dispatch_scaling() {
    constexpr uint64_t length = 4096;
    for (auto policy : {Policy::Dfs, Policy::Bfs, Policy::Priority}) {
        uint64_t callbacks = 0;
        Frontier f(length, policy, policy == Policy::Priority ? Frontier::Priority(
            [&](StateKey, uint64_t) { ++callbacks; return 0; }) : Frontier::Priority{});
        f.admit(0);
        for (StateKey key = 0; key <= length; ++key) {
            assert(f.status() == Status::Running);
            auto t = *f.lease(0); assert(t.state == key);
            assert(!f.lease(1)); // only inflight work remains
            if (key == length) f.commit(t, {ResultKind::Terminal, {}, {}});
            else f.commit(t, {ResultKind::Expanded, {key + 1}, {}});
            assert(f.counters().dispatch_candidates == key + 1);
        }
        assert(f.status() == Status::Complete);
        assert(f.nodes().size() == length + 1);
        assert(callbacks == (policy == Policy::Priority ? length + 1 : 0));
    }
    // An improved inflight node must move the BFS layer barrier immediately.
    Frontier bfs(5, Policy::Bfs); bfs.admit(1, 2);
    auto t = *bfs.lease(1);
    bfs.admit(2, 1); bfs.admit(1, 0);
    assert(!bfs.lease(2));
    bfs.commit(t, {ResultKind::Terminal, {}, {}});
    assert(bfs.lease(2)->state == 2);
}
int main() {
    ordering(); recovery(); depth(); capacity_and_outcomes(); equivalence(); indexed_dispatch_scaling();
    std::cout << "frontier: ordering, leases, cuts, >512 fanout, 72 graph runs, and indexed dispatch scaling passed\n";
}
