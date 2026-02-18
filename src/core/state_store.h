/*
 * state_store.h - Transparent state storage with async persistence
 *
 * Design:
 * - save(data, len) -> hash: compress in memory, return hash immediately
 * - load(hash) -> data: query memory first, then disk
 * - Background threads handle decompression and disk I/O
 */

#ifndef __STATE_STORE_H
#define __STATE_STORE_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <list>
#include <atomic>
#include <condition_variable>
#include <thread>
#include <queue>
#include <functional>

using hash_type = uint32_t;

/* ======================================================================
 * Stored State Entry
 * ====================================================================== */

struct StateEntry {
    hash_type hash{0};  // Explicitly initialized
    
    // Uncompressed data (hot)
    std::vector<uint8_t> raw_data;
    
    // Compressed data (warm)
    std::vector<uint8_t> compressed_data;
    
    // State
    enum class State {
        RAW,            // Has uncompressed data
        COMPRESSED,     // Has compressed data, raw may be dropped
        PERSISTING,     // Being written to disk
        PERSISTED,      // On disk
        LOADING         // Being loaded from disk
    };
    std::atomic<State> state{State::RAW};
    
    // For LRU - explicitly initialized
    std::atomic<uint64_t> last_access{0};
    
    // For async notification
    std::condition_variable cv;
    std::mutex mutex;
    
    StateEntry() = default;
    explicit StateEntry(hash_type h) : hash(h), state(State::RAW), last_access(0) {}
};

using StateEntryPtr = std::shared_ptr<StateEntry>;

/* ======================================================================
 * State Store
 * ====================================================================== */

class StateStore {
public:
    struct Config {
        size_t memory_cache_size = 2ULL * 1024 * 1024 * 1024;  // 2GB uncompressed
        int compression_level = 1;
        int io_threads = 2;
    };
    
    StateStore();
    ~StateStore();
    
    void init(const Config& config);
    void init();
    
    // Save data: compress in memory, return hash immediately
    // Background thread will write to disk
    hash_type save(const void* data, size_t len);
    
    // Load data: check memory first, then disk
    // Returns actual size, or -1 if not found
    ssize_t load(hash_type hash, std::vector<uint8_t>& out_data);
    
    // Check if data is available (in memory or on disk)
    bool exists(hash_type hash);
    
    // Wait for data to be persisted to disk
    bool wait_persisted(hash_type hash, uint64_t timeout_ms = 0);
    
    // Get entry for direct access (advanced usage)
    StateEntryPtr get_entry(hash_type hash);
    
    // Shutdown and wait for all pending writes
    void shutdown();
    
    static StateStore& instance();

private:
    Config config_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> shutdown_{false};
    
    // Memory cache: hash -> entry
    std::unordered_map<hash_type, StateEntryPtr> cache_;
    std::mutex cache_mutex_;
    
    // LRU tracking
    std::list<hash_type> lru_list_;
    std::unordered_map<hash_type, std::list<hash_type>::iterator> lru_index_;
    std::mutex lru_mutex_;
    
    std::atomic<size_t> current_memory_usage_{0};
    std::atomic<uint64_t> access_counter_{0};
    
    // Background I/O
    std::queue<StateEntryPtr> io_queue_;
    std::mutex io_mutex_;
    std::condition_variable io_cv_;
    std::vector<std::thread> io_threads_;
    
    void io_worker();
    
    // Internal helpers
    StateEntryPtr find_or_create_entry(hash_type hash);
    void update_lru(hash_type hash);
    void evict_if_needed(size_t required_bytes);
    
    // Compression
    std::vector<uint8_t> compress(const std::vector<uint8_t>& data);
    std::vector<uint8_t> decompress(const std::vector<uint8_t>& data, size_t original_size);
    
    // Disk operations
    bool write_to_disk(hash_type hash, const std::vector<uint8_t>& data);
    bool read_from_disk(hash_type hash, std::vector<uint8_t>& data);
    bool exists_on_disk(hash_type hash);
};

/* ======================================================================
 * C-style Interface (for easy integration)
 * ====================================================================== */

#ifdef __cplusplus
extern "C" {
#endif

void state_store_init(void);
uint32_t state_store_save(const void* data, size_t len);
ssize_t state_store_load(uint32_t hash, void* data, size_t max_len);
int state_store_exists(uint32_t hash);

#ifdef __cplusplus
}
#endif

#endif /* __STATE_STORE_H */
