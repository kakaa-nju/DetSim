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
/* Use xxHash for fast hash computation */
#include "xxhash.h"

static uint32_t compute_hash(const void* data, size_t len) {
    // xxHash is 2-5x faster than CRC32
    return XXHash64::hash32(data, len, 0);
}

/* ======================================================================
 * StateStore Implementation
 * ====================================================================== */

StateStore::StateStore() : current_memory_usage_(0) {
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
    std::lock_guard<std::mutex> cache_lock(cache_mutex_);
    std::lock_guard<std::mutex> lru_lock(lru_mutex_);
    
    while (current_memory_usage_ + required_bytes > config_.memory_cache_size 
           && !lru_list_.empty()) {
        // Evict oldest (back of list)
        hash_type oldest_hash = lru_list_.back();
        auto it = cache_.find(oldest_hash);
        
        if (it != cache_.end()) {
            auto& entry = it->second;
            
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
                size_t freed = entry->raw_data.size() - entry->compressed_data.size();
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
        }
        
        lru_list_.pop_back();
        lru_index_.erase(oldest_hash);
    }
}

hash_type StateStore::save(const void* data, size_t len) {
    if (!data || len == 0) return 0;
    
    // Compute hash from raw data
    hash_type hash = compute_hash(data, len);
    
    // Check if already exists
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = cache_.find(hash);
        if (it != cache_.end()) {
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
    
    // Update memory usage
    current_memory_usage_ += len;
    update_lru(hash);
    
    // Compress immediately (memory -> memory, fast)
    entry->compressed_data = compress(entry->raw_data);
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
        entry->last_access = ++access_counter_;
        
        StateEntry::State state = entry->state.load();
        
        // Return raw data if available
        if (!entry->raw_data.empty()) {
            out_data = entry->raw_data;
            LOG_TRACE("StateStore load hash %08x from raw memory", hash);
            return out_data.size();
        }
        
        // Decompress if needed
        if (!entry->compressed_data.empty()) {
            // Pass 0 to let decompress() figure out the size
            out_data = decompress(entry->compressed_data, 0);
            
            // Restore raw data if memory allows
            if (current_memory_usage_ + out_data.size() <= config_.memory_cache_size) {
                entry->raw_data = out_data;
                current_memory_usage_ += out_data.size();
                entry->state = StateEntry::State::RAW;
            }
            
            LOG_TRACE("StateStore load hash %08x from compressed memory", hash);
            return out_data.size();
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
            current_memory_usage_ += out_data.size();
            update_lru(hash);
            
            evict_if_needed(0);
            return out_data.size();
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
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = cache_.find(hash);
        if (it == cache_.end()) {
            // Not in cache, check disk
            return exists_on_disk(hash);
        }
        entry = it->second;
    }
    
    std::unique_lock<std::mutex> lock(entry->mutex);
    if (timeout_ms == 0) {
        entry->cv.wait(lock, [&entry]() {
            StateEntry::State s = entry->state.load();
            return s == StateEntry::State::PERSISTED || 
                   s == StateEntry::State::PERSISTING;
        });
        return true;
    } else {
        return entry->cv.wait_for(lock, 
            std::chrono::milliseconds(timeout_ms),
            [&entry]() {
                StateEntry::State s = entry->state.load();
                return s == StateEntry::State::PERSISTED || 
                       s == StateEntry::State::PERSISTING;
            });
    }
}

/* ======================================================================
 * Background I/O Worker
 * ====================================================================== */

void StateStore::io_worker() {
    while (!shutdown_) {
        StateEntryPtr entry;
        
        {
            std::unique_lock<std::mutex> lock(io_mutex_);
            io_cv_.wait(lock, [this] { 
                return !io_queue_.empty() || shutdown_; 
            });
            
            if (shutdown_) break;
            if (io_queue_.empty()) continue;
            
            entry = io_queue_.front();
            io_queue_.pop();
        }
        
        if (!entry || entry->compressed_data.empty()) continue;
        
        // Mark as persisting
        entry->state = StateEntry::State::PERSISTING;
        
        // Write to disk
        if (write_to_disk(entry->hash, entry->compressed_data)) {
            entry->state = StateEntry::State::PERSISTED;
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
    
    size_t bound = ZSTD_compressBound(data.size());
    std::vector<uint8_t> compressed(bound);
    
    size_t compressed_size = ZSTD_compress(
        compressed.data(), bound,
        data.data(), data.size(),
        config_.compression_level);
    
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
    if (original_size == 0 || original_size == ZSTD_CONTENTSIZE_UNKNOWN) {
        // Get actual content size from frame header
        unsigned long long frame_size = ZSTD_getFrameContentSize(data.data(), data.size());
        if (frame_size != ZSTD_CONTENTSIZE_UNKNOWN && frame_size != ZSTD_CONTENTSIZE_ERROR) {
            original_size = frame_size;
        } else {
            // Conservative estimate: compression ratio rarely exceeds 10x
            original_size = data.size() * 10;
        }
    }
    
    // Try decompression with potential retry
    std::vector<uint8_t> decompressed;
    size_t dst_capacity = original_size;
    
    while (dst_capacity <= original_size * 100) {  // Max 100x expansion
        decompressed.resize(dst_capacity);
        
        size_t result = ZSTD_decompress(
            decompressed.data(), dst_capacity,
            data.data(), data.size());
        
        if (!ZSTD_isError(result)) {
            decompressed.resize(result);
            return decompressed;
        }
        
        if (ZSTD_getErrorCode(result) == ZSTD_error_dstSize_tooSmall) {
            // Buffer too small, double it and retry
            dst_capacity *= 2;
            LOG_WARN("Decompression buffer too small, retrying with %zu bytes", dst_capacity);
        } else {
            LOG_ERROR("ZSTD decompression failed: %s", ZSTD_getErrorName(result));
            return {};
        }
    }
    
    LOG_ERROR("Decompression failed: max buffer size exceeded");
    return {};
}

/* ======================================================================
 * Disk Operations
 * ====================================================================== */

bool StateStore::write_to_disk(hash_type hash, const std::vector<uint8_t>& data) {
    std::string path = fileutils::format_hash_filename("memory", ".mem.zstd", hash);
    
    // Create parent directory if needed
    size_t last_slash = path.find_last_of('/');
    if (last_slash != std::string::npos) {
        std::string dir = path.substr(0, last_slash);
        namespace fs = std::filesystem;
        if (!fs::exists(dir)) {
            fs::create_directories(dir);
        }
    }
    
    // Write compressed data directly using the same format as compress_tmp_file
    // Format: [compressed_chunk_size][compressed_data]...
    FILE* fp = fopen(path.c_str(), "wb");
    if (!fp) {
        LOG_ERROR("Failed to open %s for writing", path.c_str());
        return false;
    }
    
    const size_t CHUNK_SIZE = 64 * 1024;
    size_t offset = 0;
    
    while (offset < data.size()) {
        size_t remaining = data.size() - offset;
        size_t to_compress = std::min(CHUNK_SIZE, remaining);
        
        // Compress chunk
        size_t compressed_bound = ZSTD_compressBound(to_compress);
        std::vector<uint8_t> compressed(compressed_bound);
        size_t compressed_size = ZSTD_compress(
            compressed.data(), compressed_bound,
            data.data() + offset, to_compress,
            config_.compression_level);
        
        if (ZSTD_isError(compressed_size)) {
            fclose(fp);
            return false;
        }
        
        // Write [size][data]
        if (fwrite(&compressed_size, sizeof(size_t), 1, fp) != 1 ||
            fwrite(compressed.data(), 1, compressed_size, fp) != compressed_size) {
            fclose(fp);
            return false;
        }
        
        offset += to_compress;
    }
    
    fclose(fp);
    return true;
}

bool StateStore::read_from_disk(hash_type hash, std::vector<uint8_t>& data) {
    std::string path = fileutils::format_hash_filename("memory", ".mem.zstd", hash);
    
    // Use decompress_file_tmp which handles the [size][compressed_data] format
    FILE* decompressed_fp = decompress_file_tmp(path.c_str());
    if (!decompressed_fp) {
        return false;
    }
    
    // Read all decompressed data
    fseek(decompressed_fp, 0, SEEK_END);
    long file_size = ftell(decompressed_fp);
    fseek(decompressed_fp, 0, SEEK_SET);
    
    if (file_size < 0) {
        fclose(decompressed_fp);
        return false;
    }
    
    data.resize(file_size);
    if (file_size > 0) {
        size_t read_size = fread(data.data(), 1, file_size, decompressed_fp);
        if (read_size != (size_t)file_size) {
            fclose(decompressed_fp);
            return false;
        }
    }
    
    fclose(decompressed_fp);
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
