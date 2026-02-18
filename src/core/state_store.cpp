/*
 * state_store.cpp - Transparent state storage implementation
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
/* Use xxHash for fast hash computation */
#include "xxhash.h"

static uint32_t compute_hash(const void* data, size_t len) {
    // xxHash is 2-5x faster than CRC32
    return XXHash64::hash32(data, len, 0);
}

/* ======================================================================
 * StateStore Implementation
 * ====================================================================== */

StateStore::StateStore() 
    : config_()
    , current_memory_usage_(0) {
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
    
    // Start I/O workers
    for (int i = 0; i < config_.io_threads; i++) {
        io_threads_.emplace_back(&StateStore::io_worker, this);
    }
    
    LOG_INFO("StateStore initialized: memory_limit=%zuMB, io_threads=%d",
             config_.memory_cache_size / (1024*1024), config_.io_threads);
}

void StateStore::init() {
    Config default_config;
    init(default_config);
}

void StateStore::shutdown() {
    if (shutdown_.exchange(true)) return;
    
    io_cv_.notify_all();
    for (auto& t : io_threads_) {
        if (t.joinable()) t.join();
    }
}

StateEntryPtr StateStore::find_or_create_entry(hash_type hash) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = cache_.find(hash);
    if (it != cache_.end()) {
        return it->second;
    }
    
    auto entry = std::make_shared<StateEntry>(hash);
    cache_[hash] = entry;
    return entry;
}

void StateStore::update_lru(hash_type hash) {
    std::lock_guard<std::mutex> lock(lru_mutex_);
    
    auto it = lru_index_.find(hash);
    if (it != lru_index_.end()) {
        lru_list_.erase(it->second);
    }
    
    lru_list_.push_front(hash);
    lru_index_[hash] = lru_list_.begin();
}

void StateStore::evict_if_needed(size_t required_bytes) {
    // Always acquire locks in consistent order: cache_mutex_ first, then lru_mutex_
    std::lock_guard<std::mutex> cache_lock(cache_mutex_);
    std::lock_guard<std::mutex> lru_lock(lru_mutex_);
    
    while (current_memory_usage_ + required_bytes > config_.memory_cache_size 
           && !lru_list_.empty()) {
        // Evict oldest (back of list)
        hash_type oldest_hash = lru_list_.back();
        auto it = cache_.find(oldest_hash);
        
        if (it != cache_.end()) {
            auto& entry = it->second;
            
            // Lock entry state to prevent race conditions
            std::lock_guard<std::mutex> entry_lock(entry->mutex);
            
            // Only evict if persisted or compressible
            StateEntry::State state = entry->state.load();
            if (state == StateEntry::State::PERSISTED) {
                // Safe to remove both raw and compressed
                size_t freed = entry->raw_data.size() + entry->compressed_data.size();
                current_memory_usage_ -= freed;
                cache_.erase(it);
            } else if (state == StateEntry::State::RAW && entry->compressed_data.empty()) {
                // Compress before evicting
                entry->compressed_data = compress(entry->raw_data);
                // Check if compression succeeded
                if (entry->compressed_data.empty()) {
                    // Compression failed, skip this entry
                    LOG_WARN("Compression failed during eviction for hash %08x", oldest_hash);
                    // Move to next entry
                    lru_list_.pop_back();
                    lru_index_.erase(oldest_hash);
                    continue;
                }
                size_t raw_size = entry->raw_data.size();
                size_t compressed_size = entry->compressed_data.size();
                // Prevent underflow
                size_t freed = (raw_size > compressed_size) ? (raw_size - compressed_size) : 0;
                entry->raw_data.clear();
                entry->raw_data.shrink_to_fit();
                entry->state = StateEntry::State::COMPRESSED;
                current_memory_usage_ -= freed;
                
                if (current_memory_usage_ + required_bytes <= config_.memory_cache_size) {
                    break;
                }
            } else if (state == StateEntry::State::COMPRESSED) {
                // Just drop raw if exists
                size_t freed = entry->raw_data.size();
                entry->raw_data.clear();
                entry->raw_data.shrink_to_fit();
                current_memory_usage_ -= freed;
                
                if (current_memory_usage_ + required_bytes <= config_.memory_cache_size) {
                    break;
                }
            }
        } else {
            // Entry not in cache, clean up LRU tracking
            LOG_WARN("LRU entry %08x not found in cache during eviction", oldest_hash);
        }
        
        lru_list_.pop_back();
        lru_index_.erase(oldest_hash);
    }
}

hash_type StateStore::save(const void* data, size_t len) {
    if (!data || len == 0) return 0;
    
    // Compute hash from raw data
    hash_type hash = compute_hash(data, len);
    
    // Check if already exists (must hold lock during cache check and LRU update)
    {
        std::unique_lock<std::mutex> lock(cache_mutex_);
        auto it = cache_.find(hash);
        if (it != cache_.end()) {
            // Release cache_mutex_ before calling update_lru to avoid deadlock
            lock.unlock();
            update_lru(hash);
            return hash;  // Already cached
        }
    }
    
    // Check if exists on disk
    if (exists_on_disk(hash)) {
        return hash;  // Already persisted
    }
    
    // Create entry with raw data
    auto entry = find_or_create_entry(hash);
    entry->raw_data.resize(len);
    memcpy(entry->raw_data.data(), data, len);
    entry->state = StateEntry::State::RAW;
    
    // Update memory usage (atomic operation)
    current_memory_usage_.fetch_add(len);
    update_lru(hash);
    
    // Compress immediately (memory -> memory, fast)
    entry->compressed_data = compress(entry->raw_data);
    if (entry->compressed_data.empty() && !entry->raw_data.empty()) {
        LOG_ERROR("StateStore compression failed for hash %08x", hash);
        // Continue anyway - we still have raw data
    }
    entry->state = StateEntry::State::COMPRESSED;
    
    LOG_TRACE("StateStore saved hash %08x: raw=%zu, compressed=%zu (%.1f%%)",
              hash, len, entry->compressed_data.size(),
              100.0 * entry->compressed_data.size() / len);
    
    // Trigger async disk write
    {
        std::lock_guard<std::mutex> lock(io_mutex_);
        io_queue_.push(entry);
    }
    io_cv_.notify_one();
    
    // Evict if memory pressure
    evict_if_needed(0);
    
    return hash;
}

ssize_t StateStore::load(hash_type hash, std::vector<uint8_t>& out_data) {
    // Check memory cache
    StateEntryPtr entry;
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = cache_.find(hash);
        if (it != cache_.end()) {
            entry = it->second;
        }
    }
    
    if (entry) {
        update_lru(hash);
        entry->last_access.store(++access_counter_);
        
        // Lock entry for state check and data access
        std::lock_guard<std::mutex> entry_lock(entry->mutex);
        
        StateEntry::State state = entry->state.load();
        
        // Return raw data if available
        if (!entry->raw_data.empty()) {
            out_data = entry->raw_data;
            LOG_TRACE("StateStore load hash %08x from raw memory", hash);
            return static_cast<ssize_t>(out_data.size());
        }
        
        // Decompress if needed
        if (!entry->compressed_data.empty()) {
            // Pass 0 to let decompress() figure out the size
            out_data = decompress(entry->compressed_data, 0);
            
            if (out_data.empty()) {
                LOG_ERROR("StateStore decompression failed for hash %08x", hash);
                return -1;
            }
            
            // Restore raw data if memory allows
            size_t current_usage = current_memory_usage_.load();
            if (current_usage + out_data.size() <= config_.memory_cache_size) {
                entry->raw_data = out_data;
                current_memory_usage_.fetch_add(out_data.size());
                entry->state = StateEntry::State::RAW;
            }
            
            LOG_TRACE("StateStore load hash %08x from compressed memory", hash);
            return static_cast<ssize_t>(out_data.size());
        }
    }
    
    // Check disk
    if (exists_on_disk(hash)) {
        // read_from_disk already returns decompressed data
        if (read_from_disk(hash, out_data)) {
            LOG_TRACE("StateStore load hash %08x from disk", hash);
            
            if (out_data.empty()) {
                LOG_ERROR("Empty data read from disk for hash %08x", hash);
                return -1;
            }
            
            // Cache it
            auto new_entry = find_or_create_entry(hash);
            new_entry->raw_data = out_data;
            new_entry->state = StateEntry::State::RAW;
            current_memory_usage_.fetch_add(out_data.size());
            update_lru(hash);
            
            evict_if_needed(0);
            return static_cast<ssize_t>(out_data.size());
        }
    }
    
    return -1;  // Not found
}

bool StateStore::exists(hash_type hash) {
    // Check memory
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        if (cache_.find(hash) != cache_.end()) {
            return true;
        }
    }
    
    // Check disk
    return exists_on_disk(hash);
}

StateEntryPtr StateStore::get_entry(hash_type hash) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = cache_.find(hash);
    if (it != cache_.end()) {
        return it->second;
    }
    return nullptr;
}

bool StateStore::wait_persisted(hash_type hash, uint64_t timeout_ms) {
    StateEntryPtr entry;
    {
        std::unique_lock<std::mutex> lock(cache_mutex_);
        auto it = cache_.find(hash);
        if (it == cache_.end()) {
            // Not in cache, check disk
            lock.unlock();
            return exists_on_disk(hash);
        }
        entry = it->second;
    }
    
    if (!entry) {
        return exists_on_disk(hash);
    }
    
    std::unique_lock<std::mutex> lock(entry->mutex);
    
    // Use a copy of shared_ptr in lambda to ensure entry stays alive
    auto state_check = [entry]() {
        StateEntry::State s = entry->state.load();
        return s == StateEntry::State::PERSISTED || 
               s == StateEntry::State::PERSISTING;
    };
    
    if (timeout_ms == 0) {
        entry->cv.wait(lock, state_check);
        return true;
    } else {
        return entry->cv.wait_for(lock, 
            std::chrono::milliseconds(timeout_ms),
            state_check);
    }
}

/* ======================================================================
 * Background I/O Worker
 * ====================================================================== */

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
        
        // Lock entry before modifying state
        std::lock_guard<std::mutex> entry_lock(entry->mutex);
        
        // Mark as persisting
        entry->state.store(StateEntry::State::PERSISTING);
        
        // Release entry lock during I/O (write_to_disk is thread-safe)
        entry->mutex.unlock();
        
        // Write to disk
        bool success = write_to_disk(entry->hash, entry->compressed_data);
        
        // Re-acquire lock to update state
        entry->mutex.lock();
        
        if (success) {
            entry->state.store(StateEntry::State::PERSISTED);
            LOG_TRACE("StateStore persisted hash %08x to disk", entry->hash);
        } else {
            LOG_ERROR("StateStore failed to persist hash %08x", entry->hash);
        }
        
        entry->cv.notify_all();
    }
}

/* ======================================================================
 * Compression
 * ====================================================================== */

std::vector<uint8_t> StateStore::compress(const std::vector<uint8_t>& data) {
    if (data.empty()) return {};
    
    // Validate compression level (ZSTD supports 1-22)
    int level = config_.compression_level;
    if (level < 1) level = 1;
    if (level > 22) level = 22;
    
    size_t bound = ZSTD_compressBound(data.size());
    if (bound == 0 || bound > SIZE_MAX / 2) {
        LOG_ERROR("ZSTD compressBound returned invalid size: %zu", bound);
        return {};
    }
    
    std::vector<uint8_t> compressed;
    try {
        compressed.resize(bound);
    } catch (const std::bad_alloc& e) {
        LOG_ERROR("Failed to allocate compression buffer: %s", e.what());
        return {};
    }
    
    size_t compressed_size = ZSTD_compress(
        compressed.data(), bound,
        data.data(), data.size(),
        level);
    
    if (ZSTD_isError(compressed_size)) {
        LOG_ERROR("ZSTD compression failed: %s", ZSTD_getErrorName(compressed_size));
        return {};
    }
    
    compressed.resize(compressed_size);
    return compressed;
}

std::vector<uint8_t> StateStore::decompress(const std::vector<uint8_t>& data, size_t original_size) {
    if (data.empty()) return {};
    
    // If original_size is unknown or seems too small, use a safe estimate
    size_t dst_capacity = original_size;
    if (original_size == 0 || original_size == ZSTD_CONTENTSIZE_UNKNOWN) {
        // Get actual content size from frame header
        unsigned long long frame_size = ZSTD_getFrameContentSize(data.data(), data.size());
        if (frame_size != ZSTD_CONTENTSIZE_UNKNOWN && frame_size != ZSTD_CONTENTSIZE_ERROR) {
            if (frame_size > SIZE_MAX) {
                LOG_ERROR("ZSTD frame content size too large: %llu", frame_size);
                return {};
            }
            dst_capacity = static_cast<size_t>(frame_size);
        } else {
            // Conservative estimate: compression ratio rarely exceeds 10x
            if (data.size() > SIZE_MAX / 10) {
                LOG_ERROR("Data size too large for decompression estimate");
                return {};
            }
            dst_capacity = data.size() * 10;
        }
    } else {
        dst_capacity = original_size;
    }
    
    // Ensure minimum capacity
    if (dst_capacity < 64) {
        dst_capacity = 64;
    }
    
    // Limit max expansion to prevent memory exhaustion
    const size_t max_expansion_factor = 100;
    size_t max_capacity;
    if (data.size() > SIZE_MAX / max_expansion_factor) {
        max_capacity = SIZE_MAX;
    } else {
        max_capacity = data.size() * max_expansion_factor;
    }
    
    // Try decompression with potential retry
    std::vector<uint8_t> decompressed;
    
    while (dst_capacity <= max_capacity) {
        try {
            decompressed.resize(dst_capacity);
        } catch (const std::bad_alloc& e) {
            LOG_ERROR("Failed to allocate decompression buffer (%zu bytes): %s", 
                      dst_capacity, e.what());
            return {};
        }
        
        size_t result = ZSTD_decompress(
            decompressed.data(), dst_capacity,
            data.data(), data.size());
        
        if (!ZSTD_isError(result)) {
            decompressed.resize(result);
            return decompressed;
        }
        
        if (ZSTD_getErrorCode(result) == ZSTD_error_dstSize_tooSmall) {
            // Buffer too small, double it and retry
            if (dst_capacity > max_capacity / 2) {
                LOG_ERROR("Decompression failed: buffer expansion would exceed max capacity");
                return {};
            }
            dst_capacity *= 2;
            LOG_WARN("Decompression buffer too small, retrying with %zu bytes", dst_capacity);
        } else {
            LOG_ERROR("ZSTD decompression failed: %s (code: %zu)", 
                      ZSTD_getErrorName(result), ZSTD_getErrorCode(result));
            return {};
        }
    }
    
    LOG_ERROR("Decompression failed: max buffer size exceeded (%zu bytes)", dst_capacity);
    return {};
}

/* ======================================================================
 * Disk Operations
 * ====================================================================== */

bool StateStore::write_to_disk(hash_type hash, const std::vector<uint8_t>& data) {
    std::string path = fileutils::format_hash_filename("memory", ".mem.zstd", hash);
    
    // Create parent directory if needed
    fileutils::ensure_directory_for_file(path);
    
    // Write compressed data directly using the same format as compress_tmp_file
    // Format: [compressed_chunk_size][compressed_data]...
    FILE* fp = fopen(path.c_str(), "wb");
    if (!fp) {
        LOG_ERROR("Failed to open %s for writing: %s", path.c_str(), strerror(errno));
        return false;
    }
    
    // RAII wrapper to ensure fclose is called
    struct FileGuard {
        FILE* fp;
        FileGuard(FILE* f) : fp(f) {}
        ~FileGuard() { if (fp) fclose(fp); }
    } file_guard(fp);
    
    // Validate compression level
    int level = config_.compression_level;
    if (level < 1) level = 1;
    if (level > 22) level = 22;
    
    const size_t CHUNK_SIZE = 64 * 1024;
    size_t offset = 0;
    
    while (offset < data.size()) {
        size_t remaining = data.size() - offset;
        size_t to_compress = std::min(CHUNK_SIZE, remaining);
        
        // Compress chunk
        size_t compressed_bound = ZSTD_compressBound(to_compress);
        if (compressed_bound == 0) {
            LOG_ERROR("ZSTD_compressBound returned 0 for size %zu", to_compress);
            return false;
        }
        
        std::vector<uint8_t> compressed;
        try {
            compressed.resize(compressed_bound);
        } catch (const std::bad_alloc& e) {
            LOG_ERROR("Failed to allocate compression buffer: %s", e.what());
            return false;
        }
        
        size_t compressed_size = ZSTD_compress(
            compressed.data(), compressed_bound,
            data.data() + offset, to_compress,
            level);
        
        if (ZSTD_isError(compressed_size)) {
            LOG_ERROR("ZSTD compression failed: %s", ZSTD_getErrorName(compressed_size));
            return false;
        }
        
        // Write [size][data]
        if (fwrite(&compressed_size, sizeof(size_t), 1, fp) != 1) {
            LOG_ERROR("Failed to write chunk size to %s", path.c_str());
            return false;
        }
        if (fwrite(compressed.data(), 1, compressed_size, fp) != compressed_size) {
            LOG_ERROR("Failed to write compressed data to %s", path.c_str());
            return false;
        }
        
        offset += to_compress;
    }
    
    // Flush to ensure data is written to disk
    if (fflush(fp) != 0) {
        LOG_ERROR("Failed to flush data to %s: %s", path.c_str(), strerror(errno));
        return false;
    }
    
    return true;
}

bool StateStore::read_from_disk(hash_type hash, std::vector<uint8_t>& data) {
    std::string path = fileutils::format_hash_filename("memory", ".mem.zstd", hash);
    
    // Check if file exists first
    if (!fileutils::file_exists(path)) {
        return false;
    }
    
    // Use decompress_file_tmp which handles the [size][compressed_data] format
    FILE* decompressed_fp = decompress_file_tmp(path.c_str());
    if (!decompressed_fp) {
        LOG_ERROR("Failed to decompress file: %s", path.c_str());
        return false;
    }
    
    // RAII wrapper to ensure fclose is called
    struct FileGuard {
        FILE* fp;
        FileGuard(FILE* f) : fp(f) {}
        ~FileGuard() { if (fp) fclose(fp); }
    } file_guard(decompressed_fp);
    
    // Read all decompressed data
    if (fseek(decompressed_fp, 0, SEEK_END) != 0) {
        LOG_ERROR("Failed to seek to end of decompressed file");
        return false;
    }
    
    long file_size = ftell(decompressed_fp);
    if (file_size < 0) {
        LOG_ERROR("Failed to get file size: %s", strerror(errno));
        return false;
    }
    
    if (fseek(decompressed_fp, 0, SEEK_SET) != 0) {
        LOG_ERROR("Failed to seek to beginning of decompressed file");
        return false;
    }
    
    // Validate file size to prevent memory exhaustion
    if (file_size > static_cast<long>(config_.memory_cache_size)) {
        LOG_ERROR("Decompressed file size (%ld) exceeds memory cache limit", file_size);
        return false;
    }
    
    try {
        data.resize(file_size);
    } catch (const std::bad_alloc& e) {
        LOG_ERROR("Failed to allocate buffer for decompressed data: %s", e.what());
        return false;
    }
    
    if (file_size > 0) {
        size_t read_size = fread(data.data(), 1, file_size, decompressed_fp);
        if (read_size != static_cast<size_t>(file_size)) {
            if (ferror(decompressed_fp)) {
                LOG_ERROR("Error reading decompressed file: %s", strerror(errno));
            } else if (feof(decompressed_fp)) {
                LOG_ERROR("Unexpected EOF reading decompressed file (expected %ld, got %zu)", 
                          file_size, read_size);
            }
            return false;
        }
    }
    
    return true;
}

bool StateStore::exists_on_disk(hash_type hash) {
    std::string path = fileutils::format_hash_filename("memory", ".mem.zstd", hash);
    return fileutils::file_exists(path);
}

/* ======================================================================
 * C Interface
 * ====================================================================== */

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
    
    size_t copy_len = std::min((size_t)result, max_len);
    memcpy(data, buffer.data(), copy_len);
    return result;
}

int state_store_exists(uint32_t hash) {
    return StateStore::instance().exists(hash) ? 1 : 0;
}

}
