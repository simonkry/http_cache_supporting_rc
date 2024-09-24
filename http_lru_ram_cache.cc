#include "http_lru_ram_cache.h"

namespace Envoy::Http {

void HTTPLRURAMCache::initCacheCapacity(uint32_t cacheCapacity) {
    std::unique_lock uniqueLock(shared_mtx_);
    if (capacity_ != 0) {
        return;
    }
    capacity_ = cacheCapacity;
}

CacheEntrySharedPtr HTTPLRURAMCache::at(const std::string& key) {
    std::shared_lock sharedLock(shared_mtx_);
    const auto &itCacheMap = cache_map_.find(key);
    if (itCacheMap == cache_map_.end()) {
        return nullptr;
    }
    CacheEntrySharedPtr value = itCacheMap->second->second;
    if (key != LRU_list_.front().first) {
        sharedLock.unlock();
        std::unique_lock uniqueLock(shared_mtx_);
        // Move the accessed node to the front (most recently used position)
        LRU_list_.erase(itCacheMap->second);
        LRU_list_.emplace_front(key, value);
        // Update the iterator in the map
        cache_map_[key] = LRU_list_.begin();
    }
    return value;
}

void HTTPLRURAMCache::insert(const std::string& key, const CacheEntrySharedPtr& value) {
    std::shared_lock sharedLock(shared_mtx_);
    const auto& itCacheMap = cache_map_.find(key);
    // In case inserting key that already exists
    if (itCacheMap != cache_map_.end()) {
        sharedLock.unlock();
        std::unique_lock uniqueLock(shared_mtx_);
        // Remove the old node from the list and map
        LRU_list_.erase(itCacheMap->second);
        cache_map_.erase(itCacheMap);
    }
    else {
        sharedLock.unlock();
    }
    std::unique_lock uniqueLock(shared_mtx_);
    // Insert the new node at the front of the list
    LRU_list_.emplace_front(key, value);
    cache_map_[key] = LRU_list_.begin();
    // If the cache size exceeds the capacity, remove the least recently used item
    uniqueLock.unlock();
    sharedLock.lock();
    if (cache_map_.size() > capacity_) {
        const auto& lastNodeKey = LRU_list_.back().first;
        sharedLock.unlock();
        uniqueLock.lock();
        LRU_list_.pop_back();
        cache_map_.erase(lastNodeKey);
    }
}

uint32_t HTTPLRURAMCache::getCacheCapacity() const {
    std::shared_lock sharedLock(shared_mtx_);
    return capacity_;
}

std::unordered_map<std::string, LRUList::iterator> HTTPLRURAMCache::getCacheMap() const {
    std::shared_lock sharedLock(shared_mtx_);
    return cache_map_;
}

} // namespace Envoy::Http
