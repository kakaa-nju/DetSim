/*
 * debug_memory.h - Memory debugging utilities for tracer
 */

#ifndef __DEBUG_MEMORY_H
#define __DEBUG_MEMORY_H

#include <cstddef>
#include <cstdio>
#include <string>

// Track memory usage of specific components
struct MemoryTracker {
    const char* name;
    size_t current_bytes = 0;
    size_t peak_bytes = 0;
    size_t count = 0;
    
    void alloc(size_t bytes) {
        current_bytes += bytes;
        count++;
        if (current_bytes > peak_bytes) peak_bytes = current_bytes;
    }
    
    void free(size_t bytes) {
        if (bytes <= current_bytes) current_bytes -= bytes;
        if (count > 0) count--;
    }
    
    void report() const;
};

// Global memory debug interface
namespace memory_debug {
    // Initialize tracking
    void init();
    
    // Report all tracked components
    void report_all();
    
    // Get RSS
    size_t get_rss_mb();
    
    // Track specific components
    extern MemoryTracker sys_state_objects;
    extern MemoryTracker tracee_state_objects;
    extern MemoryTracker state_tree_entries;
    extern MemoryTracker state_queue_entries;
    extern MemoryTracker sockstate_objects;
    extern MemoryTracker fsstate_objects;
    extern MemoryTracker l1_cache_entries;
    extern MemoryTracker l2_cache_entries;
}

// RAII tracker for automatic tracking
class AutoMemoryTrack {
    MemoryTracker& tracker_;
    size_t size_;
public:
    AutoMemoryTrack(MemoryTracker& tracker, size_t size) 
        : tracker_(tracker), size_(size) {
        tracker_.alloc(size_);
    }
    ~AutoMemoryTrack() {
        tracker_.free(size_);
    }
};

#define TRACK_ALLOC(tracker, size) AutoMemoryTrack _auto_track_##__LINE__(tracker, size)

#endif
