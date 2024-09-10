#pragma once

#include "hash_table_entry.h"

constexpr uint32_t LINEAR_PROBING_STEP = 17;

/***********************************************************************************************************************
 * INSPIRATION TAKEN FROM: https://embeddedartistry.com/blog/2017/05/17/creating-a-circular-buffer-in-c-and-c/
 ***********************************************************************************************************************/

namespace Envoy::Http {

/**
 * @brief Implements methods used by ring buffer cache - insertion, search, etc.
 * The buffer memory is allocated only on construction with uint32_t value. De-allocation happens thanks to std::unique_ptr.
 * Keeps track of number of elements - size_.
 */
class RingBufferHTTPCache : public Logger::Loggable<Logger::Id::filter> {
public:
    RingBufferHTTPCache() = default;
    explicit RingBufferHTTPCache(uint32_t capacity);
    bool insert(const HashTableEntrySharedPtr& entry);
    HashTableEntrySharedPtr at(const std::string & requestHeadersStr) const;
    void reset();
    bool empty() const;
    bool full() const;
    uint32_t capacity() const;
    uint32_t size() const;

private:
    const uint32_t capacity_ {};
    std::unique_ptr<HashTableEntrySharedPtr[]> buffer_ {};
    uint32_t size_ = 0;
};

/**
 * @brief Creates new ring buffer cache if the previous one is full.
 * Stores caches in std::vector.
 */
class RingBufferHTTPCacheFactory : public Logger::Loggable<Logger::Id::filter> {
public:
    RingBufferHTTPCacheFactory() = default;
    void initialize(uint32_t singleCacheCapacity);
    void insert(const HashTableEntrySharedPtr& entry);
    // No const methods because of locking std::mutex
    HashTableEntrySharedPtr at(const std::string & requestHeadersStr);
    const std::list<RingBufferHTTPCache> & getCaches();
    uint32_t getCacheCount();

private:
    std::mutex mtx_ {};
    uint32_t single_cache_capacity_ {};
    std::list<RingBufferHTTPCache> caches_ {};
    uint32_t cache_count_ = 0;
};

} // namespace Envoy::Http
