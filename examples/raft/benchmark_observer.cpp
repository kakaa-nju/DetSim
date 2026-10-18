// Audit-only observer. The timed benchmark keeps the ordinary two plugins.
// Reuse the case's checked projection rather than decoding private guest memory
// differently in the benchmark. Projection equality is not raw-state equality.
#define detsim_plugin_entry_v2 benchmark_unused_checker_entry
#include "plugins/check.cpp"
#undef detsim_plugin_entry_v2
#include <nlohmann/json.hpp>
namespace {
using Json = nlohmann::json;
Json projected(const detsim_host_v2* host, View view) {
    Json output = Json::array();
    const auto nodes = project(host, view);
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        detsim_time time{};
        if (host->time(view, i, &time)) throw Unavailable("benchmark clock unavailable");
        Json logs = Json::array(), matches = Json::array();
        for (const auto& [index, entry] : node.logs)
            logs.push_back({index, entry.term, entry.type, entry.command});
        for (const auto& [peer, index] : node.match) matches.push_back({peer,index});
        output.push_back({{"initialized",node.initialized},{"term",node.term},{"role",node.role},
            {"vote",node.vote},{"commit",node.commit},{"applied",node.applied},
            {"snapshot",{node.snap_idx,node.snap_term}},{"base",node.base},{"count",node.count},
            {"logs",logs},{"match",matches},{"time",{time.seconds,time.microseconds}}});
    }
    return output;
}
detsim_verdict observe_transition(const detsim_host_v2* host, View before, View after,
                                  const detsim_event_v2* event, detsim_diagnostic diagnostic) {
    return guarded(diagnostic,[&] {
        if (!event) throw Unavailable("benchmark transition missing event");
        const Json record{{"before",projected(host,before)},{"after",projected(host,after)},
            {"process",event->process},{"thread",event->thread},{"choice",event->choice},
            {"syscall",event->syscall_number},{"phase",event->phase},
            {"result_valid",event->result_valid},{"result",event->result}};
        static FILE* output = std::fopen("benchmark-transitions.jsonl", "w");
        if (!output) throw Unavailable("cannot open benchmark transition log");
        const auto line = record.dump() + '\n';
        if (std::fwrite(line.data(),1,line.size(),output) != line.size() || std::fflush(output))
            throw Unavailable("cannot persist benchmark transition log");
    });
}
const detsim_plugin_v2 benchmark_descriptor{DETSIM_PLUGIN_ABI_V2,sizeof(detsim_plugin_v2),
    "willemt-raft.benchmark-projection",0,nullptr,observe_transition,nullptr,nullptr,nullptr,nullptr};
}
extern "C" const detsim_plugin_v2* detsim_plugin_entry_v2() { return &benchmark_descriptor; }
