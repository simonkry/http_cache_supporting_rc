#pragma once

#include "envoy/http/header_map.h"
#include "envoy/http/metadata_interface.h"
#include "envoy/buffer/buffer.h"

constexpr size_t LINEAR_PROBING_STEP = 17;

/***********************************************************************************************************************
 * INSPIRATION TAKEN FROM: https://embeddedartistry.com/blog/2017/05/17/creating-a-circular-buffer-in-c-and-c/
 ***********************************************************************************************************************/

namespace Envoy::Http {

/**
 * @brief States of a slot in ring buffer cache.
 * EMPTY
 * OCCUPIED
 * TOMBSTONE == deleted element
 */
enum class HashTableSlotState { EMPTY, OCCUPIED, TOMBSTONE };

using InstanceOptRef = OptRef<Buffer::Instance>;
using MetadataMapOptRef = OptRef<MetadataMap>;

/**
 * @brief Class to wrap up cache key, response attributes and slot state.
 */
class HashTableSlot {
public:
    // host_url_ used as key in the cache
    std::string host_url_;
    ResponseHeaderMapOptRef headers_;
    InstanceOptRef data_;
    ResponseTrailerMapOptRef trailers_;
    MetadataMapOptRef metadata_map_;
    HashTableSlotState state_ = HashTableSlotState::EMPTY;
};

/**
 * @brief Implements methods used by ring buffer cache - insertion, search, etc.
 * The buffer memory is allocated only on construction with a size_t value. De-allocation happens thanks to std::unique_ptr.
 * Keeps track of number of elements - size_.
 */
class RingBufferHTTPCache {
public:
    RingBufferHTTPCache() = default;
    explicit RingBufferHTTPCache(size_t capacity);
    ~RingBufferHTTPCache() = default;
    RingBufferHTTPCache(const RingBufferHTTPCache & other);
    RingBufferHTTPCache & operator=(RingBufferHTTPCache other);
    bool insert(const HashTableSlot & entry);
    std::optional<HashTableSlot> at(const std::string_view & hostUrl);
    void reset();
    bool empty();
    bool full();
    size_t capacity() const;
    size_t size();

private:
    // This std::mutex might not be necessary since one is already used in RingBufferHTTPCacheFactory class
    std::mutex mtx_ {};
    std::unique_ptr<HashTableSlot[]> buffer_ {};
    const size_t capacity_ {};
    size_t size_ = 0;
};

/**
 * @brief Creates new ring buffer cache if the previous one is full.
 * Stores caches in std::vector.
 */
class RingBufferHTTPCacheFactory {
public:
    explicit RingBufferHTTPCacheFactory(size_t cacheCapacity);
    void insert(const HashTableSlot & entry);
    std::optional<HashTableSlot> at(const std::string_view & hostUrl);
    std::vector<RingBufferHTTPCache> & getCaches() { return caches_; }
private:
    std::mutex mtx_ {};
    std::vector<RingBufferHTTPCache> caches_ {};
    size_t caches_count_ = 1;
    const size_t cache_capacity_ {};
};

} // namespace Envoy::Http
