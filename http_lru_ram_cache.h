/***********************************************************************************************************************
 * INSPIRATION TAKEN FROM: https://www.geeksforgeeks.org/lru-cache-implementation-using-double-linked-lists
 ***********************************************************************************************************************/

#pragma once

#include "cache_entry.h"

namespace Envoy::Http {

using LRUList = std::list<std::pair<std::string, CacheEntrySharedPtr>>;

/**
 * @brief HTTP Least-Recently-Used RAM cache.
 * Uses double linked list from the standard library.
 */
class HTTPLRURAMCache : public Logger::Loggable<Logger::Id::filter> {
public:
    void initCacheCapacity(uint32_t cacheCapacity);
    // Get the value for a given key
    CacheEntrySharedPtr at(const std::string& key);
    // Put a key-value pair into the cache
    void insert(const std::string& key, const CacheEntrySharedPtr& value);
    uint32_t getCacheCapacity() const;
    std::unordered_map<std::string, LRUList::iterator> getCacheMap() const;

private:
    mutable std::shared_mutex shared_mtx_ {};
    uint32_t capacity_ {0};
    // std::unordered_map : Amortized Complexity: Due to rehashing, the amortized complexity
    // of operations (insertion, search) is O(1) on average
    std::unordered_map<std::string, LRUList::iterator> cache_map_ {};
    LRUList LRU_list_ {};
};

} // namespace Envoy::Http
