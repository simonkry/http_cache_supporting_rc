/***********************************************************************************************************************
 * CREDITS TO: https://www.geeksforgeeks.org/lru-cache-implementation-using-double-linked-lists
 ***********************************************************************************************************************/

#pragma once

#include "cache_entry.h"

constexpr uint32_t CACHE_CAPACITY = 1024;

namespace Envoy::Http {

using LRUList = std::list<std::pair<std::string, CacheEntrySharedPtr>>;

/**
 * @brief HTTP Least-Recently-Used RAM cache.
 */
class HTTPLRURAMCache : public Logger::Loggable<Logger::Id::filter> {
public:
    // Get the value for a given key
    CacheEntrySharedPtr at(const std::string& key);
    // Put a key-value pair into the cache
    void insert(const std::string& key, const CacheEntrySharedPtr& value);
    uint32_t getCacheCapacity() const;
    std::unordered_map<std::string, LRUList::iterator> getCacheMap() const;

private:
    const uint32_t capacity_ {CACHE_CAPACITY};
    mutable std::shared_mutex shared_mtx_ {};
    // std::unordered_map : Amortized Complexity: Due to rehashing, the amortized complexity
    // of operations (insertion, search) is O(1) on average
    std::unordered_map<std::string, LRUList::iterator> cache_map_ {};
    LRUList LRU_list_ {};
};

} // namespace Envoy::Http
