// 添加调试输出到 StateStore 的关键函数

// 1. 在 evict_l1_if_needed 添加调试
void StateStore::evict_l1_if_needed(size_t required_bytes) {
    std::lock_guard<std::recursive_mutex> lock(hot_mutex_);
    
    int eviction_count = 0;
    while (hot_memory_usage_ + required_bytes > config_.hot_cache_size && 
           !hot_lru_.empty()) {
        hash_type oldest = hot_lru_.back();
        auto it = hot_cache_.find(oldest);
        
        if (it != hot_cache_.end()) {
            size_t freed = it->second->raw_data.size();
            LOG_INFO("EVICT L1: hash=%08x, size=%zu, usage_before=%zu", 
                     oldest, freed, hot_memory_usage_.load());
            it->second->raw_data.clear();
            it->second->raw_data.shrink_to_fit();
            hot_memory_usage_ -= freed;
            
            if (it->second->compressed_data.empty()) {
                it->second->state = StateEntry::State::PERSISTED;
            } else {
                it->second->state = StateEntry::State::COMPRESSED;
            }
            
            hot_cache_.erase(it);
            eviction_count++;
        }
        
        hot_lru_.pop_back();
        hot_lru_index_.erase(oldest);
    }
    
    if (eviction_count > 0) {
        LOG_INFO("EVICT L1 DONE: evicted=%d, usage_after=%zu", 
                 eviction_count, hot_memory_usage_.load());
    }
}

// 2. 在 evict_l2_if_needed 添加调试
void StateStore::evict_l2_if_needed(size_t required_bytes) {
    std::lock_guard<std::recursive_mutex> lock(warm_mutex_);
    
    int eviction_count = 0;
    size_t total_freed = 0;
    
    while (warm_memory_usage_ + required_bytes > config_.warm_cache_size && 
           !warm_lru_.empty()) {
        hash_type oldest = warm_lru_.back();
        auto it = warm_cache_.find(oldest);
        
        if (it != warm_cache_.end()) {
            size_t freed = it->second->compressed_data.size();
            LOG_INFO("EVICT L2: hash=%08x, size=%zu, usage_before=%zu", 
                     oldest, freed, warm_memory_usage_.load());
            warm_memory_usage_ -= freed;
            total_freed += freed;
            warm_cache_.erase(it);
            eviction_count++;
        }
        
        warm_lru_.pop_back();
        warm_lru_index_.erase(oldest);
    }
    
    if (eviction_count > 0) {
        LOG_INFO("EVICT L2 DONE: evicted=%d, total_freed=%zu, usage_after=%zu", 
                 eviction_count, total_freed, warm_memory_usage_.load());
    }
}
