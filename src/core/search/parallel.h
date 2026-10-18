#pragma once
#include "frontier.h"
#include "state/state.h"
#include <optional>
#include <string>

namespace detsim::search {
struct Options {
    // Shared publication guarantees cross-process visibility; explicit store
    // barriers/finalization retain durability. Durable uses complete bundles.
    enum class Storage { Shared, Durable };
    Storage storage = Storage::Shared;
    unsigned workers = 2;
    uint64_t depth = 1;
    Policy policy = Policy::Dfs;
    Frontier::Priority priority;
    unsigned retries = 2; // total transient failures allowed for each source
    int timeout_ms = 30'000; // startup/task deadline, also caps frame I/O
};
enum class ParallelStatus { Complete, Bounded, Violation, Failed };
struct Report {
    ParallelStatus status = ParallelStatus::Failed;
    bool property_violation = false;
    std::optional<StateKey> violation_state;
    uint64_t nodes = 0, edges = 0, attempted_edges = 0, retries = 0;
    uint64_t local_completions = 0;
    Counters frontier;
    double elapsed_seconds = 0;
    std::string artifacts;
    std::string error;
};
// Owns independent worker processes/stores. The calling tracer retains its live
// tracees; imported immutable states never imply completed exploration.
Report run_parallel(const sys_state& root, const Options& options);
} // namespace detsim::search
