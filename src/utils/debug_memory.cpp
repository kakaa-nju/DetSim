/*
 * debug_memory.cpp - Memory debugging implementation
 */

#include "debug_memory.h"
#include <sys/types.h>
#include <unistd.h>
#include <fstream>
#include <string>

namespace memory_debug {
    MemoryTracker sys_state_objects{"sys_state objects"};
    MemoryTracker tracee_state_objects{"tracee_state objects"};
    MemoryTracker state_tree_entries{"state_tree entries"};
    MemoryTracker state_queue_entries{"state_queue entries"};
    MemoryTracker sockstate_objects{"SockState objects"};
    MemoryTracker fsstate_objects{"FileSystemState objects"};
    MemoryTracker l1_cache_entries{"L1 cache entries"};
    MemoryTracker l2_cache_entries{"L2 cache entries"};
}

using namespace memory_debug;

void MemoryTracker::report() const {
    printf("  %-30s: current=%8.2f MB, peak=%8.2f MB, count=%zu\n",
           name, current_bytes / (1024.0*1024.0), peak_bytes / (1024.0*1024.0), count);
}

void memory_debug::init() {
    printf("[Memory Debug] Initialized\n");
}

void memory_debug::report_all() {
    printf("\n========== MEMORY DEBUG REPORT ==========\n");
    printf("Current RSS: %zu MB\n", get_rss_mb());
    printf("\nComponent Usage:\n");
    
    sys_state_objects.report();
    tracee_state_objects.report();
    state_tree_entries.report();
    state_queue_entries.report();
    sockstate_objects.report();
    fsstate_objects.report();
    l1_cache_entries.report();
    l2_cache_entries.report();
    
    printf("=========================================\n\n");
}

size_t memory_debug::get_rss_mb() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.find("VmRSS:") == 0) {
            size_t rss_kb = 0;
            sscanf(line.c_str(), "VmRSS: %zu", &rss_kb);
            return rss_kb / 1024;
        }
    }
    return 0;
}
