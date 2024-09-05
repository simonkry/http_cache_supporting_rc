#include "ring_buffer_cache.h"

namespace Envoy::Http {

RingBufferHTTPCache::RingBufferHTTPCache(uint32_t capacity) :
        capacity_(capacity),
        buffer_(std::make_unique<HashTableSlot[]>(capacity)) {}

RingBufferHTTPCache::RingBufferHTTPCache(const RingBufferHTTPCache & other) :
        capacity_(other.capacity_),
        buffer_(std::make_unique<HashTableSlot[]>(other.capacity_)),
        size_(other.size_) {
    std::copy(other.buffer_.get(), other.buffer_.get() + other.size_, buffer_.get());
}

RingBufferHTTPCache & RingBufferHTTPCache::operator=(RingBufferHTTPCache other) {
    std::swap(buffer_, other.buffer_);
    std::swap(size_, other.size_);
    return *this;
}

bool RingBufferHTTPCache::insert(const HashTableSlot& entry) {
    if ( full() ) return false;
    // Calculate cache key
    uint32_t hashValue = std::hash<std::string>()(entry.host_url_) % capacity_;
    uint32_t initialHashValue = hashValue;
    ENVOY_LOG(trace, "[RingBufferHTTPCache::insert] capacity_: {}", capacity_);
    ENVOY_LOG(trace, "[RingBufferHTTPCache::insert] hashValue: {}", hashValue);
    // Using hash function with linear probing
    while ( buffer_[hashValue].state_ == HashTableSlotState::OCCUPIED ) {
        hashValue = (hashValue + LINEAR_PROBING_STEP) % capacity_;
        // Check cycle
        if (hashValue == initialHashValue) return false;
    }
    buffer_[hashValue] = entry;
    ++size_;
    return true;
}

std::optional<HashTableSlot> RingBufferHTTPCache::at(const std::string & hostUrl) const {
    // Calculate cache key
    uint32_t hashValue = std::hash<std::string>()(hostUrl) % capacity_;
    uint32_t initialHashValue = hashValue;
    ENVOY_LOG(trace, "[RingBufferHTTPCache::at] capacity_: {}", capacity_);
    ENVOY_LOG(trace, "[RingBufferHTTPCache::at] hashValue: {}", hashValue);
    // Using hash function with linear probing
    while ( buffer_[hashValue].state_ != HashTableSlotState::EMPTY ) {
        if ( buffer_[hashValue].state_ == HashTableSlotState::OCCUPIED &&
             buffer_[hashValue].host_url_ == hostUrl ) {
            return buffer_[hashValue];
        }
        hashValue = (hashValue + LINEAR_PROBING_STEP) % capacity_;
        // Check cycle - mismatch in the whole buffer
        if (hashValue == initialHashValue) return std::nullopt;
    }
    return std::nullopt;
}

void RingBufferHTTPCache::reset() {
    buffer_ = nullptr;
    buffer_ = std::make_unique<HashTableSlot[]>(capacity_);
}

bool RingBufferHTTPCache::empty() const {
    return size_ == 0;
}

bool RingBufferHTTPCache::full() const {
    return size_ == capacity_;
}

uint32_t RingBufferHTTPCache::capacity() const {
    return capacity_;
}

uint32_t RingBufferHTTPCache::size() const {
    return size_;
}


void RingBufferHTTPCacheFactory::initialize(uint32_t singleCacheCapacity) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (cache_count_ > 0) return;
    single_cache_capacity_ = singleCacheCapacity;
    caches_.emplace_back(single_cache_capacity_);
    cache_count_ = 1;
}

void RingBufferHTTPCacheFactory::insert(const HashTableSlot& entry) {
    std::lock_guard<std::mutex> lock(mtx_);
    // Linear search O(n) in vector - a place for improvement
    for (auto & cache : caches_) {
        if (cache.insert(entry)) return;
    }
    // Following the assignment: The number of ring buffers is not limited, therefore I didn't implement
    // replacement (and deletion) in a ring buffer cache
    caches_.emplace_back(single_cache_capacity_);
    ++cache_count_;
    caches_.back().insert(entry);
}

std::optional<HashTableSlot> RingBufferHTTPCacheFactory::at(const std::string & hostUrl) {
    std::optional<HashTableSlot> responseEntry;
    std::lock_guard<std::mutex> lock(mtx_);
    // Linear search O(n) in vector - a place for improvement
    for (const auto & cache : caches_) {
        responseEntry = cache.at(hostUrl);
        if (responseEntry != std::nullopt) return responseEntry;
    }
    return std::nullopt;
}

const std::vector<RingBufferHTTPCache> & RingBufferHTTPCacheFactory::getCaches() {
    std::lock_guard<std::mutex> lock(mtx_);
    return caches_;
}

uint32_t RingBufferHTTPCacheFactory::getCacheCount() {
    std::lock_guard<std::mutex> lock(mtx_);
    return cache_count_;
}

} // namespace Envoy::Http
