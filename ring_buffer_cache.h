// TODO(simonkry): fixes and tidying up; better hash key(whole headers), create separate file(s), RC

#pragma once

#include "hash_table_slot.h"

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
    ~RingBufferHTTPCache() = default;
    RingBufferHTTPCache(const RingBufferHTTPCache & other);
    RingBufferHTTPCache & operator=(RingBufferHTTPCache other);
    bool insert(const HashTableSlot& entry);
    std::optional<HashTableSlot> at(const std::string & hostUrl) const;
    void reset();
    bool empty() const;
    bool full() const;
    uint32_t capacity() const;
    uint32_t size() const;

private:
    const uint32_t capacity_ {};
    std::unique_ptr<HashTableSlot[]> buffer_ {};
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
    void insert(const HashTableSlot& entry);
    // No const methods because of locking std::mutex
    std::optional<HashTableSlot> at(const std::string & hostUrl);
    const std::vector<RingBufferHTTPCache> & getCaches();
    uint32_t getCacheCount();

private:
    std::mutex mtx_ {};
    uint32_t single_cache_capacity_ {};
    std::vector<RingBufferHTTPCache> caches_ {};
    uint32_t cache_count_ = 0;
};

} // namespace Envoy::Http
