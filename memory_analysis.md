# Memory Analysis Report for detsim

## Valgrind Report Summary (Key Entries)

### Major Allocation Sites:

1. **StateStore::load() - 182MB in 449 blocks**
   - Called by: tracee_state::tracee_state(hash), sys_state::sys_state(hash)
   - Allocation: out_data = raw_data (vector copy)
   - Status: "still reachable" - held by sys_state objects

2. **StateStore::load() - 111MB in 274 blocks**
   - Called by: restore_memory_mappings
   - Same issue: vector copy from L1 cache

3. **StateStore::load() - 93MB in 229 blocks**
   - Called by: restore_memory_mappings

4. **StateStore::io_worker() - 85MB in 21,067 blocks**
   - Allocation: compressed = compress(entry->raw_data)
   - Status: "still reachable" - held by L2 cache

5. **StateStore::insert_to_l2() - 3.7MB in 21,067 blocks**
   - Allocation: warm_cache entry creation
   - Status: "still reachable" - L2 cache metadata

## Memory Flow Analysis

### State Creation Path (300Hz):
```
capture_memory_state()
  -> StateStore::save()
    -> insert_to_l1() - allocates 400KB raw_data
    -> io_queue.push(entry)
      -> io_worker()
        -> compress() - allocates ~50KB compressed
        -> write_to_disk()
        -> insert_to_l2() - moves compressed to L2
        -> entry from L1 removed, but L2 holds data
```

### State Recovery Path (BFS):
```
state_queue_extract()
  -> new sys_state(hash)
    -> SysStateStore::load() - loads sys_state data
    -> for each child: tracee_state(hash)
      -> StateStore::load() - COPIES 400KB from L1/L2 to out_data
        -> cereal deserializes to tracee_state members
    -> delete state_fetched (frees sys_state, but...)
```

## Key Findings

### 1. Double Storage Issue
When `StateStore::load()` copies data from L1/L2 to `out_data`:
- L1/L2 cache keeps a copy
- `out_data` vector gets a copy
- After deserialization, `out_data` is destroyed
- But L1/L2 cache retains the data!

**Result**: Each unique state exists in TWO places:
- StateStore cache (L1 or L2)
- Deserialized sys_state/tracee_state objects

### 2. sys_state Lifecycle Issue
```cpp
// scheduler.cpp:865
ptmc_state.source_state = *state_fetched;  // COPY
```
- `state_fetched` is deleted at line 941
- But `ptmc_state.source_state` holds COPY of all data
- `ptmc_state` is global, destroyed at exit
- Memory accumulates in `source_state` until overwritten

### 3. L1 Cache Eviction Problem
evict_l1_if_needed() clears raw_data but:
- Only removes from hot_cache when evicting
- If entry also in L2, it stays in warm_cache
- Entry object itself (shared_ptr) not destroyed

## Root Cause

The 182MB "still reachable" from StateStore::load() is because:
1. load() copies data from cache to caller's vector
2. Caller deserializes and holds data in object members
3. Caller's vector is destroyed, but object members retain data
4. Object is copied to ptmc_state.source_state (global)
5. Original object is deleted, but global copy remains

**Conclusion**: This is NOT a leak - it's accumulated state data held by:
- StateStore L1/L2 caches (expected)
- ptmc_state.source_state/dest_state (global vars, destroyed at exit)

## Solutions

### 1. Limit L1 Cache Size (Already implemented, 512MB)
- Check if actually being enforced

### 2. Limit L2 Cache Size (Already implemented, 2GB)
- Check if eviction is working

### 3. Reduce Copy Operations
- Use move semantics in load()
- Return pointer to cached data instead of copy

### 4. Periodic Cache Cleanup
- Add explicit cache pruning during BFS
- Clear old entries from L2

### 5. Fix sys_state Copy
- Use move instead of copy for source_state
- Or clear source_state after use
