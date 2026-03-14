/*
 * state_store.cpp - Tiered cache with prefetch implementation
 */

#include "state_store.h"
#include "debug.h"
#include "utils.h"
#include <zstd.h>
#include <zstd_errors.h>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <cerrno>
#include <cstdlib>  // for malloc/free
#include "xxhash.h"

static uint64_t compute_hash(const void* data, size_t len) {
    return XXHash64::hash(data, len, 0);
}

/* ==============================================================================
 * Constructor / Destructor
 * ============================================================================== */

StateStore::StateStore()
    : config_()
    , hot_memory_usage_(0)
    , warm_memory_usage_(0)
    , access_counter_(0) {
}

StateStore::~StateStore() {
    shutdown();
}

StateStore& StateStore::instance() {
    static StateStore instance;
    return instance;
}

void StateStore::init(const Config& config) {
    if (initialized_.exchange(true)) {
        LOG_WARN("StateStore already initialized");
        return;
    }
    
    config_ = config;
    
    // Check malloc tuning (glibc-specific)
    #ifdef __linux__
    const char* arena_max = getenv("MALLOC_ARENA_MAX");
    const char* mmap_threshold = getenv("MALLOC_MMAP_THRESHOLD_");
    
    if (!arena_max) {
        LOG_WARN("MALLOC_ARENA_MAX not set. Consider setting to 4 to limit 64MB arenas.");
        LOG_WARN("  Run with: export MALLOC_ARENA_MAX=4");
    }
    
    if (!mmap_threshold) {
        LOG_WARN("MALLOC_MMAP_THRESHOLD_ not set. 512KB vectors may create new arenas.");
        LOG_WARN("  Run with: export MALLOC_MMAP_THRESHOLD_=1073741824 (1GB)");
    }
    #endif
    
    // Start persistence workers
    for (size_t i = 0; i < config_.io_threads; i++) {
        io_threads_.emplace_back(&StateStore::io_worker, this);
    }
    
    // Start prefetch workers
    for (size_t i = 0; i < config_.prefetch_threads; i++) {
        prefetch_threads_.emplace_back(&StateStore::prefetch_worker, this);
    }
    
    LOG_INFO("StateStore initialized: hot=%zuMB, warm=%zuMB, prefetch_window=%zu",
             config_.hot_cache_size / (1024*1024),
             config_.warm_cache_size / (1024*1024),
             config_.prefetch_window);
}

void StateStore::init() {
    Config default_config;
    init(default_config);
}

void StateStore::shutdown() {
    if (shutdown_.exchange(true)) return;
    
    // Notify all workers and waiting producers
    io_cv_.notify_all();
    io_queue_not_full_cv_.notify_all();
    prefetch_cv_.notify_all();
    inflight_cv_.notify_all();
    
    // Join persistence threads
    for (auto& t : io_threads_) {
        if (t.joinable()) t.join();
    }
    
    // Join prefetch threads
    for (auto& t : prefetch_threads_) {
        if (t.joinable()) t.join();
    }
    
    // Clear all caches to free memory (prevents "still reachable" in valgrind)
    {
        std::lock_guard<std::recursive_mutex> lock(hot_mutex_);
        hot_cache_.clear();
        hot_lru_.clear();
        hot_lru_index_.clear();
        hot_memory_usage_ = 0;
    }
    
    {
        std::lock_guard<std::recursive_mutex> lock(warm_mutex_);
        warm_cache_.clear();
        warm_lru_.clear();
        warm_lru_index_.clear();
        warm_memory_usage_ = 0;
    }
    
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        state_queue_.clear();
    }
    
    {
        std::lock_guard<std::mutex> lock(inflight_mutex_);
        prefetch_inflight_.clear();
    }
    
    {
        std::lock_guard<std::mutex> lock(prefetch_mutex_);
        while (!prefetch_queue_.empty()) {
            prefetch_queue_.pop();
        }
    }
    
    // Clear ZSTD context caches (prevents memory leak warnings)
    {
        std::lock_guard<std::mutex> lock(ctx_cache_mutex_);
        ctx_caches_.clear();
    }
    
    // Clear IO queue (should already be empty after workers join)
    {
        std::lock_guard<std::mutex> lock(io_mutex_);
        while (!io_queue_.empty()) {
            io_queue_.pop();
        }
    }
}

/* ==============================================================================
 * Queue Management (with Transparent Prefetch)
 * ============================================================================== */

void StateStore::queue_push_back(hash_type hash) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    state_queue_.push_back(hash);
}

hash_type StateStore::queue_front() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (state_queue_.empty()) {
        return 0;
    }
    return state_queue_.front();
}

void StateStore::queue_pop_front() {
    hash_type current_hash = 0;
    
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (state_queue_.empty()) return;
        current_hash = state_queue_.front();
        state_queue_.pop_front();
    }
    
    // Trigger prefetch for upcoming states
    submit_prefetch_tasks();
}

bool StateStore::queue_empty() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return state_queue_.empty();
}

size_t StateStore::queue_size() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return state_queue_.size();
}

void StateStore::queue_clear() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    state_queue_.clear();
}

hash_type StateStore::queue_peek(size_t offset) const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (offset >= state_queue_.size()) {
        return 0;
    }
    return state_queue_[offset];
}

/* ==============================================================================
 * Prefetch System
 * ============================================================================== */

void StateStore::submit_prefetch_tasks() {
    std::vector<hash_type> to_prefetch;
    
    {
        std::lock_guard<std::mutex> inflight_lock(inflight_mutex_);
        
        // Check if inflight is already at max
        if (prefetch_inflight_.size() >= config_.max_inflight_prefetch) {
            return;
        }
        
        for (size_t i = 0; i < config_.prefetch_window; i++) {
            hash_type h = queue_peek(i);
            if (h == 0) break;
            
            // Check: not in L1, not in L2, not already prefetching
            if (!in_l1(h) && !in_l2(h) && 
                prefetch_inflight_.find(h) == prefetch_inflight_.end()) {
                
                // Only prefetch tracee_state (stored as .mem.zstd files)
                // sys_state (.ss files) should not be prefetched
                std::string path = fileutils::format_hash_filename("memory", ".mem.zstd", h);
                if (!fileutils::file_exists(path)) {
                    continue;
                }
                
                prefetch_inflight_.insert(h);
                to_prefetch.push_back(h);
                
                if (prefetch_inflight_.size() >= config_.max_inflight_prefetch) {
                    break;
                }
            }
        }
    }
    
    // Submit tasks
    {
        std::lock_guard<std::mutex> lock(prefetch_mutex_);
        for (auto h : to_prefetch) {
            prefetch_queue_.push({h, std::chrono::steady_clock::now()});
        }
    }
    prefetch_cv_.notify_all();
    
    // Update stats
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.prefetch_issued += to_prefetch.size();
    }
}

void StateStore::prefetch_worker() {
    while (!shutdown_.load()) {
        PrefetchTask task;
        
        {
            std::unique_lock<std::mutex> lock(prefetch_mutex_);
            prefetch_cv_.wait(lock, [this] { 
                return !prefetch_queue_.empty() || shutdown_.load(); 
            });
            
            if (shutdown_.load()) break;
            if (prefetch_queue_.empty()) continue;
            
            task = prefetch_queue_.front();
            prefetch_queue_.pop();
        }
        
        // Check if already loaded by main thread
        if (in_l1(task.hash) || in_l2(task.hash)) {
            std::lock_guard<std::mutex> lock(inflight_mutex_);
            prefetch_inflight_.erase(task.hash);
            inflight_cv_.notify_all();
            continue;
        }
        
        // Try to load from disk to L2
        std::vector<uint8_t> compressed;
        if (read_compressed_from_disk(task.hash, compressed)) {
            insert_to_l2(task.hash, std::move(compressed));
            LOG_TRACE("Prefetched hash %016lx to L2", task.hash);
        }
        // Silently skip if not found (could be sys_state stored as .ss, or race condition)
        
        // Remove from inflight
        {
            std::lock_guard<std::mutex> lock(inflight_mutex_);
            prefetch_inflight_.erase(task.hash);
            inflight_cv_.notify_all();
        }
    }
}

void StateStore::prefetch(hash_type hash) {
    if (hash == 0) return;
    
    {
        std::lock_guard<std::mutex> lock(inflight_mutex_);
        if (in_l1(hash) || in_l2(hash) || 
            prefetch_inflight_.find(hash) != prefetch_inflight_.end()) {
            return;
        }
        prefetch_inflight_.insert(hash);
    }
    
    {
        std::lock_guard<std::mutex> lock(prefetch_mutex_);
        prefetch_queue_.push({hash, std::chrono::steady_clock::now()});
    }
    prefetch_cv_.notify_one();
}

void StateStore::prefetch_batch(const std::vector<hash_type>& hashes) {
    std::vector<hash_type> to_submit;
    
    {
        std::lock_guard<std::mutex> lock(inflight_mutex_);
        for (auto h : hashes) {
            if (h == 0) continue;
            if (!in_l1(h) && !in_l2(h) && 
                prefetch_inflight_.find(h) == prefetch_inflight_.end() &&
                prefetch_inflight_.size() < config_.max_inflight_prefetch) {
                prefetch_inflight_.insert(h);
                to_submit.push_back(h);
            }
        }
    }
    
    {
        std::lock_guard<std::mutex> lock(prefetch_mutex_);
        for (auto h : to_submit) {
            prefetch_queue_.push({h, std::chrono::steady_clock::now()});
        }
    }
    prefetch_cv_.notify_all();
}

bool StateStore::in_l1(hash_type hash) const {
    std::lock_guard<std::recursive_mutex> lock(hot_mutex_);
    auto it = hot_cache_.find(hash);
    return it != hot_cache_.end() && !it->second->raw_data.empty();
}

bool StateStore::in_l2(hash_type hash) const {
    std::lock_guard<std::recursive_mutex> lock(warm_mutex_);
    auto it = warm_cache_.find(hash);
    return it != warm_cache_.end() && !it->second->compressed_data.empty();
}

bool StateStore::in_preflight(hash_type hash) const {
    std::lock_guard<std::mutex> lock(inflight_mutex_);
    return prefetch_inflight_.find(hash) != prefetch_inflight_.end();
}

/* ==============================================================================
 * Cache Operations
 * ============================================================================== */

void StateStore::insert_to_l1(hash_type hash, std::vector<uint8_t>&& raw_data) {
    if (raw_data.empty()) return;
    
    evict_l1_if_needed(raw_data.size());
    
    std::lock_guard<std::recursive_mutex> lock(hot_mutex_);
    
    // Check if entry already exists
    auto existing = hot_cache_.find(hash);
    if (existing != hot_cache_.end()) {
        // Update existing entry: subtract old size, add new size
        hot_memory_usage_ -= existing->second->raw_data.size();
        existing->second->raw_data = std::move(raw_data);
        existing->second->state = StateEntry::State::RAW;
        existing->second->last_access = ++access_counter_;
        
        // Update LRU (move to front)
        auto idx_it = hot_lru_index_.find(hash);
        if (idx_it != hot_lru_index_.end()) {
            hot_lru_.erase(idx_it->second);
        }
        hot_lru_.push_front(hash);
        hot_lru_index_[hash] = hot_lru_.begin();
        
        hot_memory_usage_ += existing->second->raw_data.size();
        return;
    }
    
    // Create new entry
    auto entry = std::make_shared<StateEntry>(hash);
    entry->raw_data = std::move(raw_data);
    entry->state = StateEntry::State::RAW;
    entry->last_access = ++access_counter_;
    
    hot_cache_[hash] = entry;
    hot_lru_.push_front(hash);
    hot_lru_index_[hash] = hot_lru_.begin();
    
    hot_memory_usage_ += entry->raw_data.size();
}

void StateStore::insert_to_l2(hash_type hash, std::vector<uint8_t>&& compressed_data) {
    if (compressed_data.empty()) return;
    
    evict_l2_if_needed(compressed_data.size());
    
    std::lock_guard<std::recursive_mutex> lock(warm_mutex_);
    
    // Check if already exists
    auto it = warm_cache_.find(hash);
    if (it != warm_cache_.end()) {
        // Update existing entry
        warm_memory_usage_ -= it->second->compressed_data.size();
        it->second->compressed_data = std::move(compressed_data);
        it->second->state = StateEntry::State::COMPRESSED;
        it->second->last_access = ++access_counter_;
        warm_memory_usage_ += it->second->compressed_data.size();
        
        // Update LRU
        warm_lru_.erase(warm_lru_index_[hash]);
        warm_lru_.push_front(hash);
        warm_lru_index_[hash] = warm_lru_.begin();
        return;
    }
    
    auto entry = std::make_shared<StateEntry>(hash);
    entry->compressed_data = std::move(compressed_data);
    entry->state = StateEntry::State::COMPRESSED;
    entry->last_access = ++access_counter_;
    
    warm_cache_[hash] = entry;
    warm_lru_.push_front(hash);
    warm_lru_index_[hash] = warm_lru_.begin();
    
    warm_memory_usage_ += entry->compressed_data.size();
}

bool StateStore::move_l2_to_l1(hash_type hash) {
    StateEntryPtr entry;
    
    {
        std::lock_guard<std::recursive_mutex> lock(warm_mutex_);
        auto it = warm_cache_.find(hash);
        if (it == warm_cache_.end() || it->second->compressed_data.empty()) {
            return false;
        }
        entry = it->second;
    }
    
    // Decompress
    std::vector<uint8_t> raw = decompress(entry->compressed_data, 0);
    if (raw.empty()) {
        LOG_ERROR("Failed to decompress hash %016lx for L1 promotion", hash);
        return false;
    }
    
    // Insert to L1
    insert_to_l1(hash, std::move(raw));
    
    // Keep in L2 (compressed), update state
    {
        std::lock_guard<std::recursive_mutex> lock(warm_mutex_);
        entry->state = StateEntry::State::RAW;  // Now also in L1
    }
    
    return true;
}

void StateStore::evict_l1_if_needed(size_t required_bytes) {
    std::lock_guard<std::recursive_mutex> lock(hot_mutex_);
    
    while (hot_memory_usage_ + required_bytes > config_.hot_cache_size && 
           !hot_lru_.empty()) {
        hash_type oldest = hot_lru_.back();
        auto it = hot_cache_.find(oldest);
        
        if (it != hot_cache_.end()) {
            size_t freed = it->second->raw_data.size();
            // Force immediate memory release - swap with empty vector
            // shrink_to_fit() doesn't guarantee release for mmap'd memory
            std::vector<uint8_t>().swap(it->second->raw_data);
            hot_memory_usage_ -= freed;
            
            // Move state to COMPRESSED if still in L2, otherwise PERSISTED
            if (it->second->compressed_data.empty()) {
                it->second->state = StateEntry::State::PERSISTED;
            } else {
                it->second->state = StateEntry::State::COMPRESSED;
            }
            
            hot_cache_.erase(it);
        }
        
        hot_lru_.pop_back();
        hot_lru_index_.erase(oldest);
    }
}

void StateStore::evict_l2_if_needed(size_t required_bytes) {
    size_t total_evicted = 0;
    bool should_trim = false;
    
    {
        std::lock_guard<std::recursive_mutex> lock(warm_mutex_);
        
        while (warm_memory_usage_ + required_bytes > config_.warm_cache_size && 
               !warm_lru_.empty()) {
            hash_type oldest = warm_lru_.back();
            auto it = warm_cache_.find(oldest);
            
            if (it != warm_cache_.end()) {
                size_t freed = it->second->compressed_data.size();
                warm_memory_usage_ -= freed;
                total_evicted += freed;
                warm_cache_.erase(it);
            }
            
            warm_lru_.pop_back();
            warm_lru_index_.erase(oldest);
        }
        
        // Check if we should trim memory (outside lock)
        #ifdef __linux__
        if (config_.enable_malloc_trim && total_evicted > 0) {
            static std::atomic<size_t> accumulated_evicted{0};
            size_t new_total = accumulated_evicted += total_evicted;
            
            if (new_total >= config_.malloc_trim_threshold) {
                accumulated_evicted = 0;
                should_trim = true;
            }
        }
        #endif
    } // unlock here
    
    // Release memory back to OS (outside lock to avoid blocking)
    #ifdef __linux__
    if (should_trim) {
        malloc_trim(0);
    }
    #endif
}

void StateStore::update_hot_lru(hash_type hash) {
    std::lock_guard<std::recursive_mutex> lock(hot_mutex_);
    auto it = hot_lru_index_.find(hash);
    if (it != hot_lru_index_.end()) {
        hot_lru_.erase(it->second);
        hot_lru_.push_front(hash);
        it->second = hot_lru_.begin();
    }
}

void StateStore::update_warm_lru(hash_type hash) {
    std::lock_guard<std::recursive_mutex> lock(warm_mutex_);
    auto it = warm_lru_index_.find(hash);
    if (it != warm_lru_index_.end()) {
        warm_lru_.erase(it->second);
        warm_lru_.push_front(hash);
        it->second = warm_lru_.begin();
    }
}

/* ==============================================================================
 * Save / Load
 * ============================================================================== */

hash_type StateStore::save(const void* data, size_t len) {
    if (!data || len == 0) return 0;
    
    hash_type hash = compute_hash(data, len);
    
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.save_calls++;
    }
    
    // Check if already exists (single lock scope to avoid deadlock)
    bool in_hot = false, in_warm = false;
    {
        std::lock_guard<std::recursive_mutex> lock(hot_mutex_);
        auto it = hot_cache_.find(hash);
        in_hot = (it != hot_cache_.end() && !it->second->raw_data.empty());
        if (in_hot) {
            // Update LRU while holding lock
            auto idx_it = hot_lru_index_.find(hash);
            if (idx_it != hot_lru_index_.end()) {
                hot_lru_.erase(idx_it->second);
                hot_lru_.push_front(hash);
                idx_it->second = hot_lru_.begin();
            }
        }
    }
    
    if (in_hot) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.save_dedup_hot++;
        return hash;
    }
    
    if (in_l2(hash)) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.save_dedup_warm++;
        update_warm_lru(hash);
        return hash;
    }
    
    if (exists_on_disk(hash)) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.save_dedup_disk++;
        return hash;
    }
    
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.save_new++;
    }
    
    // Insert to L1 (raw) - NO COMPRESSION HERE
    std::vector<uint8_t> raw(len);
    memcpy(raw.data(), data, len);
    insert_to_l1(hash, std::move(raw));
    
    // Get entry for async processing (compression + persistence)
    StateEntryPtr entry;
    {
        std::lock_guard<std::recursive_mutex> lock(hot_mutex_);
        entry = hot_cache_[hash];
    }
    
    // NOTE: Compression is now done asynchronously in io_worker
    // This reduces main thread latency significantly
    
    // Trigger async compression and disk write
    // Use backpressure: wait if queue is full to prevent OOM
    {
        std::unique_lock<std::mutex> lock(io_mutex_);
        // Wait until queue has space (backpressure)
        io_queue_not_full_cv_.wait(lock, [this] {
            return io_queue_.size() < max_io_queue_size_ || shutdown_.load();
        });
        if (shutdown_.load()) return hash;
        io_queue_.push(entry);
    }
    io_cv_.notify_one();
    
    LOG_TRACE("StateStore saved hash %016lx: raw=%zu (async compression)", hash, len);
    return hash;
}

ssize_t StateStore::load(hash_type hash, std::vector<uint8_t>& out_data) {
    if (hash == 0) return -1;
    
    // Update statistics
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.load_requests++;
    }
    
    // Try L1 first
    {
        std::lock_guard<std::recursive_mutex> lock(hot_mutex_);
        auto it = hot_cache_.find(hash);
        if (it != hot_cache_.end() && !it->second->raw_data.empty()) {
            out_data = it->second->raw_data;
            it->second->last_access = ++access_counter_;
            // Update LRU inline (already holding lock)
            auto idx_it = hot_lru_index_.find(hash);
            if (idx_it != hot_lru_index_.end()) {
                hot_lru_.erase(idx_it->second);
                hot_lru_.push_front(hash);
                idx_it->second = hot_lru_.begin();
            }
            // Update stats
            {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                stats_.l1_hits++;
            }
            LOG_TRACE("StateStore load %016lx from L1", hash);
            return static_cast<ssize_t>(out_data.size());
        }
    }
    
    // Try L2 (decompress to L1)
    std::vector<uint8_t> compressed_from_l2;
    {
        std::lock_guard<std::recursive_mutex> lock(warm_mutex_);
        auto it = warm_cache_.find(hash);
        if (it != warm_cache_.end() && !it->second->compressed_data.empty()) {
            // Copy compressed data before releasing lock
            compressed_from_l2 = it->second->compressed_data;
            it->second->last_access = ++access_counter_;
            // Update LRU inline (already holding lock)
            auto idx_it = warm_lru_index_.find(hash);
            if (idx_it != warm_lru_index_.end()) {
                warm_lru_.erase(idx_it->second);
                warm_lru_.push_front(hash);
                idx_it->second = warm_lru_.begin();
            }
        }
    }
    
    // Decompress outside the lock
    if (!compressed_from_l2.empty()) {
        std::vector<uint8_t> decompressed = decompress(compressed_from_l2, 0);
        if (decompressed.empty()) {
            LOG_ERROR("Decompression failed for %016lx", hash);
            return -1;
        }
        
        // Promote to L1 if space allows (move to avoid copy)
        if (hot_memory_usage_ + decompressed.size() <= config_.hot_cache_size) {
            insert_to_l1(hash, std::move(decompressed));
            // Reference L1 data for return (avoid another copy)
            {
                std::lock_guard<std::recursive_mutex> lock(hot_mutex_);
                auto it = hot_cache_.find(hash);
                if (it != hot_cache_.end() && !it->second->raw_data.empty()) {
                    out_data = it->second->raw_data;
                } else {
                    out_data = std::move(decompressed);
                }
            }
        } else {
            out_data = std::move(decompressed);
        }
        
        // Update stats
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.l2_hits++;
        }
        LOG_TRACE("StateStore load %016lx from L2", hash);
        return static_cast<ssize_t>(out_data.size());
    }
    
    // Try disk
    if (exists_on_disk(hash)) {
        std::vector<uint8_t> disk_data;
        if (read_from_disk(hash, disk_data)) {
            // Insert to L1 first (move to avoid copy)
            insert_to_l1(hash, std::move(disk_data));
            
            // Reference L1 data for L2 compression and return
            std::vector<uint8_t> compressed;
            {
                std::lock_guard<std::recursive_mutex> lock(hot_mutex_);
                auto it = hot_cache_.find(hash);
                if (it != hot_cache_.end() && !it->second->raw_data.empty()) {
                    compressed = compress(it->second->raw_data);
                    out_data = it->second->raw_data;
                }
            }
            
            if (!compressed.empty()) {
                insert_to_l2(hash, std::move(compressed));
            }
            
            // Update stats
            {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                stats_.disk_reads++;
            }
            LOG_TRACE("StateStore load %016lx from disk", hash);
            return static_cast<ssize_t>(out_data.size());
        }
    }
    
    return -1;
}

bool StateStore::exists(hash_type hash) {
    if (in_l1(hash) || in_l2(hash)) return true;
    return exists_on_disk(hash);
}

StateEntryPtr StateStore::get_entry(hash_type hash) {
    {
        std::lock_guard<std::recursive_mutex> lock(hot_mutex_);
        auto it = hot_cache_.find(hash);
        if (it != hot_cache_.end()) return it->second;
    }
    {
        std::lock_guard<std::recursive_mutex> lock(warm_mutex_);
        auto it = warm_cache_.find(hash);
        if (it != warm_cache_.end()) return it->second;
    }
    return nullptr;
}

bool StateStore::wait_persisted(hash_type hash, uint64_t timeout_ms) {
    // Check disk first
    if (exists_on_disk(hash)) return true;
    
    // Get entry
    StateEntryPtr entry = get_entry(hash);
    if (!entry) {
        return exists_on_disk(hash);
    }
    
    std::unique_lock<std::mutex> lock(entry->mutex);
    
    auto check_state = [entry]() {
        auto s = entry->state.load();
        return s == StateEntry::State::PERSISTED || 
               s == StateEntry::State::PERSISTING;
    };
    
    if (timeout_ms == 0) {
        entry->cv.wait(lock, check_state);
        return true;
    } else {
        return entry->cv.wait_for(lock, 
            std::chrono::milliseconds(timeout_ms), check_state);
    }
}

/* ==============================================================================
 * Background I/O Worker (Persistence)
 * ============================================================================== */

void StateStore::io_worker() {
    while (!shutdown_.load()) {
        StateEntryPtr entry;
        
        {
            std::unique_lock<std::mutex> lock(io_mutex_);
            io_cv_.wait(lock, [this] { 
                return !io_queue_.empty() || shutdown_.load(); 
            });
            
            if (shutdown_.load()) break;
            if (io_queue_.empty()) continue;
            
            entry = io_queue_.front();
            io_queue_.pop();
            // Notify producers that queue has space
            io_queue_not_full_cv_.notify_one();
        }
        
        if (!entry || entry->raw_data.empty()) continue;
        
        // Async compression
        std::vector<uint8_t> compressed = compress(entry->raw_data);
        
        if (!compressed.empty()) {
            // Store compressed in entry
            {
                std::lock_guard<std::mutex> entry_lock(entry->mutex);
                entry->compressed_data = std::move(compressed);
                entry->state = StateEntry::State::COMPRESSED;
            }
            
            // Note: L2 cache is filled during load, not here (to avoid deadlocks)
            // Mark as persisting and write to disk
            {
                std::lock_guard<std::mutex> entry_lock(entry->mutex);
                entry->state = StateEntry::State::PERSISTING;
            }
            
            bool success = write_to_disk(entry->hash, entry->compressed_data);
            
            if (success) {
                // Move from L1 to L2: keep compressed, free raw
                size_t freed_bytes = 0;
                {
                    std::lock_guard<std::mutex> entry_lock(entry->mutex);
                    entry->state = StateEntry::State::PERSISTED;
                    freed_bytes = entry->raw_data.size();
                    // Force immediate memory release - swap with empty vector
                    std::vector<uint8_t>().swap(entry->raw_data);
                }
                
                // Move entry from hot_cache to warm_cache (L1 -> L2)
                if (freed_bytes > 0) {
                    std::lock_guard<std::recursive_mutex> hot_lock(hot_mutex_);
                    auto it = hot_cache_.find(entry->hash);
                    if (it != hot_cache_.end()) {
                        hot_cache_.erase(it);
                        auto lru_it = hot_lru_index_.find(entry->hash);
                        if (lru_it != hot_lru_index_.end()) {
                            hot_lru_.erase(lru_it->second);
                            hot_lru_index_.erase(lru_it);
                        }
                        hot_memory_usage_ -= freed_bytes;
                    }
                }
                
                // Insert to L2 (warm cache)
                // Use swap to avoid copy: L2 gets the data, entry keeps empty vector
                std::vector<uint8_t> compressed_for_l2;
                {
                    std::lock_guard<std::mutex> entry_lock(entry->mutex);
                    compressed_for_l2.swap(entry->compressed_data);
                }
                insert_to_l2(entry->hash, std::move(compressed_for_l2));
                
                LOG_TRACE("Persisted hash %016lx (async), moved to L2, freed %zu bytes from L1", 
                          entry->hash, freed_bytes);
            } else {
                LOG_ERROR("Failed to persist hash %016lx", entry->hash);
            }
            
            entry->cv.notify_all();
        }
    }
}

/* ==============================================================================
 * Compression
 * ============================================================================== */

/* ==============================================================================
 * ZSTD Context Cache Implementation
 * Prevents repeated 128MB heap segment allocations by reusing contexts
 * ============================================================================== */

void StateStore::ZSTDContextCache::reset() {
    if (cctx) {
        ZSTD_freeCCtx(cctx);
        cctx = nullptr;
    }
    if (dctx) {
        ZSTD_freeDCtx(dctx);
        dctx = nullptr;
    }
}

ZSTD_CCtx* StateStore::ZSTDContextCache::getCCtx() {
    if (!cctx) {
        cctx = ZSTD_createCCtx();
    }
    return cctx;
}

ZSTD_DCtx* StateStore::ZSTDContextCache::getDCtx() {
    if (!dctx) {
        dctx = ZSTD_createDCtx();
    }
    return dctx;
}

StateStore::ZSTDContextCache* StateStore::get_thread_ctx_cache() {
    std::thread::id tid = std::this_thread::get_id();
    
    {
        std::lock_guard<std::mutex> lock(ctx_cache_mutex_);
        auto it = ctx_caches_.find(tid);
        if (it != ctx_caches_.end()) {
            return it->second.get();
        }
    }
    
    // Create new cache for this thread
    auto cache = std::make_unique<ZSTDContextCache>();
    auto* ptr = cache.get();
    
    {
        std::lock_guard<std::mutex> lock(ctx_cache_mutex_);
        ctx_caches_[tid] = std::move(cache);
    }
    
    return ptr;
}

std::vector<uint8_t> StateStore::compress(const std::vector<uint8_t>& data) {
    if (data.empty()) return {};
    
    int level = config_.compression_level;
    if (level < 1) level = 1;
    if (level > 22) level = 22;
    
    size_t bound = ZSTD_compressBound(data.size());
    if (bound == 0 || bound > SIZE_MAX / 2) {
        LOG_ERROR("ZSTD_compressBound failed");
        return {};
    }
    
    // Use C-style malloc to avoid std::vector resize using mmap
    // malloc respects MALLOC_MMAP_THRESHOLD_ env var, vector::resize does not
    uint8_t* compressed_buf = (uint8_t*)malloc(bound);
    if (!compressed_buf) {
        LOG_ERROR("Failed to allocate compression buffer (malloc %zu failed)", bound);
        return {};
    }
    
    // Use cached context to avoid repeated internal allocations
    auto* cache = get_thread_ctx_cache();
    std::lock_guard<std::mutex> cache_lock(cache->mutex);
    
    ZSTD_CCtx* cctx = cache->getCCtx();
    if (!cctx) {
        LOG_ERROR("Failed to create ZSTD compression context");
        free(compressed_buf);
        return {};
    }
    
    size_t result = ZSTD_compressCCtx(cctx, compressed_buf, bound,
                                      data.data(), data.size(), level);
    
    if (ZSTD_isError(result)) {
        LOG_ERROR("ZSTD compression failed: %s", ZSTD_getErrorName(result));
        free(compressed_buf);
        // Reset context on error (might be corrupted)
        cache->reset();
        return {};
    }
    
    // Transfer to vector with exact size (this will do heap allocation via malloc)
    std::vector<uint8_t> compressed;
    compressed.reserve(result);  // Reserve, don't resize - may avoid mmap
    compressed.assign(compressed_buf, compressed_buf + result);
    free(compressed_buf);
    return compressed;
}

std::vector<uint8_t> StateStore::decompress(const std::vector<uint8_t>& data, size_t original_size) {
    if (data.empty()) return {};
    
    size_t dst_capacity = original_size;
    if (original_size == 0 || original_size == ZSTD_CONTENTSIZE_UNKNOWN) {
        unsigned long long frame_size = ZSTD_getFrameContentSize(data.data(), data.size());
        if (frame_size != ZSTD_CONTENTSIZE_UNKNOWN && frame_size != ZSTD_CONTENTSIZE_ERROR) {
            if (frame_size > SIZE_MAX) {
                LOG_ERROR("Frame content size too large");
                return {};
            }
            dst_capacity = static_cast<size_t>(frame_size);
        } else {
            dst_capacity = data.size() * 10;  // Conservative estimate
        }
    }
    
    if (dst_capacity < 64) dst_capacity = 64;
    
    const size_t max_factor = 500;
    size_t max_capacity = (data.size() > SIZE_MAX / max_factor) ? 
                          SIZE_MAX : data.size() * max_factor;
    
    // Use cached context
    auto* cache = get_thread_ctx_cache();
    std::lock_guard<std::mutex> cache_lock(cache->mutex);
    
    ZSTD_DCtx* dctx = cache->getDCtx();
    if (!dctx) {
        LOG_ERROR("Failed to create ZSTD decompression context");
        return {};
    }
    
    while (dst_capacity <= max_capacity) {
        // Use C-style malloc to avoid std::vector resize using mmap
        uint8_t* decompressed_buf = (uint8_t*)malloc(dst_capacity);
        if (!decompressed_buf) {
            LOG_ERROR("Failed to allocate decompression buffer (malloc %zu failed)", dst_capacity);
            return {};
        }
        
        size_t result = ZSTD_decompressDCtx(dctx, decompressed_buf, dst_capacity,
                                            data.data(), data.size());
        
        if (!ZSTD_isError(result)) {
            // Success: transfer to vector with exact size
            std::vector<uint8_t> decompressed;
            decompressed.reserve(result);
            decompressed.assign(decompressed_buf, decompressed_buf + result);
            free(decompressed_buf);
            return decompressed;
        }
        
        free(decompressed_buf);
        
        if (ZSTD_getErrorCode(result) == ZSTD_error_dstSize_tooSmall) {
            if (dst_capacity > max_capacity / 2) break;
            dst_capacity *= 2;
        } else {
            LOG_ERROR("ZSTD decompression failed: %s", ZSTD_getErrorName(result));
            // Reset context on error
            cache->reset();
            return {};
        }
    }
    
    LOG_ERROR("Decompression buffer exceeded max capacity");
    return {};
}

/* ==============================================================================
 * Disk Operations
 * ============================================================================== */

bool StateStore::write_to_disk(hash_type hash, const std::vector<uint8_t>& data) {
    std::string path = fileutils::format_hash_filename("memory", ".mem.zstd", hash);
    fileutils::ensure_directory_for_file(path);
    
    FILE* fp = fopen(path.c_str(), "wb");
    if (!fp) {
        LOG_ERROR("Failed to open %s: %s", path.c_str(), strerror(errno));
        return false;
    }
    
    struct FileGuard {
        FILE* fp;
        FileGuard(FILE* f) : fp(f) {}
        ~FileGuard() { if (fp) fclose(fp); }
    } file_guard(fp);
    
    // Write raw compressed data directly (our format)
    if (fwrite(data.data(), 1, data.size(), fp) != data.size()) {
        LOG_ERROR("Failed to write data to %s", path.c_str());
        return false;
    }
    
    if (fflush(fp) != 0) {
        LOG_ERROR("Failed to flush %s: %s", path.c_str(), strerror(errno));
        return false;
    }
    
    return true;
}

bool StateStore::read_from_disk(hash_type hash, std::vector<uint8_t>& data) {
    std::string path = fileutils::format_hash_filename("memory", ".mem.zstd", hash);
    
    if (!fileutils::file_exists(path)) {
        return false;
    }
    
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) {
        LOG_ERROR("Failed to open %s: %s", path.c_str(), strerror(errno));
        return false;
    }
    
    struct FileGuard {
        FILE* fp;
        FileGuard(FILE* f) : fp(f) {}
        ~FileGuard() { if (fp) fclose(fp); }
    } file_guard(fp);
    
    if (fseek(fp, 0, SEEK_END) != 0) {
        LOG_ERROR("Failed to seek end of %s", path.c_str());
        return false;
    }
    
    long size = ftell(fp);
    if (size < 0) {
        LOG_ERROR("Failed to get size of %s", path.c_str());
        return false;
    }
    
    if (size == 0) {
        data.clear();
        return true;
    }
    
    if (fseek(fp, 0, SEEK_SET) != 0) {
        LOG_ERROR("Failed to seek start of %s", path.c_str());
        return false;
    }
    
    std::vector<uint8_t> compressed(size);
    if (fread(compressed.data(), 1, size, fp) != static_cast<size_t>(size)) {
        LOG_ERROR("Failed to read %s", path.c_str());
        return false;
    }
    
    data = decompress(compressed, 0);
    return !data.empty();
}

bool StateStore::read_compressed_from_disk(hash_type hash, std::vector<uint8_t>& compressed_data) {
    std::string path = fileutils::format_hash_filename("memory", ".mem.zstd", hash);
    
    if (!fileutils::file_exists(path)) {
        return false;
    }
    
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) {
        LOG_ERROR("Failed to open %s: %s", path.c_str(), strerror(errno));
        return false;
    }
    
    struct FileGuard {
        FILE* fp;
        FileGuard(FILE* f) : fp(f) {}
        ~FileGuard() { if (fp) fclose(fp); }
    } file_guard(fp);
    
    if (fseek(fp, 0, SEEK_END) != 0) {
        LOG_ERROR("Failed to seek end of %s", path.c_str());
        return false;
    }
    
    long size = ftell(fp);
    if (size <= 0) {
        return false;
    }
    
    if (fseek(fp, 0, SEEK_SET) != 0) {
        LOG_ERROR("Failed to seek start of %s", path.c_str());
        return false;
    }
    
    try {
        compressed_data.resize(size);
    } catch (...) {
        LOG_ERROR("Failed to allocate buffer for %s", path.c_str());
        return false;
    }
    
    if (fread(compressed_data.data(), 1, size, fp) != static_cast<size_t>(size)) {
        LOG_ERROR("Failed to read %s", path.c_str());
        return false;
    }
    
    return true;
}

bool StateStore::exists_on_disk(hash_type hash) {
    std::string path = fileutils::format_hash_filename("memory", ".mem.zstd", hash);
    return fileutils::file_exists(path);
}

/* ==============================================================================
 * Statistics
 * ============================================================================== */

StateStore::Stats StateStore::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void StateStore::reset_stats() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_ = Stats();
}

size_t StateStore::get_l1_entry_count() const {
    std::lock_guard<std::recursive_mutex> lock(const_cast<std::recursive_mutex&>(hot_mutex_));
    return hot_cache_.size();
}

size_t StateStore::get_l2_entry_count() const {
    std::lock_guard<std::recursive_mutex> lock(const_cast<std::recursive_mutex&>(warm_mutex_));
    return warm_cache_.size();
}

size_t StateStore::get_io_queue_size() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(io_mutex_));
    return io_queue_.size();
}

size_t StateStore::get_prefetch_queue_size() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(prefetch_mutex_));
    return prefetch_queue_.size();
}

void StateStore::print_stats() const {
    Stats stats = get_stats();
    
    printf("\n=== StateStore Cache Statistics ===\n");
    printf("Load requests:     %lu\n", stats.load_requests);
    printf("  L1 hits:         %lu (%.1f%%)\n", stats.l1_hits, 
           stats.load_requests > 0 ? 100.0 * stats.l1_hits / stats.load_requests : 0.0);
    printf("  L2 hits:         %lu (%.1f%%)\n", stats.l2_hits,
           stats.load_requests > 0 ? 100.0 * stats.l2_hits / stats.load_requests : 0.0);
    printf("  Disk reads:      %lu (%.1f%%)\n", stats.disk_reads,
           stats.load_requests > 0 ? 100.0 * stats.disk_reads / stats.load_requests : 0.0);
    printf("  Hit rate:        %.1f%%\n", stats.hit_rate());
    
    size_t l1_usage = hot_memory_usage_.load();
    size_t l2_usage = warm_memory_usage_.load();
    printf("\nCache Usage:\n");
    printf("  L1 (Hot):        %zu MB / %zu MB (%.1f%%)\n",
           l1_usage / (1024*1024), config_.hot_cache_size / (1024*1024),
           100.0 * l1_usage / config_.hot_cache_size);
    printf("  L2 (Warm):       %zu MB / %zu MB (%.1f%%)\n",
           l2_usage / (1024*1024), config_.warm_cache_size / (1024*1024),
           100.0 * l2_usage / config_.warm_cache_size);
    printf("  Prefetch issued: %lu\n", stats.prefetch_issued);
    printf("====================================\n");
}

/* ==============================================================================
 * C Interface
 * ============================================================================== */

extern "C" {

void state_store_init(void) {
    StateStore::instance().init();
}

uint32_t state_store_save(const void* data, size_t len) {
    return StateStore::instance().save(data, len);
}

ssize_t state_store_load(uint64_t hash, void* data, size_t max_len) {
    std::vector<uint8_t> buffer;
    ssize_t result = StateStore::instance().load(hash, buffer);
    if (result < 0) return -1;
    
    size_t copy_len = std::min(static_cast<size_t>(result), max_len);
    memcpy(data, buffer.data(), copy_len);
    return result;
}

int state_store_exists(uint64_t hash) {
    return StateStore::instance().exists(hash) ? 1 : 0;
}

} // extern "C"
