/*
 * Raft Safety Property Checker
 * 
 * Checks:
 * 1. Term monotonicity: term should never decrease
 * 2. Leader uniqueness: only one leader per term
 * 3. Snapshot term consistency: snapshot term <= current term
 * 
 * NOTE: This checker reads Raft state from ptmc_state.raft_states[]
 * which is serialized/deserialized with each sys_state. This ensures
 * correct behavior during BFS exploration where states branch.
 */

#include "common.h"
#include "monitor.h"
#include "utils/expr_eval.hpp"
#include <cstring>
#include <fmt/format.h>

extern "C" {

/* Helper to check if raft node is initialized */
bool is_node_initialized(int node_id) {
    std::string ptr_expr = fmt::format("(void*)raft");
    bool success = false;
    uintptr_t ptr_val = eval<uintptr_t>(ptr_expr.c_str(), node_id, &success);
    return success && ptr_val != 0;
}

/* Helper to read current term from tracee memory */
long get_node_term(int node_id, bool *success) {
    if (!is_node_initialized(node_id)) {
        *success = false;
        return 0;
    }
    std::string term_expr = fmt::format("((raft_server_private_t*)raft)->current_term");
    return eval<long>(term_expr.c_str(), node_id, success);
}

/* Helper to check if node is leader */
bool is_node_leader(int node_id, bool *success) {
    if (!is_node_initialized(node_id)) {
        *success = false;
        return false;
    }
    std::string state_expr = fmt::format("((raft_server_private_t*)raft)->state");
    int state = eval<int>(state_expr.c_str(), node_id, success);
    return *success && state == 3;  // RAFT_STATE_LEADER = 3
}

/* Helper to get snapshot term */
long get_snapshot_term(int node_id, bool *success) {
    if (!is_node_initialized(node_id)) {
        *success = false;
        return 0;
    }
    std::string snap_term_expr = fmt::format("((raft_server_private_t*)raft)->snapshot_last_term");
    return eval<long>(snap_term_expr.c_str(), node_id, success);
}

/* Check term monotonicity for a single node
 * 
 * The key insight: we compare current term with the stored term from
 * ptmc_state.raft_states[], which is specific to this state tree node.
 * This ensures BFS correctness - different branches don't interfere.
 */
int check_term_monotonicity(int node_id) {
    bool success = false;
    long current_term = get_node_term(node_id, &success);
    
    if (!success || current_term < 0) {
        // Node might not be initialized yet
        // ptmc_state.raft_states[node_id].current_term = 0;
        return 0;
    }
    
    // Get the previous term from ptmc_state (serialized with this state)
    long prev_term = ptmc_state.raft_states[node_id].current_term;
    
    // Check: term should never decrease
    if (current_term < prev_term) {
        printf("\n[CHECK FAILED] Node %d term decreased: %d -> %d\n",
               node_id, prev_term, current_term);
        printf("  This violates Raft's term monotonicity property!\n");
        printf("  Bug scenario: Node loaded snapshot with stale term\n");
        return 1;
    }
    
    // Check: term should not be less than snapshot term
    long snap_term = get_snapshot_term(node_id, &success);
    if (success && snap_term > 0 && current_term < snap_term) {
        printf("\n[CHECK FAILED] Node %d current_term(%ld) < snapshot_term(%ld)\n",
               node_id, current_term, snap_term);
        printf("  This is the 'stale snapshot' bug!\n");
        return 1;
    }
    
    // Update ptmc_state with current values for next check
    ptmc_state.raft_states[node_id].current_term = current_term;
    
    return 0;
}

/* Check leader uniqueness per term
 * 
 * Leader information is also stored per-state, so we can detect
 * violations that occur in specific execution paths.
 */
int check_leader_uniqueness() {
    // Track leaders per term within this check invocation
    // (this is local to the check, not across states)
    long term_leaders[100] = {0};
    long term_leader_count[100] = {0};
    
    for (int i = 0; i < NP; i++) {
        bool success = false;
        if (is_node_leader(i, &success)) {
            int term = get_node_term(i, &success);
            
            if (success && term >= 0 && term < 100) {
                term_leader_count[term]++;
                term_leaders[term] = i;
                
                if (term_leader_count[term] > 1) {
                    printf("\n[CHECK FAILED] Multiple leaders in term %d!\n", term);
                    printf("  Nodes %d and %d both claim leadership\n", 
                           term_leaders[term], i);
                    printf("  This violates Raft's safety property!\n");
                    return 1;
                }
            }
        }
        
        // Update leader status in ptmc_state
        ptmc_state.raft_states[i].is_leader = is_node_leader(i, &success) ? 1 : 0;
    }
    
    return 0;
}

/* Check for term regression after snapshot load */
int check_snapshot_term_consistency() {
    for (int i = 0; i < NP; i++) {
        // Skip if node not initialized
        if (!is_node_initialized(i)) {
            continue;
        }
        
        bool success = false;
        std::string snap_idx_expr = fmt::format("((raft_server_private_t*)nodes[{}])->snapshot_last_idx", i);
        long snap_idx = eval<long>(snap_idx_expr.c_str(), i, &success);
        
        long snap_term = get_snapshot_term(i, &success);
        long current_term = get_node_term(i, &success);
        
        // If node has a snapshot, verify term consistency
        if (success && snap_idx > 0 && snap_term > 0) {
            // The bug: if snapshot_term > current_term, node loaded stale snapshot
            if (snap_term > current_term) {
                printf("\n[CHECK FAILED] Node %d loaded stale snapshot!\n", i);
                printf("  snapshot_term=%ld, current_term=%ld\n", snap_term, current_term);
                printf("  snapshot_idx=%ld\n", snap_idx);
                printf("  This violates Raft's snapshot consistency!\n");
                return 1;
            }
        }
        
        // Update snapshot term in ptmc_state
        ptmc_state.raft_states[i].last_log_term = snap_term;
    }
    return 0;
}

/* Main check function called by detsim
 * 
 * This is called after each state transition. The ptmc_state.raft_states[]
 * contains the values from the PREVIOUS state (restored during load).
 * We compare with CURRENT values from tracee memory to detect violations.
 */
int check() {
    int result = 0;
    
    // Check all nodes
    for (int i = 0; i < NP; i++) {
        result |= check_term_monotonicity(i);
    }
    
    result |= check_leader_uniqueness();
    result |= check_snapshot_term_consistency();
    
    return result;
}

} // extern "C"
