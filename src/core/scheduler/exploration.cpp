/*
 * exploration.cpp - State space exploration implementations
 */

#include "scheduler.h"
#include "state/state.h"
#include "state/state_store.h"
#include "state/sysstate_store.h"
#include "state/state_transition.h"
#include "monitor.h"
#include "proc_status.h"
#include "utils/utils.h"
#include "ui/log_wrapper.h"
#include "engine/signal_handler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <optional>
#include <random>
#include <set>

/* ======================================================================
 * Global State for Exploration
 * ====================================================================== */

/* Exploration statistics - atomic for thread safety */
static std::atomic<size_t> g_states_searched{0};
static std::atomic<size_t> g_states_new{0};
static std::atomic<size_t> g_queue_size{0};
static std::atomic<bool> g_stop_requested{false};

/* Use sigint from signal handler namespace */
using sig::sigint_received;

/* Statistics accumulated during exploration */
static ExplorationStats g_stats;

/* Callbacks */
static StateExploredCallback g_state_explored_cb;
static ExplorationInterruptedCallback g_interrupted_cb;
static ExplorationCompleteCallback g_complete_cb;

/* Timing */
static double g_start_time = 0.0;

/* State set for deduplication */
static std::set<hash_type> g_state_set;

/* ======================================================================
 * Utility Functions
 * ====================================================================== */

static double gettime()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void update_stats()
{
    g_stats.states_searched = g_states_searched.load();
    g_stats.states_unique = g_state_set.size();
    g_stats.states_queued = g_queue_size.load();
    g_stats.elapsed_time = gettime() - g_start_time;
    if (g_stats.elapsed_time > 0) {
        g_stats.states_per_sec = g_stats.states_searched / g_stats.elapsed_time;
    }
}

/* ======================================================================
 * Exploration Control
 * ====================================================================== */

const ExplorationStats& get_exploration_stats()
{
    update_stats();
    return g_stats;
}

void reset_exploration_stats()
{
    g_stats.reset();
    g_states_searched = 0;
    g_states_new = 0;
    g_queue_size = 0;
}

bool should_stop_exploration()
{
    return g_stop_requested.load() || sigint_received;
}

void request_exploration_stop()
{
    g_stop_requested = true;
}

void set_state_explored_callback(StateExploredCallback cb)
{
    g_state_explored_cb = cb;
}

void set_exploration_interrupted_callback(ExplorationInterruptedCallback cb)
{
    g_interrupted_cb = cb;
}

void set_exploration_complete_callback(ExplorationCompleteCallback cb)
{
    g_complete_cb = cb;
}

/* ======================================================================
 * State Checking
 * ====================================================================== */

extern long expr(const char *e, bool *success);

static int check_state_assertions()
{
    hash_type current_hash = ptmc_state.running_state.ss_hash;
    for (auto &assertion : ptmc_state.assertions)
    {
        bool success;
        long val = expr(assertion.c_str(), &success);
        LOG_DEBUG("Assertion \"%s\" = %ld (success=%d)", assertion.c_str(), val, success);
        if (!success)
            LOG_ERROR("Failed to evaluate expression \"%s\"", assertion.c_str());
        if (val == false)
        {
            return 1;
        }
    }
    for (auto f : ptmc_state.user_checks)
        if (f())
            return 1;

    return 0;
}

/* ======================================================================
 * BFS Implementation
 * ====================================================================== */

int scheduler_exec_bfs()
{
    ptmc_state.mode = PTMC_STATE::MODE_BFS;
    g_start_time = gettime();
    srand(time(NULL));

    std::optional<sys_state> state_fetched = std::nullopt;
    syscall_info syscall_info[NP];

    size_t states_searched_this_run = 0;
    size_t states_new_this_run = 0;

    g_stop_requested = false;
    g_state_set.clear();

    StateStore::instance().reset_stats();
    StateStore::instance().enable_prefetch(100);

    g_states_searched = 0;
    g_states_new = 0;
    g_queue_size = 0;

    sys_state s = ptmc_state.running_state;
    StateStore::instance().queue_clear();
    state_queue_append(ptmc_state.running_state);

    if (!g_state_set.count(s.ss_hash))
    {
        states_new_this_run++;
    }
    g_state_set.emplace(s.ss_hash);
    states_searched_this_run++;

    g_states_searched = states_searched_this_run;
    g_states_new = states_new_this_run;

    ptmc_state.n_choose = 0;
    ptmc_state.choose = 0;

    int loop_count = 0;
    while (!should_stop_exploration())
    {
        loop_count++;
        if (loop_count % 100 == 0)
        {
            LOG_TRACE("BFS loop %d, queue_size=%zu, searched=%zu", loop_count,
                      StateStore::instance().queue_size(), states_searched_this_run);
        }

        state_fetched = state_queue_extract();
        if (!state_fetched.has_value())
        {
            LOG_TRACE("BFS: queue empty, exiting loop. Total searched: %zu",
                      states_searched_this_run);
            break;
        }

        LOG_INFO("BFS loop %d: fetched state %016lx, queue_size=%zu", loop_count,
                  state_fetched.value().ss_hash, StateStore::instance().queue_size());

        std::vector<int> indexes;
        for (int k = 0; k < NP; k++)
            indexes.push_back(k);
        shuffle(indexes.begin(), indexes.end(), std::default_random_engine(rand()));

        for (auto &i : indexes)
        {
        again:
            s = state_fetched.value();

            if (DISDEAD(s.status[i])) {
                LOG_INFO("Process %d is dead, skipping", i);
                continue;
            }

            LOG_INFO("Processing process %d, state hash=%016lx", i, s.ss_hash);

            for (int j = 0; j < NP; j++)
                ptmc_state.status[j] = s.status[j];

            for (int j = 0; j < NP; j++)
                if (s.ts_hash[j] == 0)
                    LOG_CRIT("Child state hash is 0 for tracee %d. This should not "
                             "happen if the state is properly saved.", j);

            TransitionResult result = state_transition(s, i);
            LOG_INFO("state_transition result: code=%d, new_state_hash=%016lx", result.code, result.new_state.ss_hash);
            states_searched_this_run++;
            g_states_searched = states_searched_this_run;

            int check_result = check_state_assertions();
            if (check_result != 0)
            {
                update_stats();
                if (g_interrupted_cb)
                    g_interrupted_cb(g_stats, "illegal state detected");
                state_fetched = std::nullopt;
                g_state_set.clear();
                return 1;
            }

            if (result.code != CKPT_DISCARD)
            {
                bool is_new = !g_state_set.count(result.new_state.ss_hash);
                if (is_new)
                {
                    if (check_result == 0)
                        state_queue_append(result.new_state);
                    g_state_set.emplace(result.new_state.ss_hash);
                    states_new_this_run++;
                    g_states_new = states_new_this_run;
                }
            }

            if (g_state_explored_cb)
            {
                update_stats();
                g_state_explored_cb(result.new_state.ss_hash, g_stats);
            }

            if (ptmc_state.n_choose)
            {
                if (ptmc_state.mode == PTMC_STATE::MODE_BFS)
                {
                    ptmc_state.choose++;
                    if (ptmc_state.choose < ptmc_state.n_choose)
                        goto again;
                    ptmc_state.choose = 0;
                }
            }
        }

        g_states_searched = states_searched_this_run;
        g_states_new = states_new_this_run;
        g_queue_size = StateStore::instance().queue_size();
    }

    update_stats();

    if (sigint_received)
    {
        sigint_received = 0;
        if (g_interrupted_cb)
            g_interrupted_cb(g_stats, "SIGINT received");
        g_state_set.clear();
        return 0;
    }

    if (g_complete_cb)
        g_complete_cb(g_stats);

    g_state_set.clear();
    return 0;
}

/* ======================================================================
 * DFS Implementation
 * ====================================================================== */

static int dfs_recursive(int depth, int max_depth, std::set<hash_type> &visited)
{
    if (should_stop_exploration())
        return 0;

    if (depth >= max_depth)
        return 0;

    sys_state s = ptmc_state.running_state;

    for (int i = 0; i < NP; i++)
    {
        if (DISDEAD(s.status[i]))
            continue;

    again:
        for (int j = 0; j < NP; j++)
            ptmc_state.status[j] = s.status[j];

        TransitionResult result = state_transition(s, i);
        g_states_searched++;

        int check_result = check_state_assertions();
        if (check_result != 0)
        {
            update_stats();
            if (g_interrupted_cb)
                g_interrupted_cb(g_stats, "illegal state detected");
            return 1;
        }

        if (result.code != CKPT_DISCARD)
        {
            bool is_new = !visited.count(result.new_state.ss_hash);
            if (is_new)
            {
                visited.emplace(result.new_state.ss_hash);
                g_states_new++;

                if (g_state_explored_cb)
                {
                    update_stats();
                    g_state_explored_cb(result.new_state.ss_hash, g_stats);
                }

                int ret = dfs_recursive(depth + 1, max_depth, visited);
                if (ret != 0)
                    return ret;

                s.recover_running_state();
                ptmc_state.running_state = s;
            }
        }

        if (ptmc_state.n_choose)
        {
            if (ptmc_state.mode == PTMC_STATE::MODE_DFS)
            {
                ptmc_state.choose++;
                if (ptmc_state.choose < ptmc_state.n_choose)
                    goto again;
                ptmc_state.choose = 0;
            }
        }
    }

    return 0;
}

int scheduler_exec_dfs(int depth)
{
    ptmc_state.mode = PTMC_STATE::MODE_DFS;
    g_start_time = gettime();

    g_stop_requested = false;
    g_states_searched = 0;
    g_states_new = 0;

    std::set<hash_type> visited;
    visited.emplace(ptmc_state.running_state.ss_hash);

    int ret = dfs_recursive(0, depth, visited);

    update_stats();

    if (sigint_received)
    {
        sigint_received = 0;
        if (g_interrupted_cb)
            g_interrupted_cb(g_stats, "SIGINT received");
        return 0;
    }

    if (ret == 0 && g_complete_cb)
        g_complete_cb(g_stats);

    return ret;
}

/* ======================================================================
 * Random Exploration Implementation
 * ====================================================================== */

int scheduler_exec_rand(int depth)
{
    ptmc_state.mode = PTMC_STATE::MODE_RAND;
    g_start_time = gettime();
    srand(time(NULL));

    g_stop_requested = false;
    g_states_searched = 0;
    g_states_new = 0;

    std::set<hash_type> visited;
    visited.emplace(ptmc_state.running_state.ss_hash);

    for (int step = 0; step < depth && !should_stop_exploration(); step++)
    {
        sys_state s = ptmc_state.running_state;

        std::vector<int> live_procs;
        for (int i = 0; i < NP; i++)
        {
            if (!DISDEAD(s.status[i]))
                live_procs.push_back(i);
        }

        if (live_procs.empty())
            break;

        int proc = live_procs[rand() % live_procs.size()];

        for (int j = 0; j < NP; j++)
            ptmc_state.status[j] = s.status[j];

        TransitionResult result = state_transition(s, proc);
        g_states_searched++;

        int check_result = check_state_assertions();
        if (check_result != 0)
        {
            update_stats();
            if (g_interrupted_cb)
                g_interrupted_cb(g_stats, "illegal state detected");
            return 1;
        }

        if (result.code != CKPT_DISCARD)
        {
            bool is_new = !visited.count(result.new_state.ss_hash);
            if (is_new)
            {
                visited.emplace(result.new_state.ss_hash);
                g_states_new++;
            }

            if (g_state_explored_cb)
            {
                update_stats();
                g_state_explored_cb(result.new_state.ss_hash, g_stats);
            }
        }
    }

    update_stats();

    if (sigint_received)
    {
        sigint_received = 0;
        if (g_interrupted_cb)
            g_interrupted_cb(g_stats, "SIGINT received");
        return 0;
    }

    if (g_complete_cb)
        g_complete_cb(g_stats);

    return 0;
}
