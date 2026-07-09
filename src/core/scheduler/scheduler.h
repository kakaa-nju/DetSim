/*
 * scheduler/scheduler.h - State space exploration scheduler
 *
 * This module provides state space exploration strategies:
 * - Breadth-first search (BFS)
 * - Depth-first search (DFS)
 * - Random exploration
 *
 * The scheduler is responsible for systematic exploration of execution
 * paths in the distributed system being simulated.
 */

#ifndef __SCHEDULER_SCHEDULER_H
#define __SCHEDULER_SCHEDULER_H

#include "types.h"
#include <cstddef>
#include <functional>

/* ======================================================================
 * Exploration Statistics
 * ====================================================================== */

struct ExplorationStats {
    size_t states_searched = 0;
    size_t states_unique = 0;
    size_t states_queued = 0;
    size_t depth = 0;
    double elapsed_time = 0.0;
    double states_per_sec = 0.0;

    void reset() {
        states_searched = 0;
        states_unique = 0;
        states_queued = 0;
        depth = 0;
        elapsed_time = 0.0;
        states_per_sec = 0.0;
    }
};

/* ======================================================================
 * Exploration Callbacks
 * ====================================================================== */

// Called when a state is explored
using StateExploredCallback = std::function<void(hash_type state_hash, const ExplorationStats& stats)>;

// Called when exploration is interrupted
using ExplorationInterruptedCallback = std::function<void(const ExplorationStats& stats, const char* reason)>;

// Called when exploration completes
using ExplorationCompleteCallback = std::function<void(const ExplorationStats& stats)>;

/* ======================================================================
 * Exploration Functions
 * ====================================================================== */

/*
 * exec_bfs - Breadth-first search exploration
 *
 * Explores the state space level by level, guaranteeing shortest paths
 * to any reachable state. Good for finding minimal counter-examples.
 *
 * Returns: 0 on success, non-zero on error or interruption
 */
int scheduler_exec_bfs();

/*
 * scheduler_exec_dfs - Depth-first search exploration
 *
 * Explores deeply along one path before backtracking. Good for finding
 * deep bugs but may not find shortest paths.
 *
 * depth: Maximum depth to explore (-1 for unlimited)
 *
 * Returns: 0 on success, non-zero on error or interruption
 */
int scheduler_exec_dfs(int depth);

/*
 * scheduler_exec_rand - Random exploration
 *
 * Randomly chooses paths through the state space. Good for fuzzing
 * and finding unexpected edge cases.
 *
 * depth: Number of random steps to take
 *
 * Returns: 0 on success, non-zero on error or interruption
 */
int scheduler_exec_rand(int depth);

/* ======================================================================
 * Statistics and Control
 * ====================================================================== */

/* Get current exploration statistics */
const ExplorationStats& get_exploration_stats();

/* Reset statistics */
void reset_exploration_stats();

/* Check if exploration should stop (called from exploration loops) */
bool should_stop_exploration();

/* Request exploration to stop */
void request_exploration_stop();

/* ======================================================================
 * Callback Registration
 * ====================================================================== */

void set_state_explored_callback(StateExploredCallback cb);
void set_exploration_interrupted_callback(ExplorationInterruptedCallback cb);
void set_exploration_complete_callback(ExplorationCompleteCallback cb);

#endif /* __SCHEDULER_SCHEDULER_H */
