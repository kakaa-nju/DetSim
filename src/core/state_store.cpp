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
#include "xxhash.h"

static uint32_t compute_hash(const void* data, size_t len) {
    return XXHash64::hash32(data, len, 0);
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
    
    // Notify all workers
    io_cv_.notify_all();
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
            LOG_TRACE("Prefetched hash %08x to L2", task.hash);
        } else {
            LOG_TRACE("Prefetch failed for hash %08x (not on disk yet?)", task.hash);
        }
        
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
    std::lock_guard<std::mutex> lock(hot_mutex_);
    auto it = hot_cache_.find(hash);
    return it != hot_cache_.end() && !it->second->raw_data.empty();
}

bool StateStore::in_l2(hash_type hash) const {
    std::lock_guard<std::mutex> lock(warm_mutex_);
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
    
    std::lock_guard<std::mutex> lock(hot_mutex_);
    
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
    
    std::lock_guard<std::mutex> lock(warm_mutex_);
    
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
        std::lock_guard<std::mutex> lock(warm_mutex_);
        auto it = warm_cache_.find(hash);
        if (it == warm_cache_.end() || it->second->compressed_data.empty()) {
            return false;
        }
        entry = it->second;
    }
    
    // Decompress
    std::vector<uint8_t> raw = decompress(entry->compressed_data, 0);
    if (raw.empty()) {
        LOG_ERROR("Failed to decompress hash %08x for L1 promotion", hash);
        return false;
    }
    
    // Insert to L1
    insert_to_l1(hash, std::move(raw));
    
    // Keep in L2 (compressed), update state
    {
        std::lock_guard<std::mutex> lock(warm_mutex_);
        entry->state = StateEntry::State::RAW;  // Now also in L1
    }
    
    return true;
}

void StateStore::evict_l1_if_needed(size_t required_bytes) {
    std::lock_guard<std::mutex> lock(hot_mutex_);
    
    while (hot_memory_usage_ + required_bytes > config_.hot_cache_size && 
           !hot_lru_.empty()) {
        hash_type oldest = hot_lru_.back();
        auto it = hot_cache_.find(oldest);
        
        if (it != hot_cache_.end()) {
            size_t freed = it->second->raw_data.size();
            it->second->raw_data.clear();
            it->second->raw_data.shrink_to_fit();
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
    std::lock_guard<std::mutex> lock(warm_mutex_);
    
    while (warm_memory_usage_ + required_bytes > config_.warm_cache_size && 
           !warm_lru_.empty()) {
        hash_type oldest = warm_lru_.back();
        auto it = warm_cache_.find(oldest);
        
        if (it != warm_cache_.end()) {
            size_t freed = it->second->compressed_data.size();
            warm_memory_usage_ -= freed;
            warm_cache_.erase(it);
        }
        
        warm_lru_.pop_back();
        warm_lru_index_.erase(oldest);
    }
}

void StateStore::update_hot_lru(hash_type hash) {
    std::lock_guard<std::mutex> lock(hot_mutex_);
    auto it = hot_lru_index_.find(hash);
    if (it != hot_lru_index_.end()) {
        hot_lru_.erase(it->second);
        hot_lru_.push_front(hash);
        it->second = hot_lru_.begin();
    }
}

void StateStore::update_warm_lru(hash_type hash) {
    std::lock_guard<std::mutex> lock(warm_mutex_);
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
    
    // Check if already exists (single lock scope to avoid deadlock)
    bool in_hot = false, in_warm = false;
    {
        std::lock_guard<std::mutex> lock(hot_mutex_);
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
    
    if (in_hot || in_l2(hash) || exists_on_disk(hash)) {
        update_warm_lru(hash);
        return hash;
    }
    
    // Insert to L1 (raw)
    std::vector<uint8_t> raw(len);
    memcpy(raw.data(), data, len);
    insert_to_l1(hash, std::move(raw));
    
    // Get entry for persistence
    StateEntryPtr entry;
    {
        std::lock_guard<std::mutex> lock(hot_mutex_);
        entry = hot_cache_[hash];
    }
    
    // Compress for L2 and persistence
    std::vector<uint8_t> compressed = compress(entry->raw_data);
    if (!compressed.empty()) {
        // Store compressed in entry for persistence
        std::lock_guard<std::mutex> entry_lock(entry->mutex);
        entry->compressed_data = std::move(compressed);
        entry->state = StateEntry::State::COMPRESSED;
        
        // Also insert to L2
        std::vector<uint8_t> compressed_copy = entry->compressed_data;
        insert_to_l2(hash, std::move(compressed_copy));
    }
    
    // Trigger async disk write
    {
        std::lock_guard<std::mutex> lock(io_mutex_);
        io_queue_.push(entry);
    }
    io_cv_.notify_one();
    
    LOG_TRACE("StateStore saved hash %08x: raw=%zu", hash, len);
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
        std::lock_guard<std::mutex> lock(hot_mutex_);
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
            LOG_TRACE("StateStore load %08x from L1", hash);
            return static_cast<ssize_t>(out_data.size());
        }
    }
    
    // Try L2 (decompress to L1)
    std::vector<uint8_t> compressed_from_l2;
    {
        std::lock_guard<std::mutex> lock(warm_mutex_);
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
        out_data = decompress(compressed_from_l2, 0);
        if (out_data.empty()) {
            LOG_ERROR("Decompression failed for %08x", hash);
            return -1;
        }
        
        // Promote to L1 if space allows
        if (hot_memory_usage_ + out_data.size() <= config_.hot_cache_size) {
            insert_to_l1(hash, std::vector<uint8_t>(out_data));
        }
        
        // Update stats
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.l2_hits++;
        }
        LOG_TRACE("StateStore load %08x from L2", hash);
        return static_cast<ssize_t>(out_data.size());
    }
    
    // Try disk
    if (exists_on_disk(hash)) {
        if (read_from_disk(hash, out_data)) {
            // Insert to L1 and L2
            insert_to_l1(hash, std::vector<uint8_t>(out_data));
            
            std::vector<uint8_t> compressed = compress(out_data);
            if (!compressed.empty()) {
                insert_to_l2(hash, std::move(compressed));
            }
            
            // Update stats
            {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                stats_.disk_reads++;
            }
            LOG_TRACE("StateStore load %08x from disk", hash);
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
        std::lock_guard<std::mutex> lock(hot_mutex_);
        auto it = hot_cache_.find(hash);
        if (it != hot_cache_.end()) return it->second;
    }
    {
        std::lock_guard<std::mutex> lock(warm_mutex_);
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
        }
        
        if (!entry || entry->compressed_data.empty()) continue;
        
        // Mark as persisting
        {
            std::lock_guard<std::mutex> entry_lock(entry->mutex);
            entry->state = StateEntry::State::PERSISTING;
        }
        
        // Write to disk
        bool success = write_to_disk(entry->hash, entry->compressed_data);
        
        {
            std::lock_guard<std::mutex> entry_lock(entry->mutex);
            if (success) {
                entry->state = StateEntry::State::PERSISTED;
                LOG_TRACE("Persisted hash %08x", entry->hash);
            } else {
                LOG_ERROR("Failed to persist hash %08x", entry->hash);
            }
        }
        
        entry->cv.notify_all();
    }
}

/* ==============================================================================
 * Compression
 * ============================================================================== */

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
    
    std::vector<uint8_t> compressed;
    try {
        compressed.resize(bound);
    } catch (...) {
        LOG_ERROR("Failed to allocate compression buffer");
        return {};
    }
    
    size_t result = ZSTD_compress(compressed.data(), bound,
                                  data.data(), data.size(), level);
    
    if (ZSTD_isError(result)) {
        LOG_ERROR("ZSTD compression failed: %s", ZSTD_getErrorName(result));
        return {};
    }
    
    compressed.resize(result);
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
    
    const size_t max_factor = 100;
    size_t max_capacity = (data.size() > SIZE_MAX / max_factor) ? 
                          SIZE_MAX : data.size() * max_factor;
    
    while (dst_capacity <= max_capacity) {
        std::vector<uint8_t> decompressed;
        try {
            decompressed.resize(dst_capacity);
        } catch (...) {
            LOG_ERROR("Failed to allocate decompression buffer");
            return {};
        }
        
        size_t result = ZSTD_decompress(decompressed.data(), dst_capacity,
                                        data.data(), data.size());
        
        if (!ZSTD_isError(result)) {
            decompressed.resize(result);
            return decompressed;
        }
        
        if (ZSTD_getErrorCode(result) == ZSTD_error_dstSize_tooSmall) {
            if (dst_capacity > max_capacity / 2) break;
            dst_capacity *= 2;
        } else {
            LOG_ERROR("ZSTD decompression failed: %s", ZSTD_getErrorName(result));
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

ssize_t state_store_load(uint32_t hash, void* data, size_t max_len) {
    std::vector<uint8_t> buffer;
    ssize_t result = StateStore::instance().load(hash, buffer);
    if (result < 0) return -1;
    
    size_t copy_len = std::min(static_cast<size_t>(result), max_len);
    memcpy(data, buffer.data(), copy_len);
    return result;
}

int state_store_exists(uint32_t hash) {
    return StateStore::instance().exists(hash) ? 1 : 0;
}

} // extern "C"
